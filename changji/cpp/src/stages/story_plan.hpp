#pragma once

// 分集：把故事按每集时长切成集。
//
// **集数是这里算出来的，不是用户填的。** 用户给的是故事体量和每集时长，
// 「我要写 N 集」那个输入被删掉了——见 docs/故事优先重构方案.md 第一节。
//
// 和 bible/script/storyboard 一样，这里只有纯函数，不碰网络也不碰 llama.cpp。

#include <vector>

#include "models/story.hpp"

namespace changji::stages {

/// 一秒成片大概消化多少字原文。
///
/// **这是个估算值。** 原文是小说体，里面的心理活动、环境铺陈在改成剧本时
/// 大半会被丢掉，所以一秒能消化的原文字数远多于能说出口的字数
/// （对白那个预算是 script.hpp 的 budget_chars，约 2.9 字/秒）。
///
/// 真实时长以配音为准——这是 README 里「配音先行」那条规矩。分集表只是建议，
/// 人可以改，成片之后某一集明显超或欠，界面上提示重算。
inline constexpr double kProseCharsPerSecond = 15.0;

/// 尾巴不单切：剩下的不到这么多个容量，就并进最后一集。
///
/// 宁可让最后一集长出三成，也不要留一个十几秒的尾巴——那种尾巴在平台上
/// 是完播率杀手，而且它照样要走一遍分镜、配音、出片的全流程。
inline constexpr double kTailMergeRatio = 1.3;

/// 有说法的钩子比无名的段落边界值多少。
///
/// 段落边界是粘贴导入和逐章展开机械登记的，只保证「不切在半句话中间」；
/// 有 text 的那些是读懂剧情之后标出来的真钩子。切在真钩子上哪怕偏一点，
/// 也比切在一个说不出为什么的地方强——**每一集的结尾是完播率的命门**。
///
/// 只在这个容差之内才让路：离得太远就不是这一集该停的地方了。
inline constexpr double kNamedHookSlack = 0.35;

/// 这个时长的一集大概能消化多少字原文。
int prose_budget_chars(double duration_s);

/// 把故事切成集。
///
/// 规则：
/// 1. 切点**只能落在钩子上**，章界永远算一个钩子。短剧每集结尾必须是悬念、
///    反转或情绪落点，按字数硬切会把一集停在半句话上。
/// 2. 贪心：从当前位置往前一个容量，取离那个理想位置最近的候选。
///    **有说法的钩子优先**：容差之内有一个带 text 的，就用它，哪怕有个
///    无名的段落边界离得更近。
/// 3. 剩下的不到 kTailMergeRatio 个容量就收尾，不单切。
/// 4. **没有正文的章节按「一章一集」**。大纲阶段章节本来就是按情节单元分的，
///    一章一集是合理的初始猜测；正文展开之后重算会更准。
///
/// per_episode_s ≤ 0 时退回 story.episode_duration_s，再不行退回 60 秒。
std::vector<models::EpisodePlan> plan_episodes(const models::Story& story,
                                               double per_episode_s);

}  // namespace changji::stages
