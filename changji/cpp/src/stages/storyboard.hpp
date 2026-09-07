#pragma once

// 分镜生成。
//
// 两阶段生成的第二阶段。角色和场景已经在资产库里有 id 了，
// 这一阶段的 schema 里没有外观字段可写——给了模型就会忍不住在分镜里
// 复述一遍，而复述必然有偏差，那正是漂移的来源。
//
// 和 bible 一样，这里**不碰网络也不碰 llama.cpp**，只有纯函数。

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/shot.hpp"
#include "stages/limits.hpp"

namespace changji::stages {

class StoryboardError : public std::runtime_error {
public:
    explicit StoryboardError(const std::string& what) : std::runtime_error(what) {}
};

/// 视频模型支持的时长档位。
///
/// **和 Python 一样在 fps=24 下算死**，不跟着 assembly.fps 走。
/// 那边 DURATION_SLOTS 是模块导入时用默认 fps 算的模块级常量，
/// 配置里改了 fps 也不会重算。这一点照抄——不是因为它对，
/// 而是因为改了会让两边的分镜表对不上。
const std::vector<double>& duration_slots();

/// 时长配额。先定骨架再填内容，比让模型自己算总时长可靠得多。
struct DurationQuota {
    /// 档位 -> 个数。用 map 而不是 unordered_map：describe() 要按档位排序。
    std::map<double, int> slots;

    double total_s() const;
    int shot_count() const;

    /// "3 个 2 秒镜头，5 个 5 秒镜头" 这样的一句话。
    std::string describe() const;

    /// 按目标时长分配镜头。
    ///
    /// 节奏上短镜头占多数，长镜头留给情绪戏。全用同一时长会平铺直叙。
    static DurationQuota for_duration(double target_s);
};

/// 把任意时长吸附到最近的可生成档位。
double snap_duration(double seconds);

/// 向上吸附。配音时长反推镜头时长时用，宁长勿短。
double ceil_duration(double seconds);

/// 生成给大模型的 JSON Schema。
///
/// 从 pydantic 导出的 Shot schema 出发（见 shot_schema.inc.hpp），
/// 做三件事：删掉运行时字段、把角色和场景 id 收紧成枚举、
/// 去掉 needs_lipsync（那个由规则算）。
///
/// 收紧成枚举是防止模型凭空造角色最硬的手段。
nlohmann::ordered_json llm_shot_schema(const models::AssetLibrary& assets);

/// 拼提示词。**输出必须和 Python 的 build_prompt 逐字节一致。**
///
/// 角色和场景只给 id 和名字，不给外观描述。
std::string build_storyboard_prompt(const std::string& script,
                                    const models::AssetLibrary& assets,
                                    const DurationQuota& quota,
                                    const std::string& episode_id);

/// location_id 空着但 scene_id 正是一个已注册场景时，把它接上。
///
/// 模型十次有八次把场景 id 填进 scene_id 就完事了。后果不是报错——
/// 分镜表照样合法，是渲染时只在 location_id 有值时才把场景描述拼进提示词，
/// 于是空间和光线那一段整个丢掉，同一个房间在每个镜头里都长得不一样。
///
/// 返回是否改动过，调用方要靠它决定用不用存盘。
bool link_location(nlohmann::json& item, const std::set<std::string>& known);

/// 把有台词但没进角色列表的说话人补进去。
///
/// 这不是分镜错误，只是漏填：说话的人必然在场。
/// 程序补上比让整条命令挂掉合理。
void add_missing_speakers(nlohmann::json& item,
                          const std::set<std::string>& known);

/// 解析模型返回，产出镜头列表。
std::vector<models::Shot> parse_storyboard(const std::string& raw,
                                           const models::AssetLibrary& assets);

/// 检查分镜有没有漏掉剧本里的东西。
///
/// 大模型很容易只写画面不写台词，产出一部哑剧。这类问题在生成阶段就能检出，
/// 不该等到配音阶段发现一句话都没有。
std::vector<std::string> check_coverage(const std::string& script,
                                        const std::vector<models::Shot>& shots);

/// 把总时长拉回目标值。
///
/// 偏差优先摊到无对白的过渡镜上，有台词的镜头不动，
/// 因为它们的时长是由配音定的。原地改，同时返回引用方便串联。
std::vector<models::Shot>& rebalance_durations(std::vector<models::Shot>& shots,
                                               double target_s,
                                               double tolerance_s = 3.0);

}  // namespace changji::stages
