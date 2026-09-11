#include "http/scripting.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "models/project.hpp"
#include "models/story.hpp"
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
                        "continue_from_previous", "reuse_characters"});
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
    if (!episode_id.empty()) {
        try {
            story = store.load_story();
        } catch (const std::exception&) {
            // 读不了就当没有，退回老路径。老项目本来就没有这个文件。
        }
        for (const auto& p : story.plan) {
            if (p.episode_id == episode_id) {
                plan = &p;
                break;
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

    std::string prompt;
    const char* source = "premise";
    if (plan != nullptr) {
        // 上一集的结尾拿来接语气。**按分集表的顺序取上一条**，不是按
        // project.episodes 的顺序——后者可能被手动加过集、插过预告片。
        std::string prev_tail;
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
        prompt = stages::build_script_prompt_from_story(
            story, *plan, project.style_line, names, prev_tail);
        source = "story";
    } else {
        prompt = stages::build_script_prompt(premise, duration_s,
                                             project.style_line, previous, names);
    }

    llm::Request req;
    req.prompt = prompt;
    req.schema = stages::script_schema();
    req.schema_name = "script";

    const stages::ScriptDraft draft = llm_guard([&] {
        return stages::parse_script(client.complete(req, tok));
    });

    // 梗概存到项目上。下次写新一集时直接回填，不用凭记忆重打。
    const std::string trimmed = text::strip_ws(premise);
    if (project.premise != trimmed) {
        project.premise = text::truncate_utf8(trimmed, 2000);
        store.save_project(project);
    }

    // 走故事那条时时长以分集表为准：那份表是按每集时长算出来的，
    // 请求里带的那个可能是页面上的旧值，用它算预算会和实际排的镜头对不上。
    const double used_duration =
        plan != nullptr ? plan->target_duration_s : duration_s;
    const int budget = stages::budget_chars(used_duration);
    const auto chars = static_cast<double>(draft.dialogue_chars());
    json out = draft_common(draft, used_duration);
    // 写长了后面配音会把镜头撑爆，写短了成片不够时长，都得说出来
    out["fit"] = chars > budget * 1.35   ? "偏长"
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
    req.schema = stages::script_schema();  // 和正片共用
    req.schema_name = "trailer";

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
