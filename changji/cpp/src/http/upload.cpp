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

/// 参考音色收哪几种。**wav 放第一个**：进程内那条路最稳的就是它。
///
/// ⚠️ **这张表要和进程内那条路真能读的对上。** 上一版还收 `audio/mp4`
/// （.m4a），而 mtmd 那边走的是 miniaudio，只认 wav / mp3 / flac
/// ——`infer/llama_tts.cpp` 里那句 `mtmd_helper_bitmap_init_from_file`
/// 失败时说的就是「只认 wav / mp3 / flac」，注释里还专门写着「拿一个
/// m4a 或者 ogg 过来是很常见的事」。
///
/// 于是传一段 m4a：这儿收下、回一句「参考音色已存」、角色也配上了，
/// 而第一句台词开配才炸——那时候人已经在跑整集了。收的时候就该说不。
///
/// （这张表原来那句「别的格式要看 ggml 那边的解码器编没编进去」是在答案
/// 还不知道的时候写的。答案现在在 llama_tts.cpp 里。）
const std::array<std::pair<const char*, const char*>, 4>& voice_types() {
    static const std::array<std::pair<const char*, const char*>, 4> kTypes = {{
        {"audio/wav", ".wav"},
        {"audio/x-wav", ".wav"},
        {"audio/mpeg", ".mp3"},
        {"audio/flac", ".flac"},
    }};
    return kTypes;
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

std::string voice_suffix_for(const std::string& content_type) {
    for (const auto& kv : voice_types()) {
        if (content_type == kv.first) return kv.second;
    }
    return {};
}

ApiResult post_character_voice(const std::string& project_path,
                               const std::string& char_id,
                               const std::string& content_type,
                               const std::string& data) {
    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, "没有角色 " + char_id);

    const std::string suffix = voice_suffix_for(content_type);
    if (suffix.empty()) {
        throw ApiError(400, "只收 wav、mp3、flac，收到的是 " +
                                (content_type.empty() ? std::string("(空)")
                                                      : content_type));
    }
    if (data.empty()) throw ApiError(400, "文件是空的");
    if (data.size() > kVoiceMaxBytes) {
        const double mb = static_cast<double>(data.size()) / 1024.0 / 1024.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", mb);
        throw ApiError(400, std::string("太大了（") + buf +
                                " MB）。参考音色几秒到十几秒的干净人声就够");
    }

    // **voices/ 是懒建的**，见 ProjectPaths::voices 上面那段。
    std::error_code ec;
    const fs::path dir = store.paths().voices();
    fs::create_directories(dir, ec);

    // 同名不同扩展名的旧片段要清掉，理由同 claim_ref_path：
    // 留着的话目录里躺一段永远用不上的，而用户看不到。
    const fs::path dest = dir / paths::from_utf8(char_id + suffix);
    for (const auto& kv : voice_types()) {
        const fs::path stale = dir / paths::from_utf8(char_id + kv.second);
        if (stale != dest && fs::is_regular_file(stale, ec)) fs::remove(stale, ec);
    }

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) throw ApiError(500, "写不了文件：" + paths::to_utf8(dest));
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) throw ApiError(500, "写文件时出错：" + paths::to_utf8(dest));

    const std::string rel = store.paths().rel(dest);
    it->second.voice_id = rel;
    store.save_assets(assets);

    // **不重跑。** 见头文件那段：音色不影响画面，和改名字一个待遇。
    return {200, {{"saved", rel}, {"size_kb", size_kb(data)}}};
}

ApiResult post_character_voice_clear(const json& body) {
    ProjectStore store = open_project(need_str(body, "project"));
    const std::string char_id = need_str(body, "char_id");
    AssetLibrary assets = store.load_assets();
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, "没有角色 " + char_id);

    std::error_code ec;
    const fs::path dir = store.paths().voices();
    for (const auto& kv : voice_types()) {
        fs::remove(dir / paths::from_utf8(char_id + kv.second), ec);
    }
    it->second.voice_id = std::nullopt;
    store.save_assets(assets);
    return {200, {{"cleared", true}}};
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
