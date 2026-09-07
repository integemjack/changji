#include "http/readonly.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "models/hardware.hpp"
#include "models/project.hpp"
#include "util/fs_time.hpp"
#include "util/paths.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

/// Python 的 round(x, 1)。银行家舍入，不是 std::round。
/// 详见 models/hardware.cpp 里的同名函数和它上面那段注释。
double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

/// 对应 Python 的 _store(path)：路径为空是 400 不是 500。
ProjectStore store_for(const std::string& path) {
    if (path.empty()) throw ApiError(400, "没有指定项目目录");
    return ProjectStore(paths::from_utf8(path));
}

/// 三个档位的参数，/api/hardware 和 /api/settings 都要。
json tiers_json(const std::map<Tier, TierSpec>& tiers, bool with_seconds) {
    json out = json::object();
    for (Tier t : all_tiers()) {
        const auto it = tiers.find(t);
        if (it == tiers.end()) continue;
        const TierSpec& s = it->second;
        json entry = {
            {"width", s.width},
            {"height", s.height},
            {"steps", s.steps},
        };
        if (with_seconds) {
            entry["seconds"] = s.measured_seconds.has_value()
                                   ? json(*s.measured_seconds)
                                   : json(nullptr);
        }
        out[to_string(t)] = entry;
    }
    return out;
}

/// std::optional<std::string> 转 JSON：无值是 null，对应 Python 的 None。
json opt(const std::optional<std::string>& v) {
    return v.has_value() ? json(*v) : json(nullptr);
}

}  // namespace

ApiResult get_hardware(const config::Settings& settings) {
    return get_hardware(settings,
                        HardwareProfile::detect(settings.vram_gb_override));
}

ApiResult get_hardware(const config::Settings& settings,
                       const HardwareProfile& p) {
    (void)settings;
    return {200, {
        {"gpu", p.gpu.has_value() ? json(p.gpu->name) : json(nullptr)},
        {"vram_gb", round1(p.vram_gb)},
        {"detected", p.detected},
        {"tiers", tiers_json(p.tiers, /*with_seconds=*/true)},
    }};
}

ApiResult get_settings(const config::Settings& s) {
    return get_settings(s, HardwareProfile::detect(s.vram_gb_override));
}

ApiResult get_settings(const config::Settings& s, const HardwareProfile& p) {
    return {200, {
        {"tiers", tiers_json(p.tiers, /*with_seconds=*/false)},
        {"assembly", {
            {"fps", s.assembly.fps},
            {"crf", s.assembly.crf},
            {"subtitle_font", s.assembly.subtitle_font},
            {"subtitle_max_chars_per_line", s.assembly.subtitle_max_chars_per_line},
            {"subtitle_max_lines", s.assembly.subtitle_max_lines},
            {"scene_transition_s", s.assembly.scene_transition_s},
        }},
        {"gates", {
            {"enabled", s.gates.enabled},
            {"max_attempts_per_shot", s.gates.max_attempts_per_shot},
            {"min_pixel_std", s.gates.min_pixel_std},
            {"min_frame_similarity", s.gates.min_frame_similarity},
            {"max_audio_drift_s", s.gates.max_audio_drift_s},
            {"target_lufs", s.gates.target_lufs},
            {"fallback_on_exhausted", s.gates.fallback_on_exhausted},
        }},
        {"tts", {
            {"tolerance_s", s.tts.tolerance_s},
            {"max_tempo_shift", s.tts.max_tempo_shift},
        }},
    }};
}

ApiResult get_project(const std::string& path) {
    ProjectStore store = store_for(path);
    Project p;
    AssetLibrary assets;
    try {
        p = store.load_project();
        assets = store.load_assets();
    } catch (const std::exception& e) {
        // Python 那边 catch 的是 FileNotFoundError 和 ValueError，都转 400
        throw ApiError(400, e.what());
    }

    json characters = json::array();
    for (const auto& kv : assets.characters) {
        characters.push_back({{"char_id", kv.second.char_id},
                              {"name", kv.second.name}});
    }
    json locations = json::array();
    for (const auto& kv : assets.locations) {
        locations.push_back({{"location_id", kv.second.location_id},
                             {"name", kv.second.name}});
    }
    json episodes = json::array();
    for (const auto& e : p.episodes) {
        json status = json::object();
        for (const auto& kv : e.counts_by_status()) status[kv.first] = kv.second;
        episodes.push_back({
            {"episode_id", e.episode_id},
            {"title", e.title},
            {"synopsis", e.synopsis},
            {"shots", static_cast<int>(e.shots.size())},
            {"duration_s", round1(e.planned_duration_s())},
            {"status", status},
        });
    }

    return {200, {
        {"project_id", p.project_id},
        {"title", p.title},
        {"style_line", to_string(p.style_line)},
        // 这部剧讲什么。剧本页拿它回填梗概框，不用凭记忆重打。
        {"premise", p.premise},
        {"root", paths::to_utf8(store.root())},
        {"characters", characters},
        {"locations", locations},
        {"episodes", episodes},
    }};
}

ApiResult get_shots(const std::string& path, const std::string& episode_id) {
    ProjectStore store = store_for(path);
    const Project p = store.load_project();
    const Episode* ep = p.episode_by_id(episode_id);
    if (ep == nullptr) throw ApiError(404, "没有剧集 " + episode_id);

    json shots = json::array();
    for (const auto& s : ep->sorted_shots()) {
        json char_ids = json::array();
        for (const auto& c : s.characters) char_ids.push_back(c.char_id);

        json dialogue = json::array();
        json audio_paths = json::array();
        for (const auto& d : s.dialogue) {
            dialogue.push_back({
                {"char_id", opt(d.char_id)},
                {"text", d.text},
                {"duration_s", d.actual_duration_s.has_value()
                                   ? json(*d.actual_duration_s)
                                   : json(nullptr)},
            });
            if (d.audio_path.has_value() && !d.audio_path->empty()) {
                audio_paths.push_back(*d.audio_path);
            }
        }

        shots.push_back({
            {"shot_id", s.shot_id},
            {"order", s.order},
            {"scene_id", s.scene_id},
            // 这一集用到哪几个场景，界面靠它算。缺了的话场景页
            // 只能把全剧的场景一股脑列出来，看不出跟本集的关系。
            {"location_id", opt(s.location_id)},
            {"char_ids", char_ids},
            {"shot_size", to_string(s.shot_size)},
            {"camera_angle", to_string(s.camera_angle)},
            {"camera_move", to_string(s.camera_move)},
            // 编辑器要回填这些，缺了的话打开是空的，
            // 用户以为本来就没内容，一保存就把原文清空了
            {"first_frame_prompt", s.first_frame_prompt},
            {"motion_prompt", s.motion_prompt},
            {"negative_prompt", s.negative_prompt},
            {"subtitle_text", s.subtitle_text},
            {"transition_in", to_string(s.transition_in)},
            {"transition_dur_s", s.transition_dur_s},
            {"continuity_notes", s.continuity_notes},
            {"duration_s", s.duration_s},
            {"duration_locked", s.duration_locked},
            {"needs_lipsync", s.needs_lipsync},
            {"status", to_string(s.status)},
            {"attempts", s.attempts},
            {"visual_desc", s.visual_desc},
            {"dialogue", dialogue},
            {"gate_notes", s.gate_notes},
            {"has_video", s.video_path.has_value() && !s.video_path->empty()},
            // 审片要用。只给相对路径，取文件走 /api/media，
            // 那里做了越界检查，不会读到项目目录之外。
            {"frame_path", opt(s.frame_path)},
            {"video_path", opt(s.video_path)},
            {"audio_paths", audio_paths},
            {"beat", s.beat},
        });
    }
    return {200, {{"shots", shots}}};
}

ApiResult get_assets(const std::string& path) {
    ProjectStore store = store_for(path);
    AssetLibrary assets;
    try {
        assets = store.load_assets();
    } catch (const std::exception& e) {
        throw ApiError(400, e.what());
    }

    // 参考图只有图像工作流那条路会用。默认装机没有 image.json，
    // 首帧走视频模型，而那条路直接忽略参考图。
    // 不说清楚的话，用户传了图、镜头也退回重跑了，画面却一点没变。
    std::error_code ec;
    const bool has_image_wf =
        fs::is_regular_file(store.root() / "workflows" / "image.json", ec);

    const StyleLine line = assets.style.style_line;

    json characters = json::array();
    for (const auto& kv : assets.characters) {
        const Character& c = kv.second;
        characters.push_back({
            {"char_id", c.char_id},
            {"name", c.name},
            {"identity", c.appearance.identity},
            {"body", c.appearance.body},
            {"face", c.appearance.face},
            {"attire", c.appearance.attire},
            {"style", c.appearance.style},
            {"voice_id", opt(c.voice_id)},
            // 猜出来的性别。音色留空时按它挑，猜错了用户得看得见
            {"voice_gender", c.voice_gender},
            {"ref_front", opt(c.ref_front)},
            {"ref_three_quarter", opt(c.ref_three_quarter)},
            {"ref_back", opt(c.ref_back)},
            {"lora_trigger", opt(c.lora_trigger)},
            {"lora_strength", c.lora_strength},
            {"rendered", c.render_prompt(line)},
        });
    }

    json locations = json::array();
    for (const auto& kv : assets.locations) {
        const Location& l = kv.second;
        locations.push_back({
            {"location_id", l.location_id},
            {"name", l.name},
            {"space", l.space},
            {"lighting", l.lighting},
            {"palette", l.palette},
            // 空景图是场景一致性的锚点。以前没回传，界面上就没法
            // 显示也没法换，等于这个字段只有命令行够得着。
            {"ref_empty", opt(l.ref_empty)},
            {"rendered", l.render_prompt(line)},
        });
    }

    return {200, {
        {"reference_images_used", has_image_wf},
        {"reference_hint", has_image_wf ? "" :
            "当前用视频模型出首帧，这条路不看参考图。"
            "要让参考图生效，把一个图像工作流存成项目里的 "
            "workflows/image.json。"},
        {"characters", characters},
        {"locations", locations},
        {"style", {
            {"style_line", to_string(assets.style.style_line)},
            {"global_style", assets.style.global_style},
            {"negative_prompt", assets.style.negative_prompt},
            {"aspect_ratio", assets.style.aspect_ratio},
        }},
    }};
}

ApiResult get_projects(const config::Settings& settings) {
    // 项目库里有哪些项目。
    //
    // 以前只能手打一条绝对路径。在容器里跑的时候那是
    // /data/projects/剧名 这种路径，用户根本不知道该填什么。
    const fs::path root = settings.workspace_path();
    std::error_code ec;

    struct Item {
        json body;
        double mtime = 0.0;
    };
    std::vector<Item> items;

    if (fs::is_directory(root, ec)) {
        // Python 那边是 sorted(root.iterdir())，directory_iterator 不保证顺序，
        // 先收齐再排。虽然最后按 mtime 重排，但 mtime 相同的条目要稳定。
        std::vector<fs::path> children;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            children.push_back(e.path());
        }
        std::sort(children.begin(), children.end());

        for (const auto& child : children) {
            if (!fs::is_directory(child, ec)) continue;
            const std::string dir_name = paths::to_utf8(child.filename());
            if (!dir_name.empty() && dir_name.front() == '.') continue;

            ProjectStore store(child);
            if (!store.exists()) continue;

            Project project;
            try {
                project = store.load_project();
            } catch (const std::exception& e) {
                items.push_back({json{{"path", paths::to_utf8(child)},
                                      {"dir", dir_name},
                                      {"name", dir_name},
                                      {"broken", e.what()}},
                                 0.0});
                continue;
            }

            int shots = 0, done = 0;
            for (const auto& ep : project.episodes) {
                shots += static_cast<int>(ep.shots.size());
                for (const auto& sh : ep.shots) {
                    if (sh.status == ShotStatus::FINAL_DONE ||
                        sh.status == ShotStatus::LOCKED) {
                        ++done;
                    }
                }
            }

            int outputs = 0;
            const fs::path out_dir = store.paths().output();
            if (fs::is_directory(out_dir, ec)) {
                for (const auto& f : fs::directory_iterator(out_dir, ec)) {
                    if (f.path().extension() == ".mp4") ++outputs;
                }
            }

            // Unix 秒。**不能直接用 time_since_epoch()**：
            // file_time_type 的纪元由实现定，MSVC 用的是 1601-01-01，
            // 一个刚写的文件在那上面是一百三十多亿秒。前端的 humanAgo()
            // 按 Unix 秒算，会得出 2381 年、然后一律显示"刚刚"——
            // 每个项目都显示"刚刚"，看起来像是没坏。
            double mtime = 0.0;
            const fs::path pf = store.paths().project_file();
            if (fs::is_regular_file(pf, ec)) {
                mtime = util::file_mtime_unix(pf);
            }

            items.push_back({json{
                {"path", paths::to_utf8(child)},
                // 目录名单独给，前端不该自己去切路径分隔符，
                // 那段代码在 Windows 和容器里得写两套
                {"dir", dir_name},
                {"name", project.title.empty() ? dir_name : project.title},
                {"style_line", to_string(project.style_line)},
                {"episodes", static_cast<int>(project.episodes.size())},
                {"shots", shots},
                {"done_shots", done},
                {"outputs", outputs},
                {"mtime", mtime},
            }, mtime});
        }
    }

    // 新的在前。stable_sort 保证 mtime 相同时保持目录名顺序，
    // 对应 Python 那边 sorted() 的稳定性。
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.mtime > b.mtime; });

    json projects = json::array();
    for (auto& it : items) projects.push_back(std::move(it.body));

    return {200, {
        {"workspace", paths::to_utf8(root)},
        {"projects", projects},
    }};
}

}  // namespace changji::http
