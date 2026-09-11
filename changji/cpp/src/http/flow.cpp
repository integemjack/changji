#include "http/flow.hpp"

#include <set>
#include <string>
#include <vector>

namespace changji::http {

using nlohmann::json;

namespace {

/// 取字符串字段，没有或类型不对就回空串。
///
/// **不能用 `value<std::string>`**：字段是 null 的时候它会抛
/// type_error，而 `location_id` 这类字段没填就是 null，是正常状态。
std::string str_of(const json& o, const char* key) {
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

const json& arr_of(const json& o, const char* key) {
    static const json kEmpty = json::array();
    if (!o.is_object()) return kEmpty;
    const auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return kEmpty;
    return *it;
}

bool blank(const std::string& s) {
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

}  // namespace

json flow_steps() {
    // 和 flow.js 的 STEPS 逐字对齐。**前端按 key 认人**，
    // 改一个字侧边栏就少一格。
    return json::array({
        json{{"key", "project"}, {"phase", "series"}, {"title", "项目"},
             {"hint", "选一个项目，或者新建一个"}},
        // **故事在剧本前面。** 原来第二步就是「剧本大纲」，而那一页是
        // 拿一句梗概逐集续写——集数人填、上下文只带前三集。故事这一步
        // 先把完整故事和分集定下来，剧本才有得可写。
        json{{"key", "story"}, {"phase", "series"}, {"title", "故事"},
             {"hint", "讲什么、分几章、按每集时长切成几集"}},
        json{{"key", "script"}, {"phase", "series"}, {"title", "剧本大纲"},
             {"hint", "全剧讲什么、分几集、每集写什么"}},
        json{{"key", "characters"}, {"phase", "series"}, {"title", "角色"},
             {"hint", "从剧本提人物，全剧同一批"}},
        json{{"key", "scenes"}, {"phase", "episode"}, {"title", "场景"},
             {"hint", "这一集在哪儿拍"}},
        // **镜头、成片、上传 2026-09-11 合成一步「这一集」。**
        //
        // 2026-09-10 已经把分镜和制作合过一次（同一张分镜表，一页排一页跑，
        // 而人在同一镜上来回）。这次是同一条理由再往外一层：人做的事是
        // **对照着看**——对着这句台词看这一镜对不对，看完整集顺手发出去。
        // 分成几页，来回换页才知道这一镜出自哪句话。
        json{{"key", "episode"}, {"phase", "episode"}, {"title", "这一集"},
             {"hint", "剧本、镜头、成片、发布，都在这一集上"}},
    });
}

json flow_assess(const json& project, const json& shots, const json& outputs,
                 const std::string& episode_id, const json& story) {
    json done = json::object();
    json counters = json::object();

    // ---- 项目 ----
    done["project"] = !str_of(project, "project_id").empty();

    // ---- 故事：有章节就算有 ----
    //
    // 判据不看梗概：梗概是写故事的输入，光有梗概什么都还没发生。
    // 有章节就说明大纲写出来并且被采用过了。
    const auto& story_chapters = arr_of(story, "chapters");
    done["story"] = !story_chapters.empty();
    counters["chapters"] = story_chapters.size();
    counters["plannedEpisodes"] = arr_of(story, "plan").size();

    // ---- 剧本大纲：有梗概，而且至少有一集写过东西 ----
    const auto& episodes = arr_of(project, "episodes");
    int written = 0;
    for (const auto& e : episodes) {
        const bool has_shots = e.is_object() && e.contains("shots") &&
                               e["shots"].is_number() &&
                               e["shots"].get<int>() > 0;
        if (has_shots || !str_of(e, "synopsis").empty()) ++written;
    }
    done["script"] = !blank(str_of(project, "premise")) && written > 0;

    // ---- 角色 ----
    const auto& characters = arr_of(project, "characters");
    done["characters"] = !characters.empty();

    // ---- 场景 ----
    //
    // 三个条件都要：注册过场景、这一集用到的场景都注册过、
    // **而且没有"靠 scene_id 蒙对"的镜头**。
    // 最后一条是关键：`scene_id` 恰好等于某个 location_id 的时候画面能出，
    // 但那是巧合不是关联——这一步不算做完，用户得去显式关联一次。
    std::set<std::string> known;
    for (const auto& l : arr_of(project, "locations")) {
        const auto id = str_of(l, "location_id");
        if (!id.empty()) known.insert(id);
    }
    std::set<std::string> used;
    int unlinked = 0;
    std::vector<std::string> missing;
    for (const auto& s : shots) {
        const auto loc = str_of(s, "location_id");
        if (!loc.empty()) {
            used.insert(loc);
        } else {
            const auto scene = str_of(s, "scene_id");
            if (!scene.empty() && known.count(scene)) {
                used.insert(scene);
                ++unlinked;
            }
        }
    }
    for (const auto& id : used) {
        if (!known.count(id)) missing.push_back(id);
    }
    done["scenes"] = !known.empty() && missing.empty() && unlinked == 0;

    // ---- 镜头：每一镜都出到成片状态才算完 ----
    //
    // **判据取的是原来「制作」那条，不是「分镜」那条。** 合成一步之后
    // 这一格代表的是"这一集的镜头做完了"，光有分镜表不算做完——
    // 那时候一帧画面都还没有。宽松的判据会让侧边栏早早打勾，
    // 而用户回头发现什么都没出。
    static const std::set<std::string> kFinal = {"final_done", "locked",
                                                 "fallback"};
    int produced = 0;
    int lipsync = 0;
    double planned = 0.0;
    for (const auto& s : shots) {
        if (kFinal.count(str_of(s, "status"))) ++produced;
        if (s.is_object() && s.value("needs_lipsync", false)) ++lipsync;
        if (s.is_object() && s.contains("duration_s") &&
            s["duration_s"].is_number()) {
            planned += s["duration_s"].get<double>();
        }
    }
    const bool shots_done =
        !shots.empty() && produced == static_cast<int>(shots.size());

    // ---- 成片：产物里有这一集的 ----
    bool has_film = false;
    for (const auto& o : outputs) {
        const auto name = str_of(o, "name");
        if (episode_id.empty() || name.find(episode_id) != std::string::npos) {
            has_film = true;
            break;
        }
    }
    // ---- 这一集 ----
    //
    // **判据取原来「成片」那条**：装配出片子才算这一集做完了。
    //
    // 不取「镜头全出完」：那时候还没装配，拿不到片子。也不把投递算进来——
    // 投递记录存在 Node 那个 BFF 自己的配置里，引擎这边没有那份数据，
    // 而且发不发是可选的收尾动作，把它算进来会让这一格永远不打勾。
    done["episode"] = has_film;
    // 镜头有没有全出完另外报一个数。这一格的进度条和「还差几镜」都靠它，
    // 而判定本身用的是上面那条。
    counters["shotsDone"] = shots_done;

    counters["shots"] = static_cast<int>(shots.size());
    counters["produced"] = produced;
    counters["characters"] = static_cast<int>(characters.size());
    counters["locations"] = static_cast<int>(known.size());
    counters["episodeLocations"] = static_cast<int>(used.size());
    counters["missingLocations"] = missing;
    counters["unlinkedShots"] = unlinked;
    counters["episodesWritten"] = written;
    counters["lipsync"] = lipsync;
    counters["plannedDurationS"] = planned;
    counters["outputs"] = static_cast<int>(outputs.size());

    return json{{"done", done}, {"counters", counters}};
}

}  // namespace changji::http
