#include "http/scripting.hpp"

#include "config/settings.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "http/job_stream.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "pipeline/activity.hpp"
#include "stages/script.hpp"
#include "stages/script_story.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

/// 校验请求体里没有多余字段。
///
/// 对应 pydantic 的 model_config = {"extra": "forbid"}。**422 不是 400**——
/// FastAPI 走的是校验错误那条路，body 是结构化数组不是一句话。
/// 对拍语料抓到过我在别处写成 400。
void forbid_extra(const json& body, const std::set<std::string>& allowed) {
    if (!body.is_object()) throw ApiError(400, "请求体要是一个对象");
    for (const auto& kv : body.items()) {
        // **`stream` 一律放行。** 它是传输层的信封字段，不是业务字段：
        // 路由那一层（script_route / batch_route）拿它决定这件活挪不挪到
        // 后台、结果往哪条 WebSocket 送，处理函数多半根本不看它。
        //
        // 原来是各家自己往白名单里加，2026-09-14 栽了：给「照故事定妆」
        // 接上思考流之后前端开始发 stream，而 post_bible 的白名单里没有，
        // 一按就是 422 `Extra inputs are not permitted`。十几个处理函数
        // 挨个加，漏一个的表现就是那一步整个不能用。
        //
        // `async` 不在这儿放行是因为它在更上面就被 take_async 摘掉了
        // （见 server.cpp），到这儿本来就没有。
        if (kv.key() == "stream") continue;
        if (allowed.count(kv.key()) == 0) {
            throw unprocessable_top(kv.key(), "Extra inputs are not permitted",
                                    kv.value(), "extra_forbidden");
        }
    }
}

/// 必填的字符串字段。缺了是 **422 不是 400**。
///
/// 少一个必填字段走的是 pydantic 的校验那条路，和多一个字段
/// （extra="forbid"）是同一类错误，前端也按同一种方式处理。
/// 写成 400 的话前端拿到的 detail 是字符串不是数组，
/// 校验错误的高亮逻辑整个不生效——对拍抓到过。
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

/// 取一个带范围的数字。越界抛 422，和 pydantic 的 Field(gt=..., le=...) 一致。
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
        // Python 那边 catch 的是 FileNotFoundError 和 ValueError，都转 400
        throw ApiError(400, e.what());
    }
}

AssetLibrary load_assets_or_400(const ProjectStore& store) {
    try {
        return store.load_assets();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

/// 把大模型的异常翻成 400。
///
/// 三个接口都是 `except ScriptError: raise HTTPException(400, str(exc))`。
/// LlmError 也走这条——它的消息本来就是给用户看的那句话。
template <typename F>
auto llm_guard(F&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const stages::ScriptError& e) {
        throw ApiError(400, e.what());
    } catch (const llm::LlmError& e) {
        throw ApiError(400, e.what());
    }
}

json draft_common(const stages::ScriptDraft& draft, double duration_s) {
    return {
        {"title", draft.title},
        {"logline", draft.logline},
        {"script", draft.render()},
        {"speakers", draft.speakers()},
        {"dialogue_chars", draft.dialogue_chars()},
        {"budget_chars", stages::budget_chars(duration_s)},
        {"beats", draft.beats.size()},
    };
}

std::vector<std::string> character_names(const AssetLibrary& assets) {
    std::vector<std::string> names;
    for (const auto& kv : assets.characters) names.push_back(kv.second.name);
    return names;
}

}  // namespace

ApiResult post_script_premise(const json& body, llm::Client& client,
                              pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "keywords", "count"});
    const std::string keywords = opt_str(body, "keywords");
    if (text::utf8_len(keywords) > 200) {
        throw unprocessable_top("keywords",
                                "String should have at most 200 characters",
                                body.at("keywords"), "string_too_long");
    }
    const int count = int_in_range(body, "count", 3, 3, 5);

    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);

    // 项目上已有的梗概算一个「已经想过的方向」，避免连点两次拿回同一批。
    // 已经写了几集的话，那些也算。
    std::vector<std::string> existing;
    if (!text::strip_ws(project.premise).empty()) existing.push_back(project.premise);
    for (const auto& ep : project.episodes) {
        if (!text::strip_ws(ep.synopsis).empty()) existing.push_back(ep.synopsis);
    }

    const std::string prompt = stages::build_premise_prompt(
        keywords, project.style_line, count, existing);

    llm::Request req;
    req.prompt = prompt;
    req.schema = stages::premise_schema();
    req.schema_name = "premises";
    req.on_thinking = thinking_sink();

    // **顶栏那本账要记上。** 这几个接口是同步的，没有任务表那一套，
    // 2026-09-13 之前它们在界面上整个不可见：用户点了「重新改编」，
    // 顶栏一片安静，而这一刻 LLM 槽是被它占着的——另一头的批量写作会
    // 挂在「显存不够加载 LLM」上，挡路的那件事却查不到。
    // 见 pipeline/activity.hpp 开头那段。
    pipeline::Activity act{"premise", paths::to_utf8(store.root()), "",
                           "正在想梗概"};
    const auto ideas = llm_guard([&] {
        return stages::parse_premises(client.complete(req, tok));
    });

    json out = json::array();
    for (const auto& i : ideas) {
        out.push_back({{"title", i.title}, {"premise", i.premise},
                       {"hook", i.hook}});
    }
    return {200, {{"ideas", out}, {"style_line", to_string(project.style_line)}}};
}

ApiResult post_script_write(const json& body, llm::Client& client,
                            pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "episode_id", "premise", "duration_s",
                        "continue_from_previous", "reuse_characters",
                        "variation"});
    const std::string premise = need_str(body, "premise");
    const std::string episode_id = opt_str(body, "episode_id");
    const double duration_s =
        num_in_range(body, "duration_s", 60.0, 0.0, 1800.0);
    const bool continue_prev = opt_bool(body, "continue_from_previous", true);
    const bool reuse_chars = opt_bool(body, "reuse_characters", true);

    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    const AssetLibrary assets = load_assets_or_400(store);

    // 这一集在分集表里有对应的一条吗？有就走故事那条：这一集要发生什么
    // 已经定好了，模型只负责把那一段变成拍子。**失忆是在那条路上治好的**——
    // 带的上下文是压缩的全局记忆（大纲、人物、关系、前情提要每章一句），
    // 不是下面那个「最近三集原文截 4000 字符」。
    Story story;
    const EpisodePlan* plan = nullptr;
    EpisodePlan chapter_plan_storage;
    // **章模式**（[assembly].episode_s > 0）：没有秒数、没有字数，这一章
    // 写多长由内容定；分镜出片之后再按 episode_s 切成几集。
    const bool chapter_mode =
        config::load_settings(store.root()).assembly.episode_s > 0.0;
    if (!episode_id.empty()) {
        try {
            story = store.load_story();
        } catch (const std::exception&) {
            // 读不了就当没有，退回老路径。老项目本来就没有这个文件。
        }
        // 章模式：这一集就是一章，计划按章配（整章、钩子取最后一场的
        // turn）。**不按 episode_id 去分集表里查**——那张表的 id 是按切片
        // 发的，一章切两段就有两条，和「ch07 → ep07」对不上，查到的是
        // 隔壁章的半截（2026-09-16 实撞）。
        if (chapter_mode) {
            const Episode* ep = project.episode_by_id(episode_id);
            if (ep != nullptr && !ep->chapter_refs.empty() &&
                story.chapter_by_id(ep->chapter_refs.front()) != nullptr) {
                chapter_plan_storage = stages::chapter_plan(
                    story, ep->chapter_refs.front(), ep->target_duration_s);
                plan = &chapter_plan_storage;
            }
        }
        if (plan == nullptr) {
            for (const auto& p : story.plan) {
                if (p.episode_id == episode_id) {
                    plan = &p;
                    break;
                }
            }
        }
    }

    std::string previous;
    if (continue_prev && plan == nullptr) {
        // 只取这一集**之前**的几集。把后面的也塞进去，模型会把还没发生的
        // 事当成已经发生的写。
        std::vector<std::string> earlier;
        for (const auto& ep : project.episodes) {
            if (!episode_id.empty() && ep.episode_id == episode_id) break;
            // 预告片是从正片里剪出来的，再拿它当写正片的上下文，
            // 模型会开始抄自己的预告，越写越像宣传语
            if (ep.episode_id == kTrailerEpisodeId) continue;
            const std::string s = text::strip_ws(ep.script);
            if (s.empty()) continue;
            earlier.push_back("【" + ep.episode_id + "】\n" + s);
        }
        // 只要最近三集。给多了模型会顾此失彼，而且提示词会撑爆上下文。
        const std::size_t skip = earlier.size() > 3 ? earlier.size() - 3 : 0;
        for (std::size_t i = skip; i < earlier.size(); ++i) {
            if (i > skip) previous += "\n\n";
            previous += earlier[i];
        }
    }

    const std::vector<std::string> names =
        reuse_chars ? character_names(assets) : std::vector<std::string>{};

    std::vector<stages::ScenePlan> chapter_scenes;

    std::string prompt;
    const char* source = "premise";
    // 这一集四段的形状（占几秒、每段是什么戏）。
    //
    // **两条路都摇。** 原来这儿写死 0，注释是「照梗概续写那条老路子保持
    // 原样」——那条路于是永远是 8%/10%/57% 加同一出戏（钩子 → 推进 →
    // 回报 → 留扣）。保持原样保住的是"每集一个模子"，而那正是用户说的
    // 「剧本时间线也都差不多」。
    //
    // 摇一个就够：提示词、schema、解析三处共用这一个变量，形状和秒数
    // 自然对得上（对不上的表现是段头的秒数和模型看到的不一样，不报错）。
    //
    // **body 里给了就用给的**，和出图那边的 seed 一个规矩（见 ref_gen.hpp）：
    // 界面上的"再摇一次"就是不送这个字段，"还要刚才那个节奏"就是把上次的
    // 数送回来。对拍语料送的是 0，那一档和以前一字不差。
    const std::uint32_t variation =
        body.is_object() && body.contains("variation")
            // 下界写 -1 不是 0：num_in_range 的下界是**开区间**
            // （`v <= gt` 就报 422），而 0 是合法值——它正是"不浮动"那一档。
            ? static_cast<std::uint32_t>(
                  num_in_range(body, "variation", 0.0, -1.0, 4294967295.0))
            : stages::random_shape();
    if (plan != nullptr) {
        // 上一集的结尾拿来接语气。**按分集表的顺序取上一条**，不是按
        // project.episodes 的顺序——后者可能被手动加过集、插过预告片。
        // 章模式按章：上一章对应的那一集（chapter_refs 指着上一章的）。
        std::string prev_tail;
        if (chapter_mode && plan == &chapter_plan_storage) {
            const Chapter* prev_ch = nullptr;
            for (std::size_t i = 1; i < story.chapters.size(); ++i) {
                if (story.chapters[i].chapter_id == plan->from_chapter) {
                    prev_ch = &story.chapters[i - 1];
                    break;
                }
            }
            if (prev_ch != nullptr) {
                for (const Episode& e : project.episodes) {
                    if (std::find(e.chapter_refs.begin(), e.chapter_refs.end(),
                                  prev_ch->chapter_id) != e.chapter_refs.end()) {
                        prev_tail = stages::script_tail(e.script);
                        break;
                    }
                }
            }
        } else {
            for (std::size_t i = 0; i < story.plan.size(); ++i) {
                if (story.plan[i].episode_id != episode_id) continue;
                if (i == 0) break;
                const Episode* prev_ep =
                    project.episode_by_id(story.plan[i - 1].episode_id);
                if (prev_ep != nullptr) {
                    prev_tail = stages::script_tail(prev_ep->script);
                }
                break;
            }
        }
        // 形状每写一次摇一个新的（ComfyUI 的 randomize 那个意思）——**不是
        // 按集号哈希**，那样同一集永远是同一个形状，人不喜欢这一集的节奏也
        // 换不掉。不满意就再点一次「重新改编」，满意了点采用，形状跟着定下来。
        // 章模式没有形状：剧本照正文的场走，见 build_chapter_script_prompt。
        if (chapter_mode) {
            chapter_scenes = stages::chapter_scene_plan(story, *plan);
            prompt = stages::build_chapter_script_prompt(
                story, *plan, project.style_line, names, prev_tail, chapter_scenes);
        } else {
            prompt = stages::build_script_prompt_from_story(
                story, *plan, project.style_line, names, prev_tail, variation);
        }
        source = "story";
    } else {
        prompt = stages::build_script_prompt(
            premise, duration_s, project.style_line, previous, names, variation);
    }

    // 走故事那条时时长以分集表为准：那份表是按每集时长算出来的，
    // 请求里带的那个可能是页面上的旧值，用它算预算会和实际排的镜头对不上。
    const double used_duration =
        plan != nullptr ? plan->target_duration_s : duration_s;

    llm::Request req;
    req.prompt = prompt;
    // 四段的 schema，每段的拍数地板按这一集的时长算。**地板是这一步唯一
    // 管用的东西**：提示词里"要凑够"模型不听，minItems 4 它就写 4 拍——
    // 实测 60 秒的集写出 13 秒的剧本，就是从这儿来的。
    //
    // 名字同理：提示词里说了「一字不改」，实跑还是写出了林浩 / Lin Hao /
    // LinHao / Su Wan 四种。收成枚举，和分镜那边收 char_id 是一个道理。
    const bool chapter_script = chapter_mode && plan != nullptr;
    req.schema = chapter_script
                     ? stages::script_schema_for_chapter(chapter_scenes, names)
                     : stages::script_schema(used_duration, names, variation);
    req.schema_name = "script";
    req.on_thinking = thinking_sink();

    // 走故事那条叫「改编」，照梗概写叫「写」——按钮上的字就是这么分的
    // （EpScript.vue 里那个 writeLabel），这儿跟着它说，不然顶栏说的和
    // 用户刚点的那个按钮对不上。
    pipeline::Activity act{"script", paths::to_utf8(store.root()), episode_id,
                           plan != nullptr ? "正在改编成剧本" : "正在写剧本"};
    const stages::ScriptDraft draft = llm_guard([&] {
        const std::string raw = client.complete(req, tok);
        return chapter_script
                   ? stages::parse_chapter_script(raw, chapter_scenes)
                   : stages::parse_script(raw, used_duration, variation);
    });

    // 梗概存到项目上。下次写新一集时直接回填，不用凭记忆重打。
    //
    // **写回之前重读一遍。** `project` 是这个请求一开头读的那一份，而上面
    // 那次生成跑了一两分钟——这几分钟里界面完全可能在改别的东西（镜头抽屉
    // 存一笔、改个集名、加一集）。这儿真正要改的只有 premise 一个字段，
    // 却要把整份旧 project 写回去，那些改动就被悄悄吞掉了。
    //
    // 同一个形状在批量那两条长任务上也有，理由写在 batch.cpp 里那两段。
    const std::string trimmed = text::strip_ws(premise);
    if (project.premise != trimmed) {
        Project latest = store.load_project();
        if (latest.premise != trimmed) {
            latest.premise = text::truncate_utf8(trimmed, 2000);
            store.save_project(latest);
        }
        // 本地这份也跟上：下面还要拿它拼回包
        project.premise = latest.premise;
    }

    const int budget = stages::budget_chars(used_duration);
    const auto chars = static_cast<double>(draft.dialogue_chars());
    json out = draft_common(draft, used_duration);
    // 写长了后面配音会把镜头撑爆，写短了成片不够时长，都得说出来
    // 章模式没有字数预算——长度由内容定，不判长短。
    out["fit"] = chapter_script          ? "合适"
                 : chars > budget * 1.35 ? "偏长"
                 : chars < budget * 0.6  ? "偏短"
                                         : "合适";
    out["continued_from"] = !previous.empty();
    out["reused_characters"] = names;
    // 这一集是照着故事写的还是照着一句梗概续的。前端靠它说清
    // 「这一集为什么是这些内容」，也让人一眼看出有没有走上新路子。
    out["source"] = source;
    if (plan != nullptr) {
        out["chapters"] = stages::episode_chapters(story, *plan);
        out["hook"] = plan->hook;
    }
    return {200, out};
}

ApiResult post_script_trailer(const json& body, llm::Client& client,
                              pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "duration_s", "episode_ids", "reuse_characters"});
    const double duration_s =
        num_in_range(body, "duration_s", 20.0, 0.0, 120.0);
    const bool reuse_chars = opt_bool(body, "reuse_characters", true);

    std::set<std::string> wanted;
    const auto ids = body.find("episode_ids");
    if (ids != body.end() && ids->is_array()) {
        for (const auto& v : *ids) {
            if (v.is_string()) wanted.insert(v.get<std::string>());
        }
    }

    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    const AssetLibrary assets = load_assets_or_400(store);

    std::vector<const Episode*> picked;
    for (const auto& ep : project.episodes) {
        if (text::strip_ws(ep.script).empty()) continue;
        // 预告片自己不该当自己的素材
        if (ep.episode_id == kTrailerEpisodeId) continue;
        if (!wanted.empty() && wanted.count(ep.episode_id) == 0) continue;
        picked.push_back(&ep);
    }
    if (picked.empty()) {
        throw ApiError(400, "没有可用来剪预告的剧集。先写几集正片，再回来剪预告");
    }



    std::string source;
    for (std::size_t i = 0; i < picked.size(); ++i) {
        if (i) source += "\n\n";
        source += "【" + picked[i]->episode_id + " " + picked[i]->title + "】\n" +
                  text::strip_ws(picked[i]->script);
    }

    const std::vector<std::string> names =
        reuse_chars ? character_names(assets) : std::vector<std::string>{};

    const std::string prompt = stages::build_trailer_prompt(
        project.premise, duration_s, project.style_line, source, names);

    llm::Request req;
    req.prompt = prompt;
    req.schema = stages::script_schema();  // 平的那份：预告片是蒙太奇，不分四段
    req.schema_name = "trailer";
    req.on_thinking = thinking_sink();

    pipeline::Activity act{"trailer", paths::to_utf8(store.root()), "",
                           "正在剪预告"};
    const stages::ScriptDraft draft = llm_guard([&] {
        return stages::parse_script(client.complete(req, tok));
    });

    const int budget = stages::budget_chars(duration_s);
    const auto chars = static_cast<double>(draft.dialogue_chars());
    json out = draft_common(draft, duration_s);
    // 预告片写长了比正片更要命：刷到第三秒还没看到钩子，人就划走了。
    // 所以上界比正片严得多——正片是 budget * 1.35，这里就是 budget。
    out["fit"] = chars > budget          ? "偏长"
                 : chars < budget * 0.35 ? "偏短"
                                         : "合适";
    json from = json::array();
    for (const Episode* ep : picked) from.push_back(ep->episode_id);
    out["from_episodes"] = from;
    out["episode_id"] = kTrailerEpisodeId;
    return {200, out};
}

}  // namespace changji::http
