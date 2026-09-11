#include "http/episodes.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "stages/storyboard.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

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

std::string ep_fmt(int n) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "ep%02d", n);
    return std::string(buf);
}

/// 对应 re.fullmatch(r"[a-z0-9_]+", s)。
bool is_valid_episode_id(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

/// 把 shot_id / scene_id 里的旧集号换成新的，**只换第一处**。
/// 对应 Python 的 s.replace(old, new, 1)。
std::string replace_first(const std::string& s, const std::string& from,
                          const std::string& to) {
    if (from.empty()) return s;
    const std::size_t pos = s.find(from);
    if (pos == std::string::npos) return s;
    return s.substr(0, pos) + to + s.substr(pos + from.size());
}

}  // namespace

std::string next_episode_id(const Project& project) {
    std::set<std::string> existing;
    int n = 1;
    for (const auto& ep : project.episodes) {
        existing.insert(ep.episode_id);
        if (ep.episode_id.size() > 2 && ep.episode_id.compare(0, 2, "ep") == 0) {
            const bool all_digits =
                std::all_of(ep.episode_id.begin() + 2, ep.episode_id.end(),
                            [](unsigned char c) { return c >= '0' && c <= '9'; });
            if (all_digits) ++n;
        }
    }
    while (existing.count(ep_fmt(n))) ++n;
    return ep_fmt(n);
}

ApiResult get_script(const std::string& path, const std::string& episode_id) {
    ProjectStore store = open_project(path);
    const Project project = load_or_400(store);
    const Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, "没有剧集 " + episode_id);
    return {200, {
        {"script", ep->script},
        {"target_duration_s", ep->target_duration_s},
        {"title", ep->title},
    }};
}

ApiResult post_script(const json& body, llm::Client& client,
                      pipeline::CancelToken& tok) {
    // 没有 extra="forbid"：Python 的 ScriptUpdateRequest 没写 model_config，
    // pydantic 默认忽略多余字段。加上校验会拒掉它能接受的请求。
    const std::string episode_id = need_str(body, "episode_id");
    const std::string script = need_str(body, "script");
    const bool regenerate = opt_bool(body, "regenerate", false);

    ProjectStore store = open_project(need_str(body, "project"));
    Project project = load_or_400(store);
    Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, "没有剧集 " + episode_id);

    ep->script = script;
    // 注意是 `if req.duration_s:` 不是 `is not None`——**0 也算没给**。
    // 照抄：给 0 的话下面按原时长走，不会把这一集设成零秒。
    const auto dit = body.find("duration_s");
    if (dit != body.end() && dit->is_number() && dit->get<double>() != 0.0) {
        ep->target_duration_s = dit->get<double>();
    }
    // 梗概。**空字符串不算给了**——手工存剧本时前端不带这一项，
    // 带了也可能是空的，那种情况下不能把已有的梗概洗掉。
    //
    // 以前这一项根本不收，结果单集这条路上 synopsis 永远是空的
    // （只有「批量写整季」会写）。连着坏三处：剧本页的「已写 N 集」
    // 一直是 0、「AI 剪一条预告片」永远点不亮、下次写新一集时
    // build_premise_prompt/build_script_prompt 的 existing 也是空的——
    // 「接着前几集写」勾了等于没勾，上下文断在这儿。
    const std::string synopsis = text::strip_ws(opt_str(body, "synopsis", ""));
    if (!synopsis.empty()) {
        ep->synopsis = text::truncate_utf8(synopsis, 2000);
    }

    json out = {{"saved", true}, {"regenerated", false}};
    if (regenerate) {
        const AssetLibrary assets = store.load_assets();
        llm::Request req;
        req.prompt = stages::build_storyboard_prompt(
            script, assets,
            stages::DurationQuota::for_duration(ep->target_duration_s),
            episode_id);
        req.schema = stages::llm_shot_schema(assets);
        req.schema_name = "storyboard";

        try {
            std::vector<Shot> shots =
                stages::parse_storyboard(client.complete(req, tok), assets);
            const auto gaps = stages::check_coverage(script, shots);
            if (!gaps.empty()) {
                std::string msg = "分镜表不完整：";
                for (const auto& g : gaps) msg += "\n" + g;
                msg += "\n\n换一个更强的模型，或者手工补齐这些字段后再跑。";
                throw stages::StoryboardError(msg);
            }
            apply_lipsync_rules(shots);
            ep->shots = std::move(shots);
        } catch (const stages::StoryboardError& e) {
            throw ApiError(400, e.what());
        } catch (const llm::LlmError& e) {
            throw ApiError(400, e.what());
        }
        out["regenerated"] = true;
        out["shots"] = ep->shots.size();
    }
    store.save_project(project);
    return {200, out};
}

ApiResult post_episode(const json& body) {
    const std::string title = opt_str(body, "title", "");
    double target = 60.0;
    const auto tit = body.find("target_duration_s");
    if (tit != body.end() && tit->is_number()) target = tit->get<double>();

    ProjectStore store = open_project(need_str(body, "project"));
    Project project = load_or_400(store);

    std::string ep_id = text::strip_ws(opt_str(body, "episode_id", ""));
    if (ep_id.empty()) ep_id = next_episode_id(project);
    if (!is_valid_episode_id(ep_id)) {
        throw ApiError(400, "剧集 id 只能用小写字母、数字和下划线");
    }
    if (project.episode_by_id(ep_id) != nullptr) {
        throw ApiError(409, "剧集 " + ep_id + " 已存在");
    }

    Episode ep;
    ep.episode_id = ep_id;
    ep.title = title;
    ep.target_duration_s = target;
    project.episodes.push_back(std::move(ep));
    store.save_project(project);
    return {200, {{"episode_id", ep_id}, {"title", title}}};
}

ApiResult post_episode_action(const json& body) {
    const std::string episode_id = need_str(body, "episode_id");
    const std::string action = need_str(body, "action");

    ProjectStore store = open_project(need_str(body, "project"));
    Project project = load_or_400(store);
    Episode* ep = project.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, "没有剧集 " + episode_id);

    if (action == "delete") {
        // 至少留一集。删空了界面会退回"还没有项目"的状态，
        // 用户以为整个项目没了。
        if (project.episodes.size() <= 1) throw ApiError(400, "至少要留一集");
        project.episodes.erase(
            std::remove_if(project.episodes.begin(), project.episodes.end(),
                           [&](const Episode& e) {
                               return e.episode_id == episode_id;
                           }),
            project.episodes.end());
        store.save_project(project);
        return {200, {{"deleted", episode_id}}};
    }

    if (action == "rename") {
        ep->title = opt_str(body, "new_title", "");
        const std::string title = ep->title;
        store.save_project(project);
        return {200, {{"renamed", episode_id}, {"title", title}}};
    }

    if (action == "duplicate") {
        // 编号规则和 next_episode_id **不一样**：这里数的是全部剧集数，
        // 那边只数 epNN。Python 侧就是这么写的，照抄。
        //
        // 差别在于项目里有 trailer 时：新建走 next_episode_id 得到 ep03，
        // 复制走这里得到 ep04（因为 trailer 也被数了）。两个入口给出不同的
        // 编号，看着像 bug，但改了会让两边的项目文件对不上。
        std::set<std::string> existing;
        for (const auto& e : project.episodes) existing.insert(e.episode_id);
        int n = static_cast<int>(project.episodes.size()) + 1;
        while (existing.count(ep_fmt(n))) ++n;
        const std::string new_id = ep_fmt(n);

        std::vector<Shot> shots;
        for (const Shot& s : ep->sorted_shots()) {
            Shot copy = s;
            copy.shot_id = replace_first(copy.shot_id, episode_id, new_id);
            copy.scene_id = replace_first(copy.scene_id, episode_id, new_id);
            // 产出物和状态不带过去，复制出来的是要重跑的。
            // 带过去的话它会显示成已完成但没有文件，点播放是黑的。
            copy.status = ShotStatus::PLANNED;
            copy.attempts = 0;
            copy.gate_notes.clear();
            copy.frame_path = std::nullopt;
            copy.video_path = std::nullopt;
            copy.duration_locked = false;
            for (auto& line : copy.dialogue) {
                line.audio_path = std::nullopt;
                line.actual_duration_s = std::nullopt;
            }
            shots.push_back(std::move(copy));
        }

        Episode fresh;
        fresh.episode_id = new_id;
        fresh.title = (ep->title.empty() ? episode_id : ep->title) + " 副本";
        fresh.script = ep->script;
        fresh.target_duration_s = ep->target_duration_s;
        fresh.shots = std::move(shots);
        const std::size_t count = fresh.shots.size();
        project.episodes.push_back(std::move(fresh));
        store.save_project(project);
        return {200, {{"episode_id", new_id}, {"shots", count}}};
    }

    throw ApiError(400, "不认识的操作 " + action + "，可选：delete、duplicate、rename");
}

}  // namespace changji::http
