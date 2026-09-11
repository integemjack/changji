#include "http/planning.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "http/reset.hpp"
#include "models/project.hpp"
#include "stages/bible.hpp"
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

std::string opt_str(const json& body, const char* key, const std::string& def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

bool opt_bool(const json& body, const char* key, bool def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

double opt_num(const json& body, const char* key, double def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_number()) return def;
    return it->get<double>();
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

AssetLibrary load_assets_or_400(const ProjectStore& store) {
    try {
        return store.load_assets();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

/// 把生成阶段的异常翻成 400。
///
/// Python 那边 /api/bible 只 catch BibleError、/api/plan 分别 catch
/// BibleError 和 StoryboardError，而 _extract_json 抛的是 StoryboardError——
/// 于是 /api/bible 上"模型吐了一坨不是 JSON 的东西"这个最常见的失败
/// 穿到最外面变成 500。这里三种都接住，统一回 400。
/// 有意的偏离，详见方案里那一节。
template <typename F>
auto stage_guard(F&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const stages::BibleError& e) {
        throw ApiError(400, e.what());
    } catch (const stages::StoryboardError& e) {
        throw ApiError(400, e.what());
    } catch (const llm::LlmError& e) {
        throw ApiError(400, e.what());
    }
}

AssetLibrary generate_bible(const std::string& script, StyleLine style_line,
                            llm::Client& client, pipeline::CancelToken& tok) {
    llm::Request req;
    req.prompt = stages::build_bible_prompt(script, style_line);
    req.schema = stages::bible_schema();
    req.schema_name = "bible";
    return stage_guard([&] {
        return stages::parse_bible(client.complete(req, tok), style_line);
    });
}

/// 从故事出圣经。名单已经在故事里了，这一步只定妆。
///
/// 和上面那个的区别见 stages/bible.hpp：老的那条是"读一集剧本找出角色"，
/// 于是全剧共用的资产库其实是从第一集推出来的。
AssetLibrary generate_bible_from_story(const Story& story, StyleLine style_line,
                                       llm::Client& client,
                                       pipeline::CancelToken& tok) {
    llm::Request req;
    req.prompt = stages::build_bible_prompt_from_story(story, style_line);
    req.schema = stages::bible_schema();
    req.schema_name = "bible";
    return stage_guard([&] {
        return stages::parse_bible(client.complete(req, tok), style_line);
    });
}

Story load_story_or_400(const ProjectStore& store) {
    try {
        return store.load_story();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }
}

/// 把新出的设定合进资产库，并拼出回包。
///
/// 从故事出和从剧本出两条路走到这里是一样的，所以提出来：合并规则、
/// 重跑镜头的判断、回包形状都只该有一份。source 只是告诉前端这次的名单
/// 是从哪来的——「为什么这次多出来三个人」全靠它解释。
ApiResult merge_bible(const ProjectStore& store, AssetLibrary assets,
                      const AssetLibrary& fresh, bool overwrite,
                      const char* source) {
    // 合并，不是替换。同名的默认保留旧的：手改过的设定、传过的参考图
    // 都挂在旧的那一份上。勾了覆盖才让新的顶掉。
    std::vector<std::string> added_c, added_l;
    int kept = 0;
    for (const auto& kv : fresh.characters) {
        const bool exists = assets.characters.contains(kv.first);
        if (exists && !overwrite) {
            ++kept;
            continue;
        }
        if (!exists) added_c.push_back(kv.first);
        assets.characters[kv.first] = kv.second;
    }
    for (const auto& kv : fresh.locations) {
        const bool exists = assets.locations.contains(kv.first);
        if (exists && !overwrite) {
            ++kept;
            continue;
        }
        if (!exists) added_l.push_back(kv.first);
        assets.locations[kv.first] = kv.second;
    }
    store.save_assets(assets);

    // 外观变了等于全剧提示词都变了，已渲染的镜头得退回重跑。
    // 只新增没覆盖的话，老镜头用的还是原来那份设定，不用动。
    const int reset = overwrite ? reset_all_shots(store) : 0;

    const auto is_new_c = [&added_c](const std::string& id) {
        return std::find(added_c.begin(), added_c.end(), id) != added_c.end();
    };
    const auto is_new_l = [&added_l](const std::string& id) {
        return std::find(added_l.begin(), added_l.end(), id) != added_l.end();
    };

    json chars = json::array();
    for (const auto& kv : assets.characters) {
        const Character& c = kv.second;
        chars.push_back({{"char_id", c.char_id},
                         {"name", c.name},
                         {"identity", c.appearance.identity},
                         {"face", c.appearance.face},
                         {"attire", c.appearance.attire},
                         {"is_new", is_new_c(c.char_id)}});
    }
    json locs = json::array();
    for (const auto& kv : assets.locations) {
        const Location& l = kv.second;
        locs.push_back({{"location_id", l.location_id},
                        {"name", l.name},
                        {"space", l.space},
                        {"lighting", l.lighting},
                        {"is_new", is_new_l(l.location_id)}});
    }

    return {200, {
        {"added_characters", added_c},
        {"added_locations", added_l},
        {"kept", kept},
        {"characters", chars},
        {"locations", locs},
        {"reset_shots", reset},
        {"source", source},
    }};
}


/// 对应 Python 的 round(x, 1)：**银行家舍入**。
double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

}  // namespace

ApiResult post_bible(const json& body, llm::Client& client,
                     pipeline::CancelToken& tok) {
    forbid_extra(body, {"project", "episode_id", "script", "overwrite", "source"});
    const std::string episode_id = opt_str(body, "episode_id", "");
    const bool overwrite = opt_bool(body, "overwrite", false);

    ProjectStore store = open_project(body);
    const Project project = load_or_400(store);
    AssetLibrary assets = load_assets_or_400(store);

    // 名单从哪来。默认看项目里有没有故事——有就从故事出，那份名单是全剧
    // 完整的；没有就退回老路径从一集剧本里找，老项目还得能用。
    const std::string source = opt_str(body, "source", "auto");
    if (source != "auto" && source != "story" && source != "script") {
        throw unprocessable_top("source",
                                "Input should be 'auto', 'story' or 'script'",
                                body.at("source"), "enum");
    }
    const Story story = load_story_or_400(store);
    if (source == "story" && story.chapters.empty()) {
        throw ApiError(400, "这个项目还没有故事，先去写一份大纲");
    }
    if (source != "script" && !story.chapters.empty()) {
        const AssetLibrary from_story =
            generate_bible_from_story(story, project.style_line, client, tok);
        return merge_bible(store, std::move(assets), from_story, overwrite,
                           "story");
    }

    std::string script = text::strip_ws(opt_str(body, "script", ""));
    if (script.empty() && !episode_id.empty()) {
        const Episode* ep = project.episode_by_id(episode_id);
        script = ep ? text::strip_ws(ep->script) : std::string();
    }
    if (script.empty()) {
        // 没指定就用第一集有内容的剧本。角色设定是全剧共用的，
        // 拿哪一集出都行，但总得有一集写好了。
        for (const auto& ep : project.episodes) {
            const std::string s = text::strip_ws(ep.script);
            if (!s.empty()) {
                script = s;
                break;
            }
        }
    }
    if (script.empty()) throw ApiError(400, "还没有剧本，先去写一集");

    const AssetLibrary fresh =
        generate_bible(script, project.style_line, client, tok);

    return merge_bible(store, std::move(assets), fresh, overwrite,
                       "script");
}

ApiResult post_plan(const json& body, llm::Client& client,
                    pipeline::CancelToken& tok) {
    // 注意这里**没有** forbid_extra。Python 的 PlanRequest 没写
    // model_config = {"extra": "forbid"}，pydantic 默认是忽略多余字段。
    // 加上校验就会拒掉 Python 能接受的请求。
    const std::string script = text::strip_ws(need_str(body, "script"));
    if (script.empty()) throw ApiError(400, "剧本是空的");

    const std::string episode_id = opt_str(body, "episode_id", "ep01");
    const double duration_s = opt_num(body, "duration_s", 60.0);
    const bool regenerate = opt_bool(body, "regenerate_bible", false);

    ProjectStore store = open_project(body);
    Project project = load_or_400(store);
    AssetLibrary assets = load_assets_or_400(store);

    // 角色设定。已有就不重做，避免覆盖用户改过的设定。
    if (regenerate || assets.characters.empty()) {
        // 有故事就从故事出——名单是全剧完整的，不是从这一集里找出来的。
        const Story story = load_story_or_400(store);
        assets = story.chapters.empty()
                     ? generate_bible(script, project.style_line, client, tok)
                     : generate_bible_from_story(story, project.style_line,
                                                 client, tok);
        store.save_assets(assets);
    }

    llm::Request req;
    req.prompt = stages::build_storyboard_prompt(
        script, assets, stages::DurationQuota::for_duration(duration_s),
        episode_id);
    req.schema = stages::llm_shot_schema(assets);
    req.schema_name = "storyboard";

    std::vector<Shot> shots = stage_guard([&] {
        std::vector<Shot> s = stages::parse_storyboard(client.complete(req, tok),
                                                       assets);
        // 覆盖检查在解析之后、口型推导之前。大模型很容易只写画面不写台词，
        // 产出一部哑剧——这类问题在生成阶段就该检出，不该等到配音阶段
        // 发现一句话都没有。
        const auto gaps = stages::check_coverage(script, s);
        if (!gaps.empty()) {
            std::string msg = "分镜表不完整：";
            for (const auto& g : gaps) msg += "\n" + g;
            msg += "\n\n换一个更强的模型，或者手工补齐这些字段后再跑。";
            throw stages::StoryboardError(msg);
        }
        apply_lipsync_rules(s);
        return s;
    });

    Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) {
        Episode fresh;
        fresh.episode_id = episode_id;
        fresh.target_duration_s = duration_s;
        project.episodes.push_back(std::move(fresh));
        ep = &project.episodes.back();
    }
    ep->script = need_str(body, "script");   // 存原文，不是 strip 过的
    ep->target_duration_s = duration_s;
    ep->shots = shots;
    store.save_project(project);

    double total = 0.0;
    int lipsync = 0;
    for (const Shot& s : shots) {
        total += s.duration_s;
        if (s.needs_lipsync) ++lipsync;
    }

    json chars = json::array();
    for (const auto& kv : assets.characters) {
        chars.push_back({{"char_id", kv.second.char_id},
                         {"name", kv.second.name},
                         {"face", kv.second.appearance.face}});
    }
    json locs = json::array();
    for (const auto& kv : assets.locations) {
        locs.push_back({{"location_id", kv.second.location_id},
                        {"name", kv.second.name}});
    }

    return {200, {
        {"episode_id", episode_id},
        {"shots", shots.size()},
        {"duration_s", round1(total)},
        {"lipsync", lipsync},
        {"characters", chars},
        {"locations", locs},
    }};
}

}  // namespace changji::http
