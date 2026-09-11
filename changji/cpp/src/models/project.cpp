#include "models/project.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::models {

using json = nlohmann::json;

namespace {

bool is_slug(const std::string& s, bool allow_dash) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [&](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
               (allow_dash && c == '-');
    });
}

std::size_t utf8_len(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

/// 读 JSON 文件。
///
/// 模板参数是为了 ordered_json：读资产库必须保留文档里的键顺序，
/// 因为 characters/locations 在接口响应里是**数组**，顺序是值的一部分。
/// 普通的 nlohmann::json 内部是 std::map，parse 时顺序当场就丢了。
template <typename J>
J read_json_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("读不到文件：" + paths::to_utf8(path));
    }
    J j = J::parse(in, nullptr, false);
    if (j.is_discarded()) {
        throw std::runtime_error("文件损坏，不是合法 JSON：" + paths::to_utf8(path));
    }
    return j;
}

/// 原子写。先写临时文件再替换，中途断电不会留下半个文件。
///
/// 与 Python 侧一致的关键点是 rename 必须在**同一个文件系统**上，
/// 所以临时文件放在目标文件的同目录，不能用系统临时目录。
void write_json_atomic(const fs::path& path, const json& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // 临时文件名带随机后缀，避免两个进程同时写时互相覆盖对方的临时文件。
    static std::mt19937 rng{std::random_device{}()};
    std::ostringstream suffix;
    suffix << ".tmp" << std::hex << rng();
    const fs::path tmp = path.parent_path() /
                         paths::from_utf8(paths::to_utf8(path.filename()) + suffix.str());

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("写不了临时文件：" + paths::to_utf8(tmp));
        }
        // ensure_ascii=False + indent=2，与 Python 侧的 json.dump 对齐。
        // nlohmann 默认就不转义非 ASCII，中文原样写出。
        out << data.dump(2);
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            throw std::runtime_error("写临时文件时出错：" + paths::to_utf8(tmp));
        }
    }

    // fs::rename 在标准里要求目标存在时替换掉它（POSIX 语义），
    // MSVC 底层走的是 MoveFileEx 加 MOVEFILE_REPLACE_EXISTING。
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("替换文件失败：" + paths::to_utf8(path));
    }
}

}  // namespace

const std::vector<std::string>& project_subdirs() {
    static const std::vector<std::string> kDirs = {
        "refs", "audio", "frames", "shots/draft", "shots/final",
        "subtitles", "output", "logs",
    };
    return kDirs;
}

std::string utc_now_iso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto us = duration_cast<microseconds>(now - secs).count();

    const std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif

    // Python 的 isoformat() 在有微秒时输出六位，时区写成 +00:00 不是 Z。
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06lld+00:00",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(us));
    return buf;
}

// ── Episode ────────────────────────────────────────────────────────────

std::vector<Shot> Episode::sorted_shots() const {
    std::vector<Shot> out = shots;
    // 必须是稳定排序。Python 的 sorted() 是稳定的，order 相同的镜头保持
    // 原有先后；std::sort 不保证，order 有重复时顺序会和 Python 不一致，
    // 而这个顺序直接决定成片里镜头的次序。
    std::stable_sort(out.begin(), out.end(),
                     [](const Shot& a, const Shot& b) { return a.order < b.order; });
    return out;
}

const Shot* Episode::shot_by_id(const std::string& shot_id) const {
    for (const auto& s : shots) {
        if (s.shot_id == shot_id) return &s;
    }
    return nullptr;
}

Shot* Episode::shot_by_id(const std::string& shot_id) {
    return const_cast<Shot*>(
        static_cast<const Episode*>(this)->shot_by_id(shot_id));
}

double Episode::planned_duration_s() const {
    double total = 0.0;
    for (const auto& s : shots) total += s.duration_s;
    return total;
}

std::map<std::string, int> Episode::counts_by_status() const {
    std::map<std::string, int> out;
    for (const auto& s : shots) ++out[to_string(s.status)];
    return out;
}

std::vector<Shot> Episode::shots_needing(ShotStatus status) const {
    std::vector<Shot> out;
    for (const auto& s : sorted_shots()) {
        if (s.status == status) out.push_back(s);
    }
    return out;
}

std::vector<std::string> Episode::validate() const {
    std::vector<std::string> errs;
    if (!is_slug(episode_id, false)) {
        errs.push_back("剧集 id 只能是小写字母、数字和下划线，当前是 " + episode_id);
    }
    if (target_duration_s <= 0.0) {
        errs.push_back(episode_id + "：target_duration_s 必须大于 0");
    }
    for (const auto& s : shots) {
        for (auto& e : s.validate()) {
            errs.push_back(episode_id + "/" + s.shot_id + "：" + e);
        }
    }
    return errs;
}

// ── Project ────────────────────────────────────────────────────────────

const Episode* Project::episode_by_id(const std::string& episode_id) const {
    for (const auto& e : episodes) {
        if (e.episode_id == episode_id) return &e;
    }
    return nullptr;
}

Episode* Project::episode_by_id(const std::string& episode_id) {
    return const_cast<Episode*>(
        static_cast<const Project*>(this)->episode_by_id(episode_id));
}

void Project::touch() { updated_at = utc_now_iso8601(); }

std::vector<std::string> Project::validate() const {
    std::vector<std::string> errs;
    // 项目 id 比镜头 id 多允许连字符
    if (!is_slug(project_id, true)) {
        errs.push_back("项目 id 只能是小写字母、数字、下划线和连字符，当前是 " +
                       project_id);
    }
    if (utf8_len(premise) > 2000) {
        errs.push_back("premise 超长：" + std::to_string(utf8_len(premise)) +
                       " 字，最多 2000 字");
    }
    for (const auto& e : episodes) {
        for (auto& msg : e.validate()) errs.push_back(std::move(msg));
    }
    return errs;
}

// ── ProjectPaths ───────────────────────────────────────────────────────

ProjectPaths::ProjectPaths(const fs::path& root) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(root, ec);
    root_ = ec ? root : p;
}

void ProjectPaths::ensure() const {
    std::error_code ec;
    for (const auto& sub : project_subdirs()) {
        fs::create_directories(root_ / paths::from_utf8(sub), ec);
    }
}

fs::path ProjectPaths::project_file() const { return root_ / kProjectFile; }
fs::path ProjectPaths::assets_file() const { return root_ / kAssetsFile; }
fs::path ProjectPaths::story_file() const { return root_ / kStoryFile; }
fs::path ProjectPaths::refs() const { return root_ / "refs"; }
fs::path ProjectPaths::audio() const { return root_ / "audio"; }
fs::path ProjectPaths::frames() const { return root_ / "frames"; }
fs::path ProjectPaths::subtitles() const { return root_ / "subtitles"; }
fs::path ProjectPaths::output() const { return root_ / "output"; }
fs::path ProjectPaths::logs() const { return root_ / "logs"; }

fs::path ProjectPaths::shots(const std::string& tier) const {
    return root_ / "shots" / paths::from_utf8(tier);
}

std::string ProjectPaths::rel(const fs::path& p) const {
    std::error_code ec;
    const fs::path abs_p = fs::weakly_canonical(p, ec);
    const fs::path target = ec ? p : abs_p;

    const fs::path r = fs::relative(target, root_, ec);
    // relative 走不出去时返回空，或者结果以 .. 开头都说明不在项目内
    const std::string s = ec ? std::string() : paths::to_utf8(r);
    if (s.empty() || s.rfind("..", 0) == 0) {
        throw std::runtime_error(
            "路径不在项目目录内，存进项目会破坏可移植性：" + paths::to_utf8(target) +
            "\n请先把文件复制进 " + paths::to_utf8(root_));
    }
    // 正斜杠，保证 Windows 上存的项目拿到 Linux 上也能读
    std::string posix = s;
    std::replace(posix.begin(), posix.end(), '\\', '/');
    return posix;
}

fs::path ProjectPaths::abs(const std::string& rel_path) const {
    std::error_code ec;
    fs::path p = root_ / paths::from_utf8(rel_path);
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}

// ── ProjectStore ───────────────────────────────────────────────────────

ProjectStore::ProjectStore(const fs::path& root) : paths_(root) {}

bool ProjectStore::exists() const {
    std::error_code ec;
    return fs::is_regular_file(paths_.project_file(), ec);
}

ProjectStore ProjectStore::create(const fs::path& root,
                                  const std::string& project_id,
                                  const std::string& title,
                                  StyleLine style_line) {
    ProjectStore store(root);
    if (store.exists()) {
        throw std::runtime_error("这个目录已经是一个项目了：" +
                                 paths::to_utf8(store.root()));
    }
    store.paths_.ensure();

    Project project;
    project.project_id = project_id;
    project.title = title.empty() ? project_id : title;
    project.style_line = style_line;
    project.created_at = utc_now_iso8601();
    project.updated_at = project.created_at;
    store.save_project(project);

    AssetLibrary assets;
    assets.style.style_line = style_line;
    store.save_assets(assets);
    return store;
}

Project ProjectStore::load_project() const {
    if (!exists()) {
        throw std::runtime_error(
            "这里不是一个项目目录：" + paths::to_utf8(root()) +
            "\n用 changji new 创建，或者 cd 到正确的目录");
    }
    const json raw = read_json_file<json>(paths_.project_file());
    const int version = raw.value("schema_version", 0);
    if (version > kSchemaVersion) {
        throw std::runtime_error(
            "项目是用更新版本的场记创建的（格式版本 " + std::to_string(version) +
            "，本机支持到 " + std::to_string(kSchemaVersion) + "）。请升级后再打开");
    }
    return raw.get<Project>();
}

AssetLibrary ProjectStore::load_assets() const {
    std::error_code ec;
    if (!fs::is_regular_file(paths_.assets_file(), ec)) {
        return AssetLibrary{};
    }
    // 用 ordered_json 而不是 json：见 read_json_file 的注释。
    AssetLibrary lib = read_json_file<nlohmann::ordered_json>(paths_.assets_file())
                           .get<AssetLibrary>();

    // **没写画风就按这条线补一个。** 空着的后果不是"少一句修饰"：整条
    // 提示词里一个画风词都没有，出图模型每张各自发挥——同一个项目里三个
    // 角色出了皮克斯 3D、半写实、照片三种质感（2026-09-12 实见）。
    //
    // 补在这儿而不是出图那一层：这样它是项目里一条看得见的数据，项目页
    // 那个「画风」框里显示出来、改得动。清空再存的话下次读又会补回来——
    // 那是对的：总得有个底子，"没有画风"不是一种画风。
    if (text::strip_ws(lib.style.global_style).empty()) {
        lib.style.global_style = default_style(lib.style.style_line);
    }
    return lib;
}

Story ProjectStore::load_story() const {
    std::error_code ec;
    if (!fs::is_regular_file(paths_.story_file(), ec)) {
        return Story{};
    }
    return read_json_file<json>(paths_.story_file()).get<Story>();
}

void ProjectStore::save_project(Project& project) const {
    project.touch();
    write_json_atomic(paths_.project_file(), json(project));
}

void ProjectStore::save_assets(const AssetLibrary& assets) const {
    write_json_atomic(paths_.assets_file(), json(assets));
}

void ProjectStore::save_story(const Story& story) const {
    write_json_atomic(paths_.story_file(), json(story));
}

std::string ProjectStore::copy_into(const fs::path& src,
                                    const std::string& subdir,
                                    const std::string& name) const {
    std::error_code ec;
    const fs::path source = fs::weakly_canonical(src, ec);
    const fs::path from = ec ? src : source;
    if (!fs::is_regular_file(from, ec)) {
        throw std::runtime_error("文件不存在：" + paths::to_utf8(from));
    }
    const fs::path target_dir = root() / paths::from_utf8(subdir);
    fs::create_directories(target_dir, ec);

    const fs::path target =
        target_dir / (name.empty() ? from.filename() : paths::from_utf8(name));
    if (from != target) {
        fs::copy_file(from, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw std::runtime_error("复制失败：" + paths::to_utf8(from) + " → " +
                                     paths::to_utf8(target));
        }
    }
    return paths_.rel(target);
}

}  // namespace changji::models
