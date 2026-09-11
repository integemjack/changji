#pragma once

// 从故事写一集。
//
// 和 script.hpp 里那条「正片」路线的分水岭：
//
//   老的  给一句梗概 + 前三集原文，让模型自己想这一集该发生什么
//   新的  这一集要发生什么已经定好了（分集表指着故事的一段），
//         模型只负责把那一段变成能拍的拍子
//
// **失忆是在这里治好的。** 老路线带的上下文是前三集的原文，取三集、截
// 4000 字符——写第五集时第一集已经不在上下文里了，伏笔全靠运气。这里带的
// 是压缩的全局记忆：大纲一句、人物表、关系、以及本集之前每一章一句的前情
// 提要。压缩之后二十章也塞得下。
//
// 纯函数，不碰网络也不碰 llama.cpp。

#include <string>
#include <vector>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 取一集覆盖的那段正文。
///
/// 按分集表的 [from, to) 区间在章节之间切。**按 UTF-8 字符切，不按字节**——
/// 按字节切会把汉字劈成三段，截出来的是非法 UTF-8，一路流到提示词和字幕。
///
/// 章节还没展开正文时返回空串，调用方退回用 summary。
std::string episode_text(const models::Story& story,
                         const models::EpisodePlan& plan);

/// 这一集覆盖了哪几章（按顺序，去重）。
std::vector<std::string> episode_chapters(const models::Story& story,
                                          const models::EpisodePlan& plan);

/// 这一集压着哪几场戏。
///
/// 分集是照着场的边界切的，所以正常情况下一集正好是一场；模型字数写飘了
/// 或者人改过分集表时会跨两场。拿它给写剧本那一步交代**这一集在哪、
/// 跟着谁走、他要什么、谁拦着**——正文里这些是化在叙述里的，单独列出来
/// 模型才不会把地点写丢（一集的每一镜都要照着地点画）。
std::vector<models::Scene> episode_scenes(const models::Story& story,
                                          const models::EpisodePlan& plan);

/// 这一集压着哪几场戏。
///
/// 分集是照着场的边界切的，所以正常情况下一集正好是一场；模型字数写飘了
/// 或者人改过分集表时会跨两场。拿它给写剧本那一步交代**这一集在哪、
/// 跟着谁走、他要什么、谁拦着**——正文里这些是化在叙述里的，单独列出来
/// 模型才不会把地点写丢（一集的每一镜都要照着地点画）。
std::vector<models::Scene> episode_scenes(const models::Story& story,
                                          const models::EpisodePlan& plan);

/// 渲染给模型看的那一段：底子、人物、关系、前情提要、这一集、停在哪。
///
/// 单独拆出来是为了能测——拼提示词只是在它前后接常量，真正会错的是这里：
/// 把还没发生的事写进前情（模型会当成已经发生的写）、把这一集的正文
/// 漏掉、截断截在半个字上。
///
/// previous_tail 是上一集剧本的结尾几行，用来接语气；空着也行。
std::string render_script_context(const models::Story& story,
                                  const models::EpisodePlan& plan,
                                  const std::string& previous_tail = "");

/// 拼提示词。
std::string build_script_prompt_from_story(
    const models::Story& story, const models::EpisodePlan& plan,
    models::StyleLine style_line, const std::vector<std::string>& characters = {},
    const std::string& previous_tail = "");

/// 取一段剧本的结尾，给下一集接语气用。按字符截，不按字节。
std::string script_tail(const std::string& script);

}  // namespace changji::stages
