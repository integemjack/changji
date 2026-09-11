#include "http/batch.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "http/episodes.hpp"
#include "http/scripting.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "stages/bible.hpp"
#include "models/story.hpp"
#include "http/ws.hpp"
#include "stages/chapter_write.hpp"
#include "stages/json_stream.hpp"
#include "stages/script.hpp"
#include "stages/story_plan.hpp"
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

double num_in_range(const json& body, const char* key, double def, double gt,
                    double le) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_number()) {
        throw unprocessable_top(key, "Input should be a valid number", *it,
                                "float_type");
    }
    const double v = it->get<double>();
    // 边界值用 bound_text 印，不用 std::to_string——后者给六位小数，
    // 1800.0 会变成 "1800.000000"，而 pydantic 的消息里是 "1800"。
    // 而且要带 ctx，前端靠它填出"最大 1800"这种中文提示。
    if (v <= gt) {
        throw out_of_range(key, "Input should be greater than " + bound_text(gt),
                           *it, "greater_than", "gt", gt);
    }
    if (v > le) {
        throw out_of_range(
            key, "Input should be less than or equal to " + bound_text(le), *it,
            "less_than_equal", "le", le);
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
        throw out_of_range(
            key, "Input should be greater than or equal to " + std::to_string(ge),
            *it, "greater_than_equal", "ge", ge);
    }
    if (v > le) {
        throw out_of_range(
            key, "Input should be less than or equal to " + std::to_string(le),
            *it, "less_than_equal", "le", le);
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

ApiResult post_story_chapters(const json& body,
                              std::shared_ptr<llm::Client> client) {
    forbid_extra(body, {"project", "overwrite"});
    const bool overwrite = opt_bool(body, "overwrite", false);

    // 409 在建 store 之前判，和 post_script_series 一个顺序：
    // 两个都错时回哪一个是可观测的。
    if (pipeline::jobs().running(pipeline::JobKind::Write)) {
        throw ApiError(409, "已经在写了");
    }
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);

    Story story;
    try {
        story = store.load_story();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
    if (story.chapters.empty()) {
        throw ApiError(400, "还没有故事。先写一份大纲，或者粘一段进来");
    }

    std::vector<std::string> todo;
    for (const auto& c : story.chapters) {
        if (overwrite || text::strip_ws(c.text).empty()) {
            todo.push_back(c.chapter_id);
        }
    }
    if (todo.empty()) throw ApiError(400, "每一章都有正文了");

    const models::StyleLine style = project.style_line;
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Write, "",
        [store, client, todo, style](pipeline::JobProgress& p) {
            p.set_total(static_cast<int>(todo.size()));
            // 推流式正文要用它。**按类订阅也收得到**：Hub 把 job_id 里第一个
            // '-' 之前的部分当类名（"write-a3f…" → "write"），而客户端只订
            // 得到类名——具体 id 从来不从任何接口暴露出去。
            const std::string job_id =
                pipeline::jobs().job_id(pipeline::JobKind::Write);
            int done = 0;
            for (const auto& id : todo) {
                if (p.cancelled()) return;

                // **每一轮重读。** 上一章写完已经落库了，这一章的提示词里
                // 「上一章是这么结束的」要拿到刚写的那一份。用循环外那个
                // 快照的话，每一章都以为自己接的是空的上一章。
                Story cur = store.load_story();
                const Chapter* me = cur.chapter_by_id(id);
                if (me == nullptr) {
                    p.set_done(++done);
                    continue;
                }
                p.set_message("正在写 " + id + "（" + me->title + "）");

                llm::Request req;
                try {
                    req.prompt = stages::build_chapter_prompt(cur, id, style);
                } catch (const std::exception& e) {
                    p.add_episode(json{{"chapter_id", id}, {"error", e.what()}});
                    p.set_done(++done);
                    continue;
                }
                req.schema = stages::chapter_schema();
                req.schema_name = "chapter";

                // **砸了就再要一次。**
                //
                // 最常见的砸法是模型把章节标题填进了正文字段，于是正文只有
                // 十几个字（守卫按目标篇幅的两成拦下来）。采样带随机种子，
                // 再要一次通常就对了——2026-09-11 实跑四章砸了两章，而这
                // 两章的失败彼此无关。
                //
                // **只有批量这条路重试。** 单章那个接口前面站着一个人，他
                // 看见报错自己会再按一下；批量这条是十六章无人看管地跑，
                // 中间掉两章的话，等他回来时故事里有两个空洞，而进度条上
                // 写的是"写完了 16 章"。
                //
                // 就多要一次，不是要到成功为止：提示词真有毛病时，重试到底
                // 只会把一次失败变成一小时失败。
                Story next;
                std::string last_error;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    if (attempt > 0) {
                        p.set_message("重写 " + id + "（" + me->title +
                                      "）——上一次只写出几个字");
                    }
                    try {
                        pipeline::CancelToken dummy;
                        const int floor_chars = static_cast<int>(
                            stages::chapter_target_chars(cur) *
                            stages::kChapterMinRatio);
                        // **批量这条也边写边推。** 十六章要跑一个多小时，
                        // 进度条上只有"正在写 ch07"的话，那一个多小时里
                        // 看不到一个字。推的是从 token 流里抠出来的正文，
                        // JSON 外壳和后面那串 hooks 不推（见 json_stream）。
                        stages::JsonFieldStreamer field("text");
                        int seq = 0;
                        const std::string raw = client->complete(
                            req, dummy, [&](const std::string& piece) {
                                const std::string fresh = field.feed(piece);
                                if (fresh.empty()) return;
                                ws::hub().broadcast(
                                    job_id, {{"type", "story_token"},
                                             {"job_id", job_id},
                                             {"chapter_id", id},
                                             {"seq", seq++},
                                             {"text", fresh}});
                            });
                        next = stages::apply_chapter(
                            cur, id, stages::parse_chapter(raw, floor_chars));
                        last_error.clear();
                        break;
                    } catch (const std::exception& e) {
                        last_error = e.what();
                    }
                    if (p.cancelled()) break;
                }
                if (!last_error.empty()) {
                    // 一章写砸了不该让前面几章白写，记下来接着往下写。
                    p.add_episode(json{{"chapter_id", id},
                                       {"error", last_error + "（重试过一次）"}});
                    p.set_done(++done);
                    continue;
                }

                next.plan = stages::plan_episodes(next, next.episode_duration_s);
                store.save_story(next);

                const Chapter* written = next.chapter_by_id(id);
                p.add_episode(json{
                    {"chapter_id", id},
                    {"title", written != nullptr ? written->title : std::string()},
                    {"chars", written != nullptr ? written->text_len() : 0},
                    {"episodes", next.plan.size()},
                });
                p.set_done(++done);
            }
            p.set_message("写完了 " + std::to_string(done) + " 章");
        },
        "已手动停止。已经写好的几章留着。");

    if (!started) throw ApiError(409, "已经在写了");
    return {200, {{"started", true}, {"chapters", todo.size()}}};
}

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
