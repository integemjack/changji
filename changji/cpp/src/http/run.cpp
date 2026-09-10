#include "http/run.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config/runtime.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "util/fs_time.hpp"
#include "util/human_time.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

std::string need_str(const json& body, const char* key) {
    const auto it = body.find(key);
    if (it == body.end()) {
        // **input 是整个请求体，不是 null。** FastAPI 报缺字段时把父对象
        // 放进 input，前端拿它回显"你提交的是这些"。写 null 的话那一栏是空的。
        // 实时对拍抓出来的（写接口那一轮）。
        throw unprocessable_top(key, "Field required", body, "missing");
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

/// Python 的 round()：银行家舍入。
double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

ProjectStore open_project(const std::string& path) {
    if (path.empty()) throw ApiError(400, "没有指定项目目录");
    return ProjectStore(paths::from_utf8(path));
}

Project load_or_400(const ProjectStore& store) {
    try {
        return store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
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

ApiResult post_run(const json& body, const RunDeps& deps) {
    if (!body.is_object()) throw ApiError(400, "请求体要是一个对象");

    // **不查多余的键。** RunRequest 是个普通的 BaseModel，
    // pydantic 默认忽略多余字段。这里 forbid 的话，前端多传一个键就 422，
    // 而那些键现在就在传（前端和后端的版本不一定同步升）。
    const std::string project_path = need_str(body, "project");
    const std::string episode_id = need_str(body, "episode_id");
    const bool skip_final = opt_bool(body, "skip_final", false);
    // **默认跳过。** 挂 Turbo LoRA 之后两档画质拉不开差距，草稿档就是
    // 白跑一遍。想留着的话显式传 skip_draft: false。
    const bool skip_draft = opt_bool(body, "skip_draft", true);
    // 只跑这几个镜头。**空表示整集**——新界面上每个镜头自己有一个
    // "重新生成"，用户看着某一镜不对，想重跑的就是那一个；
    // 没有这一项的话只能整集重跑，而整集是一小时。
    std::set<std::string> only_shots;
    if (const auto it = body.find("shot_ids");
        it != body.end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_string()) only_shots.insert(v.get<std::string>());
        }
    }
    const bool force = opt_bool(body, "force", false);
    const bool all_episodes = opt_bool(body, "all_episodes", false);
    const auto stage_names = opt_str_list(body, "stages");
    // **C++ 独有。** "episode"（默认）= 一集跑完再跑下一集，Python 就这样；
    // "stage" = 所有集先配音，再所有集出首帧……多卡时用这个，见任务体里的注释。
    std::string order = "episode";
    if (const auto it = body.find("order"); it != body.end() && it->is_string()) {
        order = it->get<std::string>();
    }
    if (order != "episode" && order != "stage") {
        throw ApiError(400, "order 只能是 episode 或 stage");
    }

    // 409 在读项目之前判。两个都错时回哪一个是可观测的，照抄 Python 的顺序。
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, "已经在跑 " + running_episode() + " 了");
    }
    const ProjectStore store = open_project(project_path);
    const Project project = load_or_400(store);

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
        [store, queue, skip_final, skip_draft, only_shots, force, stage_names,
         order,
         deps](pipeline::JobProgress& p) {
            // 配置和后端在**任务开始时**取一次，不是注册时。
            // 用户改完模型文件不用重启，但一次跑的中途不会换——
            // 中途换的话同一集里前半段和后半段用的是不同的模型。
            config::Settings settings = deps.settings();
            HardwareProfile profile = deps.profile();

            // **项目自己的 changji.toml 盖在全局上。** 画幅和清晰度
            // （[video]）写在那儿：一台机器上可以同时有竖屏短剧和横屏
            // 片子，画幅是这部剧的属性。
            //
            // 只在这一层合，不在 deps.settings() 里——那个函数不知道
            // 当前跑的是哪个项目，而同一个进程会轮流跑好几个。
            settings = config::load_settings(store.root());
            const auto [vw, vh] = settings.video.size();
            // 档位表是按显存推的，画幅定下来之后直接盖掉成片档的宽高。
            // **步数不动**：那是速度和画质的权衡，和画幅无关。
            if (auto it = profile.tiers.find(models::Tier::FINAL);
                it != profile.tiers.end()) {
                it->second.width = vw;
                it->second.height = vh;

                // **挂了 Turbo 就按 6 步走。**
                //
                // 档位表里的步数是按显存推的，那套数字假设的是不带蒸馏
                // LoRA 的模型（28 步）。挂着 Turbo 还跑 28 步不只是慢：
                // 资料和实测都说超过 8 步就开始过锐，画面反而变差。
                //
                // 只在用户**没有显式指定**步数时才动（[tiers].final_steps
                // 是 0 就算没指定）——显式填了 12 的人是有意的，
                // 我们不该替他改。
                const auto lora = settings.models.resolve(
                    settings.models.video_lora, settings.workspace_path());
                std::error_code ec;
                const bool turbo = !settings.models.video_lora.empty() &&
                                   std::filesystem::is_regular_file(lora, ec);
                if (turbo && settings.tiers.final_steps == 0) {
                    it->second.steps = 6;
                }
            }
            const pipeline::Backends backends = deps.backends(settings, store);

            // 阶段名的校验在这里，不在上面的路由里：
            // Python 那边它在 run_stages 内部，错误落进任务状态
            // 而不是变成 400。见 parse_stages 的注释。
            std::optional<std::vector<pipeline::Stage>> only;
            if (stage_names.has_value() && !stage_names->empty()) {
                try {
                    only = parse_stages(*stage_names);
                } catch (const std::exception& e) {
                    p.set_error(e.what());
                    return;
                }
            }

            std::vector<std::string> errors;
            // 跑一集的某几个阶段。出错记下来，不拖垮后面的。
            const auto run_one = [&](const std::string& id,
                                     std::optional<std::vector<pipeline::Stage>> which) {
                p.set_episode_id(id);
                try {
                    pipeline::RunOptions opts;
                    opts.episode_id = id;
                    opts.skip_final = skip_final;
                    opts.skip_draft = skip_draft;
                    opts.only_shots = only_shots;
                    opts.force = force;
                    opts.only = std::move(which);
                    const auto report = pipeline::run_episode(
                        store, profile, settings, opts, backends, p, p.token());
                    if (!report.errors.empty()) {
                        errors.push_back(id + "：" + join(report.errors, "；"));
                    }
                } catch (const std::exception& e) {
                    errors.push_back(std::string(id) + "：" + e.what());
                }
            };

            if (order == "stage") {
                // **按阶段排。** 所有集先配音，再所有集出首帧……
                //
                // 为什么多卡时要这样：按集排的话每一集都要经历一次配音
                // （串行、协调者那张卡）→ 首帧 → 草稿 → 成片 → 装配（CPU），
                // 其间八个工作进程反复空转；每个阶段结尾都有一条"等最慢那镜"
                // 的尾巴；每换一个阶段所有工作进程都要换一次模型。十集就是
                // 十遍。按阶段排，尾巴从十条变一条，模型每个阶段只换一次。
                //
                // 阶段内部的代码一行不动：每个阶段就是 run_episode 带
                // only={那一个阶段} 跑一遍，挑镜头仍然按状态来，断点续跑照旧。
                std::vector<pipeline::Stage> stages = {
                    pipeline::Stage::Audio, pipeline::Stage::Frames,
                    pipeline::Stage::Draft, pipeline::Stage::Final,
                    pipeline::Stage::Assemble};
                if (only.has_value()) {
                    // 用户只要某几个阶段：保留顺序，只留要的
                    std::vector<pipeline::Stage> kept;
                    for (auto st : stages) {
                        if (std::find(only->begin(), only->end(), st) != only->end()) {
                            kept.push_back(st);
                        }
                    }
                    stages = std::move(kept);
                }
                if (skip_final) {
                    stages.erase(std::remove(stages.begin(), stages.end(),
                                             pipeline::Stage::Final),
                                 stages.end());
                }
                if (skip_draft) {
                    stages.erase(std::remove(stages.begin(), stages.end(),
                                             pipeline::Stage::Draft),
                                 stages.end());
                }
                const int total = static_cast<int>(stages.size() * queue.size());
                p.set_queue(0, total);
                int done = 0;
                for (const auto st : stages) {
                    for (const auto& id : queue) {
                        if (p.cancelled()) return;
                        run_one(id, std::vector<pipeline::Stage>{st});
                        p.set_queue(++done, total);
                    }
                }
            } else {
                p.set_queue(0, static_cast<int>(queue.size()));
                int done = 0;
                for (const auto& id : queue) {
                    if (p.cancelled()) return;
                    run_one(id, only);
                    p.set_queue(++done, static_cast<int>(queue.size()));
                }
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


ApiResult get_run_preview(const std::string& path,
                          const std::string& episode_id, bool all_episodes,
                          bool skip_final, bool force,
                          const HardwareProfile& profile) {
    const ProjectStore store = open_project(path);
    const Project project = load_or_400(store);

    std::vector<const Episode*> episodes;
    if (all_episodes) {
        for (const auto& e : project.episodes) {
            if (!e.shots.empty()) episodes.push_back(&e);
        }
    } else if (const Episode* ep = project.episode_by_id(episode_id)) {
        episodes.push_back(ep);
    }
    if (episodes.empty()) throw ApiError(400, "没有可跑的剧集，先出分镜");

    // 阶段的先后。**一个镜头从它现在的状态开始，会一路走完后面所有阶段。**
    static const char* kOrder[] = {"audio", "frames", "draft", "final"};
    static const char* kLabels[] = {"配音", "首帧", "草稿档", "成片档"};
    constexpr int kCount = 4;

    // 每个状态从第几个阶段开始。不在表里的（FINAL_DONE、FALLBACK）这一次不动。
    const auto entry_of = [](ShotStatus s) -> int {
        switch (s) {
            case ShotStatus::PLANNED:        return 0;
            case ShotStatus::AUDIO_DONE:     return 1;
            case ShotStatus::FRAME_DONE:     return 2;
            case ShotStatus::DRAFT_REJECTED: return 2;
            case ShotStatus::DRAFT_DONE:     return 3;
            case ShotStatus::FINAL_REJECTED: return 3;
            default:                         return -1;
        }
    };

    int counts[kCount] = {0, 0, 0, 0};
    int total_shots = 0;
    for (const Episode* ep : episodes) {
        total_shots += static_cast<int>(ep->shots.size());
        for (const auto& shot : ep->shots) {
            // 人工确认过的镜头不重跑，除非明确要求。
            if (shot.status == ShotStatus::LOCKED && !force) continue;
            const int start_at = force ? 0 : entry_of(shot.status);
            if (start_at < 0) continue;
            for (int i = start_at; i < kCount; ++i) {
                if (skip_final && i == 3) continue;
                ++counts[i];
            }
        }
    }

    double seconds = 0.0;
    for (int i = 2; i < kCount; ++i) {
        if (counts[i] == 0) continue;
        const Tier tier = i == 2 ? Tier::DRAFT : Tier::FINAL;
        if (const auto est = profile.estimate_episode(counts[i], tier)) {
            seconds += *est;
        }
    }
    // 配音和首帧比渲染快得多，按经验各给一点，别报一个明显偏小的数。
    seconds += counts[0] * 8.0 + counts[1] * 12.0;

    json stages = json::array();
    bool any = false;
    for (int i = 0; i < kCount; ++i) {
        if (counts[i] == 0) continue;
        any = true;
        stages.push_back({{"stage", kOrder[i]},
                          {"label", kLabels[i]},
                          {"shots", counts[i]}});
    }

    json ids = json::array();
    for (const Episode* ep : episodes) ids.push_back(ep->episode_id);

    return {200, {
        {"episodes", ids},
        {"shots", total_shots},
        {"stages", stages},
        {"idle", !any},
        // Python 的 round() 回的是 int，不是保留零位小数的浮点。
        // 回成 12.0 的话前端拿到的是 "12" 还是 "12.0" 取决于序列化，
        // 而那个数会直接拼进句子里。
        {"estimate_s", static_cast<long long>(std::nearbyint(seconds))},
        {"estimate_text", seconds != 0.0 ? util::human_time(seconds)
                                         : std::string()},
    }};
}

ApiResult get_outputs(const std::string& path) {
    const ProjectStore store = open_project(path);
    const fs::path dir = store.paths().output();

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {200, {{"files", json::array()}}};

    struct Item {
        json body;
        double mtime = 0.0;
    };
    std::vector<Item> items;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        if (!f.is_regular_file(ec)) continue;
        // 扩展名不区分大小写：Python 在 Windows 上用的 pathlib.glob
        // 就是不区分的，同一个目录在两个后端下不该列出不同的文件。
        std::string ext = paths::to_utf8(f.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".mp4") continue;

        const double mtime = util::file_mtime_unix(f.path());
        const auto size = static_cast<double>(fs::file_size(f.path(), ec));
        items.push_back({json{
            {"name", paths::to_utf8(f.path().filename())},
            {"rel", store.paths().rel(f.path())},
            {"size_mb", round1(size / (1024.0 * 1024.0))},
            {"mtime", static_cast<long long>(mtime)},
        }, mtime});
    }

    // 新的在前。stable_sort 对齐 Python 的 sorted()——时间相同时保持
    // 目录遍历的顺序，不然同一秒里出的两个片子每次刷新都换位置。
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.mtime > b.mtime; });

    json files = json::array();
    for (auto& it : items) files.push_back(std::move(it.body));
    return {200, {{"files", files}}};
}

}  // namespace changji::http
