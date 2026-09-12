#include "http/story_api.hpp"

#include <chrono>
#include <set>
#include <string>
#include <vector>

#include "models/project.hpp"
#include "models/story.hpp"
#include "stages/chapter_write.hpp"
#include "stages/story_analyze.hpp"
#include "stages/story_import.hpp"
#include "stages/story_outline.hpp"
#include "stages/story_plan.hpp"
#include "stages/story_reverse.hpp"
#include "http/job_stream.hpp"
#include "http/offload.hpp"
#include "http/ws.hpp"
#include "pipeline/activity.hpp"
#include "stages/json_partial.hpp"
#include "stages/json_stream.hpp"
#include "stages/story_revise.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

/// 校验请求体里没有多余字段。和 scripting.cpp 的同名函数一致：
/// 对应 pydantic 的 extra="forbid"，**422 不是 400**。
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
        throw unprocessable_top(key, "Field required", body, "missing");
    }
    if (!it->is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", *it,
                                "string_type");
    }
    return it->get<std::string>();
}

std::string opt_str(const json& body, const char* key,
                    const std::string& def = "") {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return def;
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

/// 体量。**不能直接 get<StoryScale>()**：NLOHMANN_JSON_SERIALIZE_ENUM
/// 生成的 from_json 遇到不认识的值会**悄悄退回第一项**（也就是 short），
/// 前端拼错一个字母就变成写短篇，而且不报错。
StoryScale opt_scale(const json& body, const char* key, StoryScale def) {
    const auto it = body.find(key);
    if (it == body.end()) return def;
    if (!it->is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", *it,
                                "string_type");
    }
    const std::string v = it->get<std::string>();
    if (v == "short") return StoryScale::SHORT;
    if (v == "medium") return StoryScale::MEDIUM;
    if (v == "long") return StoryScale::LONG;
    throw unprocessable_top(key, "Input should be 'short', 'medium' or 'long'",
                            *it, "enum");
}

ProjectStore open_project(const std::string& path) {
    if (path.empty()) throw ApiError(400, "没有指定项目目录");
    return ProjectStore(paths::from_utf8(path));
}

ProjectStore open_project(const json& body) {
    return open_project(need_str(body, "project"));
}

Project load_or_400(const ProjectStore& store) {
    try {
        return store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

Story load_story_or_400(const ProjectStore& store) {
    try {
        return store.load_story();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

/// 回给前端的形状。故事本体之外多带几个数，免得每个页面各数一遍。
json story_response(const Story& story) {
    return json{
        {"story", json(story)},
        {"chapters", story.chapters.size()},
        {"written", story.written_chapters()},
        {"episodes", story.plan.size()},
        {"empty", story.empty()},
    };
}

/// 落库前统一走一遍校验。坏数据进了 story.json，下次打开这个项目就废了。
///
/// 回 400 而不是 422：422 那条路的 detail 是 pydantic 形状的结构化数组，
/// 前端按那个形状去高亮对应的输入框。这里的错是「故事内部对不上」
/// （关系指向不存在的人、分集表指向不存在的章），没有哪个输入框对得上，
/// 挂成 422 前端只会显示成 [object Object]。
void validate_or_400(const Story& story) {
    const auto errs = story.validate();
    if (errs.empty()) return;
    std::string msg = "这份故事里有对不上的地方：";
    for (std::size_t i = 0; i < errs.size(); ++i) {
        msg += (i == 0 ? "" : "；");
        msg += errs[i];
    }
    throw ApiError(400, msg);
}

}  // namespace

ApiResult get_story(const std::string& path) {
    ProjectStore store = open_project(path);
    load_or_400(store); // 只为了验证这是个项目目录，结果不用
    return {200, story_response(load_story_or_400(store))};
}

ApiResult post_story(const json& body) {
    forbid_extra(body, {"project", "premise", "scale", "episode_duration_s"});
    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    Story story = load_story_or_400(store);

    if (body.contains("premise")) {
        const std::string p = text::strip_ws(need_str(body, "premise"));
        story.premise = text::truncate_utf8(p, 2000);
        // 老流程的写剧本提示词读的是 Project::premise。两边各存一份的话，
        // 在故事页改完梗概、去写剧本用的还是旧的那句。
        if (project.premise != story.premise) {
            project.premise = story.premise;
            store.save_project(project);
        }
    }
    story.scale = opt_scale(body, "scale", story.scale);
    story.episode_duration_s =
        num_in_range(body, "episode_duration_s", story.episode_duration_s, 0.0,
                     1800.0);

    validate_or_400(story);
    store.save_story(story);
    return {200, story_response(story)};
}

// 这个函数在头文件里声明了（导出只为了能测），所以不能放进匿名 namespace。
/// 从**补齐过的半份大纲**里挑界面要显示的那几样。
///
/// ⚠️ **每一项都要先问类型。** 这份 JSON 是半路截下来补出来的，任何一个
/// 字段都可能是 null（比如刚写到 `"logline":` 还没开始写值）。拿
/// `value("logline", "")` 去取的话，nlohmann 在 null 上转字符串抛的是
/// type_error——而那会把整条生成搞挂，就为了推一帧进度。
json outline_progress_payload(const json& snap) {
    const auto str_of = [](const json& j, const char* key) -> std::string {
        const auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>()
                                                  : std::string();
    };

    json chapters = json::array();
    if (const auto it = snap.find("chapters");
        it != snap.end() && it->is_array()) {
        for (const auto& c : *it) {
            if (!c.is_object()) continue;
            chapters.push_back({{"title", str_of(c, "title")},
                                {"summary", str_of(c, "summary")},
                                {"hook", str_of(c, "hook")}});
        }
    }
    json people = json::array();
    if (const auto it = snap.find("characters");
        it != snap.end() && it->is_array()) {
        for (const auto& c : *it) {
            if (!c.is_object()) continue;
            people.push_back({{"name", str_of(c, "name")},
                              {"identity", str_of(c, "identity")}});
        }
    }
    return {{"premise", str_of(snap, "premise")},
            {"logline", str_of(snap, "logline")},
            {"genre", str_of(snap, "genre")},
            {"tone", str_of(snap, "tone")},
            {"characters", people},
            {"chapters", chapters}};
}

/// 写大纲。**同步和异步两条路跑的是这同一段**，理由同 write_one_chapter。
json write_outline(ProjectStore& store, const Project& project,
                   const Story& existing, std::string premise, StoryScale scale,
                   const std::string& keywords, const std::string& stream_id,
                   llm::Client& client, pipeline::CancelToken& tok) {
    pipeline::Activity act{"outline", paths::to_utf8(store.root()), "",
                           "正在出大纲"};

    llm::Request req;
    req.prompt =
        stages::build_outline_prompt(premise, scale, project.style_line, keywords);
    req.schema = stages::outline_schema();
    req.schema_name = "story_outline";

    Story draft;
    try {
        std::string raw;
        if (stream_id.empty()) {
            raw = client.complete(req, tok);
        } else {
            // **边写边推。** 出一份大纲三四十秒，攒齐了再蹦出来的话那几十秒
            // 界面上一个字都没有——而"它在想什么"正是这一步用户要看的东西。
            //
            // 章节正文那条抠的是一个字段（JsonFieldStreamer），这儿不行：
            // 大纲是一整个对象，章节还是一串对象。所以走"补齐再解析"，
            // 每一帧把能解出来的那一份挑几样推过去，见 stages/json_partial。
            stages::PartialJson partial;
            int seq = 0;
            // 节流。一帧要把前缀整个解一遍，而 token 是几十毫秒一个；
            // 不节流的话这条回调自己就成了负载，而人眼也看不出区别。
            //
            // ⚠️ **起点要往前推一个节流窗口**，不然第一帧会被自己吞掉——
            // 而有的后端是**整段一次回调**（Client::complete 的默认实现就是
            // 这样，回放后端走的正是它），那种情况下"第一帧"也是唯一一帧，
            // 吞掉就等于一帧都没推。
            auto last = std::chrono::steady_clock::now() -
                        std::chrono::milliseconds(200);
            std::size_t last_size = 0;
            raw = client.complete(req, tok, [&](const std::string& piece) {
                partial.feed(piece);
                const auto now = std::chrono::steady_clock::now();
                if (now - last < std::chrono::milliseconds(200)) return;
                if (partial.size() == last_size) return;   // 没长就别重解
                last = now;
                last_size = partial.size();
                const json snap = partial.snapshot();
                if (!snap.is_object()) return;   // 这一帧补不出来，跳过
                json msg = outline_progress_payload(snap);
                msg["type"] = "outline_progress";
                msg["job_id"] = stream_id;
                msg["seq"] = seq++;
                ws::hub().broadcast(stream_id, std::move(msg));
            });
        }
        draft = stages::parse_outline(raw, premise, scale);
    } catch (const stages::StoryError& e) {
        throw ApiError(502, std::string("大模型没写出能用的大纲：") + e.what());
    } catch (const std::exception& e) {
        throw ApiError(502, e.what());
    }

    // 每集时长沿用项目上已经定过的那个，草稿里就能看到分成几集。
    draft.episode_duration_s = existing.episode_duration_s;
    draft.plan = stages::plan_episodes(draft, draft.episode_duration_s);

    json out = story_response(draft);
    // **草稿，没落库。** 前端要拿这一份去 /api/story/adopt 才算数。
    out["adopted"] = false;
    return out;
}

ApiResult post_story_outline(const json& body, llm::Client& client,
                             pipeline::CancelToken& tok) {
    forbid_extra(body,
                 {"project", "premise", "scale", "keywords", "stream", "async"});
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story existing = load_story_or_400(store);

    // 梗概没给就用存着的那份。隔天回来接着写大纲时不用重打一遍。
    std::string premise = text::strip_ws(opt_str(body, "premise", ""));
    if (premise.empty()) premise = existing.premise;
    if (premise.empty()) premise = text::strip_ws(project.premise);
    // **一个字都没有也照写。** 选题是整条流水线上最难从零开始的一步，
    // 把它做成必填门槛就是把人摁在空白框前面发呆；这时候让模型连选题带
    // 大纲一起出，人再挑。给了关键词的话它会往那个方向想。

    const StoryScale scale = opt_scale(body, "scale", existing.scale);
    const std::string keywords = opt_str(body, "keywords", "");
    const std::string stream_id = text::strip_ws(opt_str(body, "stream"));

    // 异步那条，理由和写一章一模一样（见 post_story_chapter 里那段）：
    // 这个 handler 占着 Crow 的一条 I/O 线程，而出一份大纲要三四十秒，
    // 落在同一条线程上的连接会跟着冻住——顶栏那块表首当其冲。
    if (opt_bool(body, "async", false) && !stream_id.empty()) {
        const std::string project_path = paths::to_utf8(store.root());
        Offload::instance().post([project_path, premise, scale, keywords,
                                  stream_id, &client] {
            try {
                ProjectStore st = open_project(project_path);
                const Project pj = load_or_400(st);
                const Story ex = load_story_or_400(st);
                pipeline::CancelToken own;
                job_done(stream_id, write_outline(st, pj, ex, premise, scale,
                                                  keywords, stream_id, client,
                                                  own));
            } catch (const ApiError& e) {
                job_error(stream_id, e.what());
            } catch (const std::exception& e) {
                job_error(stream_id, e.what());
            }
        });
        return {202, {{"started", true}, {"stream", stream_id}}};
    }

    return {200, write_outline(store, project, existing, premise, scale,
                               keywords, stream_id, client, tok)};
}

ApiResult post_story_adopt(const json& body) {
    forbid_extra(body, {"project", "story", "overwrite"});
    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    const Story existing = load_story_or_400(store);

    const auto it = body.find("story");
    if (it == body.end() || !it->is_object()) {
        throw unprocessable_top("story", "Field required", body, "missing");
    }

    Story story;
    try {
        story = it->get<Story>();
    } catch (const std::exception& e) {
        throw ApiError(400, std::string("这份大纲读不了：") + e.what());
    }

    // 已经展开过正文的故事被一份新大纲整份顶掉，那些正文就没了。
    // 章节 id 是按位置生成的，没法靠合并保住——只能挡在这里。
    const int written = existing.written_chapters();
    if (written > 0 && !opt_bool(body, "overwrite", false)) {
        throw ApiError(409, "这个项目里已经有 " + std::to_string(written) +
                                " 章写好了正文，采用新大纲会把它们顶掉。"
                                "确认要换的话带上 overwrite。");
    }

    if (story.episode_duration_s <= 0.0) {
        story.episode_duration_s = existing.episode_duration_s;
    }
    story.plan = stages::plan_episodes(story, story.episode_duration_s);

    validate_or_400(story);
    store.save_story(story);

    if (!story.premise.empty() && project.premise != story.premise) {
        project.premise = story.premise;
        store.save_project(project);
    }

    json out = story_response(story);
    out["adopted"] = true;
    return {200, out};
}

ApiResult post_story_import(const json& body) {
    forbid_extra(body, {"project", "text", "scale", "title"});
    ProjectStore store = open_project(body);
    load_or_400(store);
    const Story existing = load_story_or_400(store);

    const std::string raw = need_str(body, "text");
    if (text::strip_ws(raw).empty()) throw ApiError(400, "粘进来的是空的");

    Story draft;
    draft.source = StorySource::PASTED;
    draft.scale = opt_scale(body, "scale", existing.scale);
    draft.premise = existing.premise;
    draft.logline = text::clean_field(opt_str(body, "title", ""));
    draft.chapters = stages::split_pasted(raw);
    if (draft.chapters.empty()) throw ApiError(400, "这段文本切不出章节来");

    draft.episode_duration_s = existing.episode_duration_s;
    draft.plan = stages::plan_episodes(draft, draft.episode_duration_s);

    json out = story_response(draft);
    out["adopted"] = false;
    // 人物、关系、地点都还是空的——那些要读懂内容才提得出来。前端靠这个
    // 数提醒人「下一步让 AI 读一遍」，不然采用之后会一路走到分镜才发现
    // 资产库是空的。
    out["needs_analysis"] = draft.characters.empty();
    return {200, out};
}

ApiResult post_story_analyze(const json& body, llm::Client& client,
                             pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "stream"});   // stream 同上，只为异步外壳
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story story = load_story_or_400(store);

    if (story.chapters.empty()) {
        throw ApiError(400, "还没有故事。先写一份大纲，或者粘一段进来");
    }
    if (story.written_chapters() == 0) {
        throw ApiError(400,
                       "章节都还没有正文，没什么可读的。"
                       "大纲写出来的故事本来就带人物表，不用走这一步");
    }

    pipeline::Activity act{"analyze", paths::to_utf8(store.root()), "",
                           "正在读这个故事"};

    llm::Request req;
    req.prompt = stages::build_analyze_prompt(story, project.style_line);
    req.schema = stages::analyze_schema();
    req.schema_name = "story_analysis";

    Story draft;
    try {
        draft = stages::apply_analysis(story, client.complete(req, tok));
    } catch (const stages::StoryError& e) {
        throw ApiError(502, std::string("大模型没读出能用的结构：") + e.what());
    } catch (const std::exception& e) {
        throw ApiError(502, e.what());
    }

    // 钩子变了，切点就变了——重算一遍分集表。这正是这一步的价值：
    // 机械切点只保证不切在半句话中间，现在能切在真正的悬念上了。
    draft.plan = stages::plan_episodes(draft, draft.episode_duration_s);

    json out = story_response(draft);
    out["adopted"] = false;
    out["needs_analysis"] = draft.characters.empty();
    return {200, out};
}

/// 真正写这一章：借大模型、边写边推、解析、落库、重算分集。
///
/// **从接口里抽出来的，因为它有两条调用路**：同步那条（老客户端）直接在
/// Crow 的线程上跑完；异步那条在后台线程上跑，接口早就回过"开始了"。
/// 两条路跑的必须是同一段代码——抄一份的话，两边迟早只改一边，而不一致的
/// 表现是"用新版界面写出来的和用 curl 写出来的不一样"。
json write_one_chapter(ProjectStore& store, const Project& project, Story story,
                       const std::string& chapter_id,
                       const std::string& stream_id, llm::Client& client,
                       pipeline::CancelToken& tok) {
    const Chapter* me = story.chapter_by_id(chapter_id);
    if (me == nullptr) throw ApiError(404, "没有这一章：" + chapter_id);

    // 登记到顶栏那本账上。不登记的话这一两分钟里引擎在界面上看着是闲着的
    // ——而它正占着大模型那一槽，别的活全得等。
    pipeline::Activity act{"write_one", paths::to_utf8(store.root()), chapter_id,
                           "正在写 " + (me->title.empty() ? chapter_id : me->title)};

    llm::Request req;
    req.prompt =
        stages::build_chapter_prompt(story, chapter_id, project.style_line);
    req.schema = stages::chapter_schema(stages::chapter_target_scenes(story),
                                       stages::chapter_scene_paras(story));
    req.schema_name = "chapter";
    req.temperature = stages::kChapterTemperature;

    // 给了 stream_id 就**边写边推**。写一章要一两分钟，攒齐了再蹦出来的话
    // 那一两分钟界面上什么都没有——而那正是用户要看的"写作的过程"。
    //
    // **这一步不能像改稿那样退回大白话。** 除了正文还要模型标出这一章里
    // 哪几个地方可以收一集（hooks），而那些钩子是一集停在真悬念上的全部
    // 依据（实跑里把比例从 25% 抬到 56%）。为了能流式砍掉 hooks，等于拿
    // 分集质量换一个动画。所以照旧约束成 JSON，只在 token 流上顺手把正文
    // 那个字段解出来推给编辑器——见 stages/json_stream。
    Story next;
    try {
        const int floor_chars = static_cast<int>(
            stages::chapter_target_chars(story) * stages::kChapterMinRatio);
        std::string raw;
        if (stream_id.empty()) {
            raw = client.complete(req, tok);
        } else {
            // **抠的是 paragraphs，不是 text。** c41821d 把章节正文从一个
            // 字符串改成了一段一项的数组，而这里没跟着改——于是流式一个字
            // 都抠不出来，界面上就是"AI 写作没有热更新"，后端不报任何错。
            stages::JsonFieldStreamer field(stages::kChapterBodyField, true);
            int seq = 0;
            raw = client.complete(req, tok, [&](const std::string& piece) {
                const std::string fresh = field.feed(piece);
                if (fresh.empty()) return;  // JSON 外壳和 hooks 那一串不推
                ws::hub().broadcast(stream_id, {{"type", "story_token"},
                                                {"job_id", stream_id},
                                                {"seq", seq++},
                                                {"text", fresh}});
            });
        }
        const stages::ChapterDraft d = stages::parse_chapter(raw, floor_chars);
        next = stages::apply_chapter(story, chapter_id, d);
    } catch (const stages::StoryError& e) {
        // story_error 是给编辑器用的（把流了一半的字撤掉）。异步那条路上
        // 还有个人在等最终结果，那条 job_error 由 server.cpp 的 start_async
        // 播——两条各管各的，别合并。
        if (!stream_id.empty()) {
            ws::hub().broadcast(stream_id, {{"type", "story_error"},
                                            {"job_id", stream_id},
                                            {"message", e.what()}});
        }
        throw ApiError(502, std::string("大模型没写出能用的正文：") + e.what());
    } catch (const std::exception& e) {
        if (!stream_id.empty()) {
            ws::hub().broadcast(stream_id, {{"type", "story_error"},
                                            {"job_id", stream_id},
                                            {"message", e.what()}});
        }
        throw ApiError(502, e.what());
    }

    // 钩子换了，切点就换了。正文落进去之前那些候选是对着空正文算的。
    next.plan = stages::plan_episodes(next, next.episode_duration_s);
    validate_or_400(next);
    store.save_story(next);

    json out = story_response(next);
    out["chapter_id"] = chapter_id;
    const Chapter* done = next.chapter_by_id(chapter_id);
    out["chars"] = done != nullptr ? done->text_len() : 0;
    out["target_chars"] = stages::chapter_target_chars(story);
    return out;
}

ApiResult post_story_chapter(const json& body, llm::Client& client,
                             pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "chapter_id", "overwrite", "stream"});
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    Story story = load_story_or_400(store);

    const std::string chapter_id = need_str(body, "chapter_id");
    const Chapter* me = story.chapter_by_id(chapter_id);
    if (me == nullptr) throw ApiError(404, "没有这一章：" + chapter_id);
    if (!text::strip_ws(me->text).empty() && !opt_bool(body, "overwrite", false)) {
        throw ApiError(409, "这一章已经有正文了。要重写就带上 overwrite");
    }

    const std::string stream_id = text::strip_ws(opt_str(body, "stream"));

    return {200, write_one_chapter(store, project, std::move(story), chapter_id,
                                   stream_id, client, tok)};
}

ApiResult post_story_episodes(const json& body) {
    forbid_extra(body, {"project"});
    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    const Story story = load_story_or_400(store);

    if (story.plan.empty()) {
        throw ApiError(400, "还没有分集表。先写一份大纲，或者改一下每集时长重算一次");
    }

    std::vector<std::string> created;
    std::vector<std::string> updated;
    for (const auto& p : story.plan) {
        if (p.episode_id.empty()) continue;

        // 覆盖了哪几章。存 id 不存区间——区间在 story.plan 里，
        // 存两份迟早对不上。
        std::vector<std::string> refs;
        bool inside = false;
        for (const auto& c : story.chapters) {
            if (c.chapter_id == p.from_chapter) inside = true;
            if (inside) refs.push_back(c.chapter_id);
            if (c.chapter_id == p.to_chapter) break;
        }

        // 一句话梗概：优先用首章的 summary（说这一集讲什么），
        // 没有才退回钩子（说这一集停在哪）。
        std::string synopsis;
        if (!refs.empty()) {
            const Chapter* c = story.chapter_by_id(refs.front());
            if (c != nullptr) {
                synopsis = text::truncate_utf8(text::collapse_ws(c->summary), 120);
            }
        }
        if (synopsis.empty()) synopsis = p.hook;

        Episode* existing = project.episode_by_id(p.episode_id);
        if (existing != nullptr) {
            // **只补元数据。** script 和 shots 一个字不动——改一次每集时长
            // 就把写好的剧本和出过的片冲掉，那是没法接受的。
            if (!p.title.empty()) existing->title = p.title;
            existing->target_duration_s = p.target_duration_s;
            existing->chapter_refs = refs;
            if (existing->synopsis.empty()) existing->synopsis = synopsis;
            updated.push_back(p.episode_id);
            continue;
        }

        Episode ep;
        ep.episode_id = p.episode_id;
        ep.title = p.title;
        ep.synopsis = synopsis;
        ep.target_duration_s = p.target_duration_s;
        ep.chapter_refs = refs;
        project.episodes.push_back(std::move(ep));
        created.push_back(p.episode_id);
    }

    store.save_project(project);

    json out = story_response(story);
    out["created"] = created;
    out["updated"] = updated;
    return {200, out};
}

namespace {

/// 从请求体里取出"选中的哪一段"，顺手校验。
stages::Span need_span(const json& body, const Story& story) {
    stages::Span span;
    span.chapter_id = need_str(body, "chapter_id");
    const Chapter* c = story.chapter_by_id(span.chapter_id);
    if (c == nullptr) throw ApiError(404, "没有这一章：" + span.chapter_id);

    const int len = c->text_len();
    span.from_char = static_cast<int>(
        num_in_range(body, "from_char", 0.0, -1.0, 1e9));
    span.to_char = static_cast<int>(
        num_in_range(body, "to_char", static_cast<double>(len), -1.0, 1e9));
    if (span.from_char < 0 || span.to_char > len || span.from_char >= span.to_char) {
        // **位置对不上就拒**，别夹到合法范围里硬改。夹过之后改的是另一段
        // 字，而用户看到的是"改好了"——他得自己一段段核对才发现改错了地方。
        throw ApiError(400, "选中的范围不对：这一章有 " + std::to_string(len) +
                                " 个字，而选的是 [" +
                                std::to_string(span.from_char) + ", " +
                                std::to_string(span.to_char) + ")");
    }
    return span;
}

std::vector<stages::ReviseTurn> read_history(const json& body) {
    std::vector<stages::ReviseTurn> out;
    const auto it = body.find("history");
    if (it == body.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (!v.is_object()) continue;
        stages::ReviseTurn t;
        t.role = v.value("role", std::string("user"));
        t.text = text::strip_ws(v.value("text", std::string()));
        if (t.text.empty()) continue;
        if (t.role != "assistant") t.role = "user";
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace

ApiResult post_story_revise(const json& body, llm::Client& client,
                            pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "chapter_id", "from_char", "to_char",
                        "instruction", "history", "stream"});
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story story = load_story_or_400(store);

    const stages::Span span = need_span(body, story);
    const std::string instruction = text::strip_ws(opt_str(body, "instruction"));
    const auto history = read_history(body);
    if (instruction.empty() && history.empty()) {
        throw ApiError(400, "没说要改成什么样。选中一段之后说一句，比如"
                            "「这儿太赶了，铺一下情绪」");
    }

    const std::string before = stages::span_text(story, span);
    const int span_chars = static_cast<int>(text::utf8_len(before));
    // 整章多长也要给：上限取"选中的六倍"和"整章的六成"里松的那条，而后者
    // 才是"抄整章"的真判据。只看倍数的话，短选区上一个正当的「拉长」会被
    // 打回——实跑撞过。
    const Chapter* whole = story.chapter_by_id(span.chapter_id);
    const int whole_chars = whole != nullptr ? whole->text_len() : 0;

    // 给了 stream_id 就**边生边推**：写一段话要十几秒，攒齐了再一次性蹦
    // 出来的话，中间那十几秒界面上什么都没有——而那正是用户要看的"写作的
    // 过程"。字走 WebSocket，这个请求照样在最后回完整的一份（前端拿它对
    // 一遍，也让丢包的连接有个兜底）。
    const std::string stream_id = text::strip_ws(opt_str(body, "stream"));
    const bool streaming = !stream_id.empty();

    pipeline::Activity act{"revise", paths::to_utf8(store.root()),
                           span.chapter_id, "正在改这一段"};

    llm::Request req;
    // **流式那条不要 JSON。** 逐字插进编辑器的话，用户先看到的会是
    // `{"text":"` 这几个字符。代价是没有 note，那本来也只是锦上添花。
    req.prompt = stages::build_revise_prompt(story, span, instruction, history,
                                             project.style_line, streaming);
    if (!streaming) {
        req.schema = stages::revise_schema();
        req.schema_name = "story_revision";
    }

    stages::Revision rev;
    try {
        if (streaming) {
            int seq = 0;
            const std::string raw = client.complete(
                req, tok, [&](const std::string& piece) {
                    // **不节流。** 逐字推正是这件事的全部意义；而 Hub 的
                    // 节流是按 (job, type) 分桶的，type 用 progress 的话
                    // 会被 200ms 一桶压掉九成。
                    ws::hub().broadcast(stream_id,
                                        {{"type", "story_token"},
                                         {"job_id", stream_id},
                                         {"seq", seq++},
                                         {"text", piece}});
                });
            rev = stages::parse_plain_revision(raw, span_chars, whole_chars);
        } else {
            rev = stages::parse_revision(client.complete(req, tok), span_chars,
                                         whole_chars);
        }
    } catch (const std::exception& e) {
        if (streaming) {
            // 报错也要推一条：前端那边正等着字，不推的话它一直显示"改着…"
            ws::hub().broadcast(stream_id, {{"type", "story_error"},
                                            {"job_id", stream_id},
                                            {"message", e.what()}});
        }
        throw ApiError(502, std::string("改稿没改出能用的东西：") + e.what());
    }
    if (streaming) {
        ws::hub().broadcast(stream_id, {{"type", "story_done"},
                                        {"job_id", stream_id},
                                        {"text", rev.text}});
    }

    // **只回草稿，不落库。** 和写大纲同一条规矩，而且这里更要紧：大纲落错了
    // 重写一份就是，改稿落错了盖掉的是作者自己写的字。
    return {200, {
        {"chapter_id", span.chapter_id},
        {"from_char", span.from_char},
        {"to_char", span.to_char},
        {"before", before},
        {"text", rev.text},
        {"note", rev.note},
    }};
}

ApiResult post_story_revise_apply(const json& body) {
    forbid_extra(body, {"project", "chapter_id", "from_char", "to_char", "text"});
    ProjectStore store = open_project(body);
    load_or_400(store);
    const Story story = load_story_or_400(store);

    const stages::Span span = need_span(body, story);
    const std::string text_in = text::strip_ws(need_str(body, "text"));
    if (text_in.empty()) throw ApiError(400, "要写回去的那一段是空的");

    Story next = stages::apply_revision(story, span, text_in);
    // **分集表跟着重算。** 正文长度变了，后面每一条的字符区间都错位了；
    // 不重算的话切线会落在句子中间，而这件事不报错，只在成片里表现成
    // "这一集从半句话开始"。
    next.plan = stages::plan_episodes(next, next.episode_duration_s);

    validate_or_400(next);
    store.save_story(next);

    json out = story_response(next);
    const Chapter* c = next.chapter_by_id(span.chapter_id);
    out["chapter_id"] = span.chapter_id;
    out["chars"] = c != nullptr ? c->text_len() : 0;
    return {200, out};
}

ApiResult post_story_from_episodes(const json& body) {
    forbid_extra(body, {"project", "overwrite"});
    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const Story existing = load_story_or_400(store);

    // 已经有故事了还反推，反推出来的那份会把它整份顶掉——而那一份里可能
    // 有人工改过的人物关系和分集切线。和采用大纲同一条规矩。
    if (!existing.empty() && !opt_bool(body, "overwrite", false)) {
        throw ApiError(409,
                       "这个项目已经有故事了，反推会把它整份顶掉。"
                       "确认要换的话带上 overwrite。");
    }

    Story story = stages::story_from_episodes(project);
    if (story.chapters.empty()) {
        throw ApiError(400,
                       "这个项目里一集剧本都没有，反推不出东西来。"
                       "先写一集，或者直接在故事那一页写大纲");
    }

    validate_or_400(story);
    store.save_story(story);

    // **顺手把集和章接上。** 不接的话故事在这儿、剧集在那儿，两边看着都
    // 齐全，只有写下一集时才发现它拿不到前情——而那时候没有任何报错。
    Project linked = project;
    for (const auto& p : story.plan) {
        Episode* ep = linked.episode_by_id(p.episode_id);
        if (ep != nullptr) ep->chapter_refs = {p.from_chapter};
    }
    store.save_project(linked);

    json out = story_response(story);
    // 反推是机械的，人物关系一个都没有——这里说清楚下一步该点哪儿，
    // 不然用户会以为反推完就齐了，而故事页上人物那一栏是空的。
    out["next"] = "读现成正文提结构";
    return {200, out};
}

ApiResult post_story_plan(const json& body) {
    forbid_extra(body, {"project", "duration_s"});
    ProjectStore store = open_project(body);
    load_or_400(store);
    Story story = load_story_or_400(store);

    if (story.chapters.empty()) {
        throw ApiError(400, "还没有故事，先写一份大纲");
    }

    story.episode_duration_s =
        num_in_range(body, "duration_s", story.episode_duration_s, 0.0, 1800.0);
    story.plan = stages::plan_episodes(story, story.episode_duration_s);

    validate_or_400(story);
    store.save_story(story);
    return {200, story_response(story)};
}

}  // namespace changji::http
