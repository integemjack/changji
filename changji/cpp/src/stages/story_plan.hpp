#pragma once

// 分集表：一章一集，就这一条。
//
// **不切章。** 用户 2026-09-16 定的：只留章模式，一章就是一集，多长由这一章
// 自己的内容定，集只在最后装配时按时长切。这儿原来是一套按每集容量找钩子
// 把一章切成上/中/下的算法；剧集表那头（sync_episodes_to_chapters）当天就改
// 成一章一集了，分集表这头却留着——同一个 ep01，剧集表说是整章，分集表说是
// 前三分之一，而剧本页的「原文」读的是分集表：一章 1800 字只显示前一半
//（2026-09-18 用户撞到）。两份真相留一份。
//
// 和 bible/script/storyboard 一样，这里只有纯函数，不碰网络也不碰 llama.cpp。

#include <cstddef>
#include <string>
#include <vector>

#include "models/story.hpp"

namespace changji::stages {

/// 一秒成片大概消化多少字原文。
///
/// **这是个估算值。** 原文是小说体，里面的心理活动、环境铺陈在改成剧本时
/// 大半会被丢掉，所以一秒能消化的原文字数远多于能说出口的字数
/// （对白那个预算是 script.hpp 的 budget_chars，约 2.9 字/秒）。
///
/// 真实时长以配音为准——这是 README 里「配音先行」那条规矩。这个数只用来
/// 估一章值多长（剧集表和分集表记的都是它）和写正文时定一章的篇幅。
inline constexpr double kProseCharsPerSecond = 15.0;

/// 这个时长大概能消化多少字原文。写正文时定一章篇幅用它。
int prose_budget_chars(double duration_s);

/// `ch07` → `ep07`。认不出编号就按它在章节表里的位置排（从 1 起）。
///
/// 分集表（plan_episodes）和剧集表（sync_episodes_to_chapters）都用这一条，
/// 两边的 id 才对得上——以前分集表的 id 是按切片发的，一章切两段就有两个，
/// 按 episode_id 查到的是隔壁章的半截。
std::string episode_id_for_chapter(const std::string& chapter_id,
                                   std::size_t index);

/// 一章一集。
///
/// 第 i 章一条：整章 [0, 正文长度)，标题就是章名，钩子取最后一场的 turn
///（没有场就取最后一条有说法的钩子），id 按 episode_id_for_chapter。
/// 时长：有正文按字数估（kProseCharsPerSecond），和剧集表记的是同一个数；
/// 没正文按 per_episode_s——它 ≤ 0 时退回 story.episode_duration_s，再不行
/// 退回 60 秒。
std::vector<models::EpisodePlan> plan_episodes(const models::Story& story,
                                               double per_episode_s);

}  // namespace changji::stages
