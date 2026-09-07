#pragma once

// 项目模型与磁盘布局。
//
// 一个项目就是一个自包含的目录，换机器整个拷走即可。目录里只有相对路径，
// 不含任何绝对路径，也不含模型文件（那些跟机器走，不跟项目走）。
//
//     我的短剧/
//     ├── project.json          项目元数据与分镜表
//     ├── assets.json           角色与场景资产库
//     ├── changji.toml          项目级配置覆盖（可选）
//     ├── refs/                 角色三视图、场景空景图
//     ├── audio/                配音
//     ├── frames/               逐镜首帧
//     ├── shots/
//     │   ├── draft/            草稿档视频
//     │   └── final/            成片档视频
//     ├── subtitles/
//     └── output/               成片
//
// 移植自 src/changji/models/project.py。
//
// ⚠️ 这个文件里所有 std::string 与 fs::path 的互转必须走
// paths::to_utf8 / paths::from_utf8。项目目录允许是 E:\AI短剧\ 这种路径，
// 直接用 path.string() 或 fs::path(str) 会在 MSVC 上抛异常，见 verify/RESULTS.md。

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/json_compat.hpp"
#include "models/shot.hpp"

namespace changji::models {

inline constexpr const char* kProjectFile = "project.json";
inline constexpr const char* kAssetsFile = "assets.json";
inline constexpr int kSchemaVersion = 1;

/// 项目里要建出来的子目录。
const std::vector<std::string>& project_subdirs();

/// 一集。分镜表挂在这里。
struct Episode {
    std::string episode_id; ///< ^[a-z0-9_]+$
    std::string title;
    std::string synopsis;
    double target_duration_s = 180.0; ///< 目标时长，> 0
    std::string script;               ///< 剧本原文
    std::vector<Shot> shots;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Episode, episode_id, title, synopsis, target_duration_s, script, shots)

    /// 按 order 排序后的镜头。
    ///
    /// Python 用的是 sorted()，稳定排序——order 相同的镜头保持原有先后。
    /// 这里必须用 std::stable_sort，用 std::sort 在 order 有重复时
    /// 顺序会和 Python 不一致，而这个顺序决定最终成片的镜头次序。
    std::vector<Shot> sorted_shots() const;

    const Shot* shot_by_id(const std::string& shot_id) const;
    Shot* shot_by_id(const std::string& shot_id);

    double planned_duration_s() const;
    std::map<std::string, int> counts_by_status() const;

    /// 取处于某个阶段的镜头。断点续跑靠它。
    std::vector<Shot> shots_needing(ShotStatus status) const;

    std::vector<std::string> validate() const;
};

/// 一个项目。
struct Project {
    int schema_version = kSchemaVersion;
    std::string project_id; ///< ^[a-z0-9_-]+$（比镜头 id 多允许连字符）
    std::string title;
    StyleLine style_line = StyleLine::REALISTIC;
    /// 这部剧讲什么。写下一集时当提示词用。
    /// 不存的话，隔天想接着写第六集，得凭记忆把当初那句话重打一遍。
    std::string premise; ///< ≤2000 字
    std::string created_at;
    std::string updated_at;
    std::vector<Episode> episodes;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Project, schema_version, project_id, title, style_line, premise,
        created_at, updated_at, episodes)

    const Episode* episode_by_id(const std::string& episode_id) const;
    Episode* episode_by_id(const std::string& episode_id);

    void touch();

    std::vector<std::string> validate() const;
};

/// 当前 UTC 时间的 ISO 8601 串。
///
/// 必须和 Python 的 datetime.now(timezone.utc).isoformat() 同格式，
/// 否则两个后端交替写同一个项目时 created_at/updated_at 的形状会变来变去。
/// 那个格式是 2026-09-07T12:34:56.123456+00:00——**微秒六位，时区写成
/// +00:00 而不是 Z**。
std::string utc_now_iso8601();

/// 项目目录布局。所有路径都由项目根推导，绝不写死。
class ProjectPaths {
public:
    explicit ProjectPaths(const std::filesystem::path& root);

    const std::filesystem::path& root() const { return root_; }

    /// 建出全部子目录。
    void ensure() const;

    std::filesystem::path project_file() const;
    std::filesystem::path assets_file() const;
    std::filesystem::path refs() const;
    std::filesystem::path audio() const;
    std::filesystem::path frames() const;
    std::filesystem::path subtitles() const;
    std::filesystem::path output() const;
    std::filesystem::path logs() const;
    std::filesystem::path shots(const std::string& tier) const;

    /// 绝对路径转成相对项目根的路径。存进 JSON 的一律用这个。
    ///
    /// 用正斜杠，保证在 Windows 上存的项目拿到 Linux 上也能读。
    /// 路径不在项目内时抛异常——存绝对路径会破坏可移植性。
    std::string rel(const std::filesystem::path& p) const;

    /// 相对路径还原成绝对路径。
    std::filesystem::path abs(const std::string& rel_path) const;

private:
    std::filesystem::path root_;
};

/// 项目的读写。写入用原子替换，避免中途断电留下半个文件。
class ProjectStore {
public:
    explicit ProjectStore(const std::filesystem::path& root);

    const ProjectPaths& paths() const { return paths_; }
    const std::filesystem::path& root() const { return paths_.root(); }

    bool exists() const;

    /// 在空目录里建一个新项目。目录已经是项目时抛异常。
    static ProjectStore create(const std::filesystem::path& root,
                               const std::string& project_id,
                               const std::string& title = "",
                               StyleLine style_line = StyleLine::REALISTIC);

    Project load_project() const;
    AssetLibrary load_assets() const;

    void save_project(Project& project) const;
    void save_assets(const AssetLibrary& assets) const;

    /// 把外部文件复制进项目，返回相对路径。
    ///
    /// 参考图这类素材必须复制进来而不是引用原位置，
    /// 否则项目拷到别的机器就断链。
    std::string copy_into(const std::filesystem::path& src,
                          const std::string& subdir,
                          const std::string& name = "") const;

private:
    ProjectPaths paths_;
};

}  // namespace changji::models
