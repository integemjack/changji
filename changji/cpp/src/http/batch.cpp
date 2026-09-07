#include "http/batch.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "http/scripting.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "stages/bible.hpp"
#include "stages/script.hpp"
#include "stages/storyboard.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

void forbid_extra(const json& body, const std::set<std::string>& allowed) {
    if (!body.is_object()) throw ApiError(400, "请求体要是一个对象");
    for (const auto& kv : body.items()) {
        if (allowed.count(kv.key()) == 0) {
            throw unprocessable_top(kv.key(), "Extra inputs are not permitted",
                                    kv.value(), "extra_forbidden");
        }
    }
}

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

double num_in_range(const json& body, const char* key, double def, double gt,
                    double le) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_number()) {
        throw unprocessable_top(key, "Input should be a valid number", *it,
                                "float_type");
    }
    const double v = it->get<double>();
    if (v <= gt) {
        throw unprocessable_top(
            key, "Input should be greater than " + std::to_string(gt), *it,
            "greater_than");
    }
    if (v > le) {
        throw unprocessable_top(
            key, "Input should be less than or equal to " + std::to_string(le),
            *it, "less_than_equal");
    }
    return v;
}

int int_in_range(const json& body, const char* key, int def, int ge, int le) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_number_integer()) {
        throw unprocessable_top(key, "Input should be a valid integer", *it,
                                "int_type");
    }
    const int v = it->get<int>();
    if (v < ge) {
        throw unprocessable_top(
            key, "Input should be greater than or equal to " + std::to_string(ge),
            *it, "greater_than_equal");
    }
    if (v > le) {
        throw unprocessable_top(
            key, "Input should be less than or equal to " + std::to_string(le),
            *it, "less_than_equal");
    }
    return v;
}

ProjectStore open_project(const json& body) {
    const std::string path = need_str(body, "project");
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

/// 下一个没被占用的剧集编号。量产时不该逼用户自己想 id。
///
/// **只数 epNN 那些。** 预告片挂在 trailer 上，把它也数进去的话，
/// 有了预告之后新建的第二集会跳号变成 ep03。
std::string next_episode_id(const Project& project) {
    std::set<std::string> existing;
    int n = 1;
    for (const auto& ep : project.episodes) {
        existing.insert(ep.episode_id);
        // 对应 re.fullmatch(r"ep\d+", ...)
        if (ep.episode_id.size() > 2 && ep.episode_id.compare(0, 2, "ep") == 0) {
            bool all_digits = true;
            for (std::size_t i = 2; i < ep.episode_id.size(); ++i) {
                if (ep.episode_id[i] < '0' || ep.episode_id[i] > '9') {
                    all_digits = false;
                    break;
                }
            }
            if (all_digits) ++n;
        }
    }
    const auto fmt = [](int k) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "ep%02d", k);
        return std::string(buf);
    };
    while (existing.count(fmt(n))) ++n;
    return fmt(n);
}

std::vector<std::string> character_names(const AssetLibrary& assets) {
    std::vector<std::string> names;
    for (const auto& kv : assets.characters) names.push_back(kv.second.name);
    return names;
}

/// 前几集当上下文。只取最近三集——整季塞进去小模型撑不住，
/// 离得远的剧情对下一集的连贯性也没什么帮助。
std::string previous_context(const Project& project) {
    std::vector<std::string> written;
    for (const auto& ep : project.episodes) {
        const std::string s = text::strip_ws(ep.script);
        if (s.empty()) continue;
        if (ep.episode_id == kTrailerEpisodeId) continue;
        written.push_back("【" + ep.episode_id + "】\n" + s);
    }
    const std::size_t skip = written.size() > 3 ? written.size() - 3 : 0;
    std::string out;
    for (std::size_t i = skip; i < written.size(); ++i) {
        if (i > skip) out += "\n\n";
        out += written[i];
    }
    return out;
}

double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

}  // namespace

ApiResult post_script_series(const json& body,
                             std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "premise", "episodes", "duration_s",
                        "reuse_characters"});
    const std::string premise = need_str(body, "premise");
    const int episodes = int_in_range(body, "episodes", 3, 1, 20);
    const double duration_s = num_in_range(body, "duration_s", 60.0, 0.0, 1800.0);
    const bool reuse_chars = opt_bool(body, "reuse_characters", true);

    // 409 要在**建 store 之前**判断吗？不。Python 那边是先判 running
    // 再 load_project，所以"已经在写了"优先于"项目不存在"。照抄这个顺序：
    // 两个都错时回哪一个是可观测的。
    if (pipeline::jobs().running(pipeline::JobKind::Write)) {
        throw ApiError(409, "已经在写了");
    }
    ProjectStore store = open_project(body);
    load_or_400(store);   // 只为了验证项目能读，结果不用

    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, premise, episodes, duration_s,
         reuse_chars](pipeline::JobProgress& p) {
            p.set_total(episodes);

            // 梗概先存下来。下次打开界面时回填，不用凭记忆重打。
            Project first = store.load_project();
            const std::string trimmed = text::strip_ws(premise);
            if (first.premise != trimmed) {
                first.premise = text::truncate_utf8(trimmed, 2000);
                store.save_project(first);
            }

            int done = 0;
            for (int i = 0; i < episodes; ++i) {
                if (p.cancelled()) return;

                Project project = store.load_project();
                const AssetLibrary assets = store.load_assets();
                const std::vector<std::string> names =
                    reuse_chars ? character_names(assets)
                                : std::vector<std::string>{};

                p.set_message("正在写第 " + std::to_string(i + 1) + " 集");

                llm::Request req;
                req.prompt = stages::build_script_prompt(
                    premise, duration_s, project.style_line,
                    previous_context(project), names);
                req.schema = stages::script_schema();
                req.schema_name = "script";

                stages::ScriptDraft draft;
                try {
                    pipeline::CancelToken dummy;
                    draft = stages::parse_script(client->complete(req, dummy));
                } catch (const std::exception& e) {
                    // 一集写砸了不该让前面几集白写，记下来接着往下写。
                    // episode_id 留空——这一集根本没建出来。
                    p.add_episode(json{{"episode_id", ""}, {"error", e.what()}});
                    p.set_done(++done);
                    continue;
                }

                const std::string episode_id = next_episode_id(project);
                Episode ep;
                ep.episode_id = episode_id;
                ep.title = draft.title;
                ep.synopsis = draft.logline;
                ep.script = draft.render();
                ep.target_duration_s = duration_s;
                project.episodes.push_back(std::move(ep));
                store.save_project(project);

                p.add_episode(json{
                    {"episode_id", episode_id},
                    {"title", draft.title},
                    {"logline", draft.logline},
                    {"speakers", draft.speakers()},
                    {"dialogue_chars", draft.dialogue_chars()},
                });
                p.set_done(++done);
            }
            p.set_message("写完了 " + std::to_string(done) + " 集");
        },
        "已手动停止。已经写好的几集留着。");

    if (!started) throw ApiError(409, "已经在写了");
    return {200, {{"started", true}, {"total", episodes}}};
}

ApiResult post_plan_all(const json& body, std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "overwrite"});
    const bool overwrite = opt_bool(body, "overwrite", false);

    if (pipeline::jobs().running(pipeline::JobKind::Write)) {
        // 和写整季不是同一句话。用户看到"已经在写了"会去找哪里在写剧本，
        // 而实际情况是那个槽被别的事占着。
        throw ApiError(409, "剧本那边还在忙");
    }
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);

    std::vector<std::string> todo;
    for (const auto& ep : project.episodes) {
        if (text::strip_ws(ep.script).empty()) continue;
        if (!overwrite && !ep.shots.empty()) continue;
        todo.push_back(ep.episode_id);
    }
    if (todo.empty()) throw ApiError(400, "没有需要出分镜的剧集。有剧本又没分镜的才算");

    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, todo](pipeline::JobProgress& p) {
            p.set_total(static_cast<int>(todo.size()));
            int done = 0;
            for (const std::string& episode_id : todo) {
                if (p.cancelled()) return;
                p.set_message("正在给 " + episode_id + " 出分镜");

                Project project = store.load_project();
                AssetLibrary assets = store.load_assets();
                Episode* ep = project.episode_by_id(episode_id);
                if (ep == nullptr) continue;

                try {
                    pipeline::CancelToken dummy;
                    // 角色设定全剧共用，第一次缺的时候补一次就够。
                    // 每集都重出的话，同一个角色前后长得不一样。
                    if (assets.characters.empty()) {
                        llm::Request breq;
                        breq.prompt = stages::build_bible_prompt(
                            ep->script, project.style_line);
                        breq.schema = stages::bible_schema();
                        breq.schema_name = "bible";
                        assets = stages::parse_bible(client->complete(breq, dummy),
                                                     project.style_line);
                        store.save_assets(assets);
                    }

                    llm::Request sreq;
                    sreq.prompt = stages::build_storyboard_prompt(
                        ep->script, assets,
                        stages::DurationQuota::for_duration(ep->target_duration_s),
                        episode_id);
                    sreq.schema = stages::llm_shot_schema(assets);
                    sreq.schema_name = "storyboard";

                    std::vector<Shot> shots = stages::parse_storyboard(
                        client->complete(sreq, dummy), assets);
                    const auto gaps = stages::check_coverage(ep->script, shots);
                    if (!gaps.empty()) {
                        std::string msg = "分镜表不完整：";
                        for (const auto& g : gaps) msg += "\n" + g;
                        msg += "\n\n换一个更强的模型，或者手工补齐这些字段后再跑。";
                        throw stages::StoryboardError(msg);
                    }
                    apply_lipsync_rules(shots);
                    ep->shots = std::move(shots);
                    store.save_project(project);
                } catch (const std::exception& e) {
                    // 一集出错不拖垮后面几集。跑一晚上，早上发现第二集挂了
                    // 导致后面十集都没动，那这一晚上就白熬了。
                    p.add_episode(
                        json{{"episode_id", episode_id}, {"error", e.what()}});
                    p.set_done(++done);
                    continue;
                }

                double total = 0.0;
                for (const Shot& s : ep->shots) total += s.duration_s;
                p.add_episode(json{{"episode_id", episode_id},
                                   {"title", ep->title},
                                   {"shots", ep->shots.size()},
                                   {"duration_s", round1(total)}});
                p.set_done(++done);
            }
            p.set_message("出完了 " + std::to_string(done) + " 集的分镜");
        },
        "已手动停止。已经出好的分镜留着。");

    if (!started) throw ApiError(409, "剧本那边还在忙");
    return {200, {{"started", true}, {"episodes", todo}}};
}

}  // namespace changji::http
