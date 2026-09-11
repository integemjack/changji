#pragma once

// 读一遍现成的正文，把结构提出来。
//
// 粘贴导入（story_import）只切章节，切完的故事里人物、关系、地点全是空的，
// 走到「设定」那一步资产库还是空的，再往下分镜就指不到任何角色。这一步把
// 那个洞补上。
//
// 和 story_outline 的分工：outline 是**无中生有**（从一句梗概编出整个
// 故事），analyze 是**读已经有的**。两件事不能合成一个提示词——让「写故事」
// 那份去读现成的正文，它会忍不住改写，而后面每一集都是照着正文展开的。
//
// 纯函数，不碰网络也不碰 llama.cpp。

#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 把章节渲染成节选：每章标题 + 开头一段 + 「……」+ 结尾一段。
///
/// 整本小说塞不进上下文。开头交代这一章从哪儿接上，结尾决定钩子在哪，
/// 中间的过程靠这两头能推个八九不离十。章多的时候每章分到的字数更少，
/// 但**每章都在**——漏掉一整章比每章少几百字糟糕得多。
std::string render_chapters_for_analysis(const models::Story& story);

/// 请求里带的 JSON Schema。
///
/// 和大纲那份的差别：这里的 chapters 带 chapter_id（要映射回去）和
/// hook_after（钩子前面那句原文，程序靠它定位切点），而且**没有 title**——
/// 章名是作者自己写的，不该让模型改。
const nlohmann::ordered_json& analyze_schema();

std::string build_analyze_prompt(const models::Story& story,
                                 models::StyleLine style_line);

/// 把模型读出来的东西并回故事里。
///
/// **正文、章名、章节 id 一个字不动。** 这一步只补 summary、hook 和全剧的
/// 人物/关系/地点——改正文就等于把用户粘进来的东西换掉了。
///
/// hook 的位置靠 hook_after 在正文里查：查得到就在那句话之后插一个带说法的
/// 切点，查不到就挂在章尾。机械切出来的那些段落边界候选**留着**，
/// 它们是找不到更好切点时的兜底。
models::Story apply_analysis(const models::Story& story, const std::string& raw);

}  // namespace changji::stages
