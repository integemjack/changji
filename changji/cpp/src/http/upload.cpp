#include "http/upload.hpp"
#include "http/reset.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <set>

#include "models/project.hpp"
#include "util/paths.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

/// 三种格式对应的扩展名。顺序固定，清理旧文件时要按这个表遍历。
const std::array<std::pair<const char*, const char*>, 3>& ref_types() {
    static const std::array<std::pair<const char*, const char*>, 3> kTypes = {{
        {"image/png", ".png"},
        {"image/jpeg", ".jpg"},
        {"image/webp", ".webp"},
    }};
    return kTypes;
}

std::string need_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        throw ApiError(400, std::string("请求里缺少字符串字段 ") + key);
    }
    return body.at(key).get<std::string>();
}

ProjectStore open_project(const std::string& path) {
    if (path.empty()) throw ApiError(400, "没有指定项目目录");
    return ProjectStore(paths::from_utf8(path));
}

/// 校验上传的数据，返回扩展名。不合格直接抛 400。
std::string check_upload(const std::string& content_type, const std::string& data) {
    const std::string suffix = ref_suffix_for(content_type);
    if (suffix.empty()) {
        throw ApiError(400, "只收 png、jpg、webp，收到的是 " +
                                (content_type.empty() ? std::string("(空)")
                                                      : content_type));
    }
    if (data.empty()) throw ApiError(400, "文件是空的");
    if (data.size() > kRefMaxBytes) {
        // Python 那边是 f"{len(data)/1024/1024:.0f} MB"，四舍五入到整数
        const double mb = static_cast<double>(data.size()) / 1024.0 / 1024.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", mb);
        throw ApiError(400, std::string("太大了（") + buf +
                                " MB）。参考图给模型看，几千像素就够");
    }
    return suffix;
}

/// 写文件。落点和旧图清理交给 claim_ref_path。
std::string write_ref(const ProjectStore& store, const std::string& stem,
                      const std::string& suffix, const std::string& data) {
    const fs::path dest = claim_ref_path(store, stem, suffix);

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) throw ApiError(500, "写不了文件：" + paths::to_utf8(dest));
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) throw ApiError(500, "写文件时出错：" + paths::to_utf8(dest));

    return store.paths().rel(dest);
}

/// 对应 Python 的 round(len(data)/1024)。
int size_kb(const std::string& data) {
    return static_cast<int>(std::nearbyint(static_cast<double>(data.size()) / 1024.0));
}

}  // namespace

fs::path claim_ref_path(const ProjectStore& store, const std::string& stem,
                        const std::string& suffix) {
    std::error_code ec;
    const fs::path refs = store.paths().refs();
    fs::create_directories(refs, ec);

    const fs::path dest = refs / paths::from_utf8(stem + suffix);
    for (const auto& kv : ref_types()) {
        const fs::path stale = refs / paths::from_utf8(stem + kv.second);
        if (stale != dest && fs::is_regular_file(stale, ec)) {
            fs::remove(stale, ec);
        }
    }
    return dest;
}

std::string ref_suffix_for(const std::string& content_type) {
    for (const auto& kv : ref_types()) {
        if (content_type == kv.first) return kv.second;
    }
    return {};
}

ApiResult post_character_reference(const std::string& project_path,
                                   const std::string& char_id,
                                   const std::string& slot,
                                   const std::string& content_type,
                                   const std::string& data) {
    static const std::set<std::string> kSlots = {"front", "three_quarter", "back"};
    if (kSlots.count(slot) == 0) {
        throw ApiError(400, "只有正面、四分之三侧面、背面三个位置");
    }

    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, "没有角色 " + char_id);

    const std::string suffix = check_upload(content_type, data);
    const std::string rel = write_ref(store, char_id + "_" + slot, suffix, data);

    Character& c = it->second;
    if (slot == "front")              c.ref_front = rel;
    else if (slot == "three_quarter") c.ref_three_quarter = rel;
    else                              c.ref_back = rel;
    store.save_assets(assets);

    // 参考图直接决定画面长什么样，跟改外观是一回事，**无条件**重跑。
    // 这里没有 reset_shots 开关——Python 那边也没有。
    return {200, {
        {"saved", rel},
        {"slot", slot},
        {"reset_shots", reset_all_shots(store)},
        {"size_kb", size_kb(data)},
    }};
}

ApiResult post_location_reference(const std::string& project_path,
                                  const std::string& location_id,
                                  const std::string& content_type,
                                  const std::string& data) {
    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.locations.find(location_id);
    if (it == assets.locations.end()) {
        throw ApiError(404, "没有场景 " + location_id);
    }

    const std::string suffix = check_upload(content_type, data);
    const std::string rel = write_ref(store, location_id + "_empty", suffix, data);

    it->second.ref_empty = rel;
    store.save_assets(assets);

    // 注意响应里**没有 slot 字段**，和角色那个不一样。场景只有一张空景图。
    return {200, {
        {"saved", rel},
        {"reset_shots", reset_all_shots(store)},
        {"size_kb", size_kb(data)},
    }};
}

ApiResult post_character_reference_clear(const json& body) {
    const std::string slot = need_str(body, "slot");
    static const std::set<std::string> kSlots = {"front", "three_quarter", "back"};
    if (kSlots.count(slot) == 0) {
        throw ApiError(400, "只有正面、四分之三侧面、背面三个位置");
    }

    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string char_id = need_str(body, "char_id");
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, "没有角色 " + char_id);

    Character& c = it->second;
    std::optional<std::string>* target =
        slot == "front" ? &c.ref_front
                        : (slot == "three_quarter" ? &c.ref_three_quarter
                                                   : &c.ref_back);
    if (!target->has_value() || (*target)->empty()) {
        return {200, {{"cleared", false}, {"reset_shots", 0}}};
    }
    *target = std::nullopt;
    store.save_assets(assets);

    // 文件留着不删。用户可能只是想先试试没有参考图的效果，
    // 删掉的话再想用回来就得重新找那张图。
    return {200, {{"cleared", true}, {"reset_shots", reset_all_shots(store)}}};
}

ApiResult post_location_reference_clear(const json& body) {
    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string location_id = need_str(body, "location_id");
    const auto it = assets.locations.find(location_id);
    if (it == assets.locations.end()) {
        throw ApiError(404, "没有场景 " + location_id);
    }

    Location& l = it->second;
    if (!l.ref_empty.has_value() || l.ref_empty->empty()) {
        return {200, {{"cleared", false}, {"reset_shots", 0}}};
    }
    l.ref_empty = std::nullopt;
    store.save_assets(assets);
    return {200, {{"cleared", true}, {"reset_shots", reset_all_shots(store)}}};
}

}  // namespace changji::http
