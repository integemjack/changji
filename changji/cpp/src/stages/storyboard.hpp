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

/// 分镜数的地板和天花板。0 表示不限。
///
/// **2026-09-12 加的，因为 60 秒的集出过两镜六秒。** 配额那句话
/// （"合计 16 个镜头，总时长 60 秒，必须严格按配额"）提示词里一个字没少，
/// 模型照样只出两镜——schema 里 shots 数组没有 minItems，两镜在语法上
/// 挑不出毛病。和剧本那边一样：数量只有写进 schema 才管用。
struct ShotCountBounds {
    int min_items = 0;
    int max_items = 0;
};

/// 数剧本里有几拍：非空行数，段头（「【开场钩子 0–5 秒】」）不算。
int count_beats(const std::string& script);

/// 从目标时长和剧本的拍数推分镜数的上下限。
///
/// 地板是**物理下限**：单镜最长 5 秒，60 秒至少 12 镜，少于它总时长凑不够。
/// 但不能高过剧本的拍数——五行的剧本硬要 12 镜出来的是空镜（chapter_write
/// 那边的教训：模型没话说的时候，多给它几个格子只会得到几格垃圾）。
/// 天花板给得宽（配额和拍数里大的那个的两倍），只防写个没完。
ShotCountBounds shot_count_bounds(const DurationQuota& quota, double target_s,
                                  int beats);

/// 生成给大模型的 JSON Schema。
///
/// 从 pydantic 导出的 Shot schema 出发（见 shot_schema.inc.hpp），
/// 做三件事：删掉运行时字段、把角色和场景 id 收紧成枚举、
/// 去掉 needs_lipsync（那个由规则算）。
///
/// 收紧成枚举是防止模型凭空造角色最硬的手段。
///
/// bounds 给了就把镜头数写进 shots 的 minItems / maxItems。
nlohmann::ordered_json llm_shot_schema(const models::AssetLibrary& assets,
                                       ShotCountBounds bounds = {});

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

/// 这一句是不是「占位符」——模型该填空数组时填进来的那种。
///
/// **2026-09-12 实跑撞上的，而且这个会出声。** schema 里写着「这一镜没有人
/// 说话就填空数组」，模型照样在 dialogue 里塞了一句 `（无台词）`：十四镜里
/// 有四镜是这样。它不会被任何校验拦下——是合法的 DialogueLine，字数也够——
/// 然后一路走到配音，**成片里真的有人念出「无台词」三个字**。
///
/// 和 normalize_speaker 是同一个病：那边管说话人栏里的 none / 旁白，
/// 这边管台词栏里的（无台词）。形式是我们定的，不是模型定的。
bool is_placeholder_line(const std::string& text);

/// 把有台词但没进角色列表的说话人补进去。
///
/// 这不是分镜错误，只是漏填：说话的人必然在场。
/// 程序补上比让整条命令挂掉合理。
void add_missing_speakers(nlohmann::json& item,
                          const std::set<std::string>& known);

/// 解析模型返回，产出镜头列表。
std::vector<models::Shot> parse_storyboard(const std::string& raw,
                                           const models::AssetLibrary& assets);

/// 把镜头编号和顺序重排成规整的一套。
///
/// **2026-09-12 加的，因为模型编出来的 id 是坏的。** 一次实跑里出了
/// `ep61_sh002`（集号都错了）、`ep01_sh6`（没补零）、`ep01_s1h11`（打错字）。
/// 提示词里写着「三位数字，按顺序递增」，schema 的 `^[a-z0-9_]+$` 也全放行
/// ——**三种写法都合法，所以一句都不报**。而首帧、配音、成片的文件名都是
/// 从 shot_id 拼的：集号错的那一镜会写到别的集的目录里去。
///
/// 按 order 稳定排序，然后 order 重排成 0..n-1、id 重排成
/// `<episode_id>_shNNN`。顺便治了 order 重复——重复时镜头次序是不定的，
/// 而那个次序就是成片的次序。
///
/// **只在分镜刚出来时调。** 配音那一步会拆镜（`free_shot_id` 发 `_b` 后缀），
/// 那之后再重排就会和已经落盘的音频文件名对不上。
void renumber_shots(std::vector<models::Shot>& shots,
                    const std::string& episode_id);

/// 检查分镜有没有**废掉**。
///
/// 大模型很容易只写画面不写台词，产出一部哑剧。这类问题在生成阶段就能检出，
/// 不该等到配音阶段发现一句话都没有。
///
/// 两条，都是「这张分镜表没法用」：一句台词都没有、一个角色都没有。
/// 调用方拿到非空就该丢掉重来。
///
/// **漏几句台词不在这儿**，那个见 missing_dialogue_lines——分镜表还能用，
/// 丢掉它等于把几分钟的显卡时间也一起丢了。
std::vector<std::string> check_coverage(const std::string& script,
                                        const std::vector<models::Shot>& shots);

/// 把剧本里漏掉的台词按顺序补进镜头，返回补了几句。
///
/// **2026-09-12 加的，因为分镜模型根本不搬台词。** 把原始输出 dump 出来看，
/// 一集剧本九句台词，模型只写了两句——不是解析吃掉了，是压根没生成。这个
/// 靠校验和清洗救不了：句子不在那儿。
///
/// 而台词本来就在剧本里，有顺序、有说话人、一字不差。分镜师真正该干的是
/// 画面——景别、运镜、构图；台词是从剧本抄过来的，不该指望模型重打一遍。
/// 行业里也是这么分的（分镜头脚本叫「导演脚本」，台词那一栏照抄剧本）。
/// 所以这一步改成：**模型排画面，台词由引擎放**。
///
/// 落位办法：已经落对的那些当锚点，漏掉的按它在剧本里的前后关系插到相邻
/// 锚点之间；一个锚点都没有就按比例摊到各镜。位置是估的，但**台词、说话人、
/// 音色、口型、字幕全是准的**——比整句话消失强得多，而且人在镜头那一页
/// 拖一下就能改。
///
/// 补进去的镜头会因为多了台词而时长不够？不会：配音那一步按语音时长
/// 重新锁定镜头时长（lock_duration），本来就是这么设计的。
int place_missing_dialogue(std::vector<models::Shot>& shots,
                           const std::string& script,
                           const models::AssetLibrary& assets);

/// 剧本里哪几句台词没落到任何镜头上，按剧本顺序。
///
/// **2026-09-12 加的。** 原来根本没人查：一集 17 拍出 12 镜、整个集尾留扣
/// 那一段（连同这一集的钩子）没进分镜，照样算通过，存下去，到成片才看得出
/// 这一集结尾不对。短剧每集结尾就是完播率的命门，丢的恰恰是最要紧的那一段。
///
/// **但它不是致命错。** 实跑里九句丢一句是常事，而出一次分镜要两三分钟的
/// 显卡时间——为了中间漏的一句把整张表扔掉，人什么也拿不到，还得从头再等
/// 一遍。所以这里只把漏掉的报回去：表照存，界面上说清楚漏了哪几句，人可以
/// 在镜头那一页把它补进某一镜，也可以重出。
///
/// 比对时空白和标点都不算数：模型把一句拆成两镜、或者把句号换成逗号，都是
/// 同一句话落地了。真丢了的那种是整句都不在，那个照样查得出来。
std::vector<std::string> missing_dialogue_lines(
    const std::string& script, const std::vector<models::Shot>& shots);

/// 把总时长拉回目标值。
///
/// 偏差优先摊到无对白的过渡镜上，有台词的镜头不动，
/// 因为它们的时长是由配音定的。原地改，同时返回引用方便串联。
std::vector<models::Shot>& rebalance_durations(std::vector<models::Shot>& shots,
                                               double target_s,
                                               double tolerance_s = 3.0);

}  // namespace changji::stages
