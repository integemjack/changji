#include "http/run.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config/runtime.hpp"
#include "infer/sd_image.hpp"
#include "infer/sd_video.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "stages/frames.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

std::string need_str(const json& body, const char* key) {
    const auto it = body.find(key);
    if (it == body.end()) {
        throw unprocessable_top(key, "Field required", nullptr, "missing");
    }
    if (!it->is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", *it,
                                "string_type");
    }
    return it->get<std::string>();
}

bool opt_bool(const json& body, const char* key, bool def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

/// 取一个字符串数组。**分得清"没给"和"给了个空的"**——
/// 这两者在这个接口上语义不同，见 RunOptions::only 的注释。
std::optional<std::vector<std::string>> opt_str_list(const json& body,
                                                     const char* key) {
    const auto it = body.find(key);
    if (it == body.end() || it->is_null()) return std::nullopt;
    if (!it->is_array()) {
        throw unprocessable_top(key, "Input should be a valid list", *it,
                                "list_type");
    }
    std::vector<std::string> out;
    for (const auto& v : *it) {
        if (!v.is_string()) {
            throw unprocessable_top(key, "Input should be a valid string", v,
                                    "string_type");
        }
        out.push_back(v.get<std::string>());
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

/// 正在跑的是哪一集。拼 409 那句话用。
std::string running_episode() {
    const json snap = pipeline::jobs().snapshot(pipeline::JobKind::Run);
    const auto it = snap.find("episode_id");
    if (it == snap.end() || !it->is_string()) return "None";  // Python f-string 里 None 就是这么印的
    return it->get<std::string>();
}

}  // namespace

std::vector<pipeline::Stage> parse_stages(
    const std::vector<std::string>& names) {
    std::vector<pipeline::Stage> out;
    std::vector<std::string> unknown;
    for (const auto& raw : names) {
        const std::string name = text::strip_ws(raw);
        if (name.empty()) continue;   // Python 那边先 strip 再滤空
        pipeline::Stage s{};
        if (pipeline::stage_from_string(name, s)) {
            out.push_back(s);
        } else {
            unknown.push_back(name);
        }
    }
    if (!unknown.empty()) {
        // 可选项按字典序列出，对齐 Python 的 sorted(known)。
        throw ApiError(400, "不认识的阶段 " + join(unknown, "、") +
                                "。可选：assemble、audio、draft、final、frames");
    }
    return out;
}

RunDeps default_run_deps() {
    RunDeps d;
    d.settings = [] { return config::runtime().snapshot(); };
    d.profile = [] { return config::runtime().profile(); };
    d.backends = [](const config::Settings& s) {
        pipeline::Backends b;
        b.frame = stages::sd_renderer();
        b.video = infer::sd_video_renderer(s);
        return b;
    };
    return d;
}

ApiResult post_run(const json& body, const RunDeps& deps) {
    if (!body.is_object()) throw ApiError(400, "请求体要是一个对象");

    // **不查多余的键。** RunRequest 是个普通的 BaseModel，
    // pydantic 默认忽略多余字段。这里 forbid 的话，前端多传一个键就 422，
    // 而那些键现在就在传（前端和后端的版本不一定同步升）。
    const std::string project_path = need_str(body, "project");
    const std::string episode_id = need_str(body, "episode_id");
    const bool skip_final = opt_bool(body, "skip_final", false);
    const bool force = opt_bool(body, "force", false);
    const bool all_episodes = opt_bool(body, "all_episodes", false);
    const auto stage_names = opt_str_list(body, "stages");

    // 409 在读项目之前判。两个都错时回哪一个是可观测的，照抄 Python 的顺序。
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, "已经在跑 " + running_episode() + " 了");
    }
    if (project_path.empty()) throw ApiError(400, "没有指定项目目录");

    const ProjectStore store(paths::from_utf8(project_path));
    Project project;
    try {
        project = store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }

    std::vector<std::string> queue;
    if (all_episodes) {
        // 做的就是量产，一集一集手点没有意义。给了这个就忽略 episode_id。
        for (const auto& ep : project.episodes) {
            if (!ep.shots.empty()) queue.push_back(ep.episode_id);
        }
        if (queue.empty()) throw ApiError(400, "这个项目还没有任何一集有分镜表");
    } else {
        queue.push_back(episode_id);
    }

    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Run, queue[0],
        [store, queue, skip_final, force, stage_names,
         deps](pipeline::JobProgress& p) {
            p.set_queue(0, static_cast<int>(queue.size()));

            // 配置和后端在**任务开始时**取一次，不是注册时。
            // 用户改完模型文件不用重启，但一次跑的中途不会换——
            // 中途换的话同一集里前半段和后半段用的是不同的模型。
            const config::Settings settings = deps.settings();
            const HardwareProfile profile = deps.profile();
            const pipeline::Backends backends = deps.backends(settings);

            std::vector<std::string> errors;
            int done = 0;
            for (const auto& id : queue) {
                if (p.cancelled()) return;
                p.set_episode_id(id);
                try {
                    pipeline::RunOptions opts;
                    opts.episode_id = id;
                    opts.skip_final = skip_final;
                    opts.force = force;
                    // 阶段名的校验在这里，不在上面的路由里：
                    // Python 那边它在 run_stages 内部，错误落进任务状态
                    // 而不是变成 400。见 parse_stages 的注释。
                    if (stage_names.has_value() && !stage_names->empty()) {
                        opts.only = parse_stages(*stage_names);
                    }

                    const auto report = pipeline::run_episode(
                        store, profile, opts, backends, p, p.token());
                    if (!report.errors.empty()) {
                        errors.push_back(id + "：" + join(report.errors, "；"));
                    }
                } catch (const std::exception& e) {
                    // 一集出错不拖垮后面几集。
                    errors.push_back(std::string(id) + "：" + e.what());
                }
                p.set_queue(++done, static_cast<int>(queue.size()));
            }
            if (!errors.empty()) p.set_error(join(errors, "；"));
        });

    // start() 只在同种任务已经在跑时返回 false，而上面刚判过。
    // 还是要判：那两步之间没有锁，两个请求同时进来时后一个要拿到 409，
    // 不能两条线程一起写同一个槽。
    if (!started) {
        throw ApiError(409, "已经在跑 " + running_episode() + " 了");
    }
    return {200, {{"started", true}, {"queue", queue}}};
}

}  // namespace changji::http
