#pragma once

// 逐章展开正文。故事分三层里的最后一层。
//
//     梗概 → 故事大纲 → **章节正文**
//
// 补的是 AI 那条路上最后一个洞：大纲写出来的故事只有「这一章发生什么，
// 三五句」，没有正文。于是写剧本时展开的是梗概，而按字符切分那套机器
// （episode_text）一直返回空。粘贴进来的故事天生有正文，走不到这一步。
//
// 一次只写一章，每次带的是**压缩的全局记忆 + 上一章结尾**，不是前面所有
// 章的正文——那和逐集续写的失忆是同一个道理，二十章的正文谁也塞不下。
//
// 纯函数，不碰网络也不碰 llama.cpp。

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 模型写回来的一章。
struct ChapterDraft {
    std::string text;
    /// 结尾钩子前面那句原文。程序靠它定位切点，见 story_analyze 里同名的做法。
    std::string hook_after;
};

/// 这一章该写多长。
///
/// **按它要撑起几集算**：分集表里从这一章起头的集数 × 每集的原文容量。
/// 大纲阶段一章一集，所以就是一集的量。写少了那一集撑不满时长，写多了
/// 切出来的集比计划的多——而用户是按「每集多长」来定的这部剧。
int chapter_target_chars(const models::Story& story,
                         const std::string& chapter_id);

/// 请求里带的 JSON Schema。
const nlohmann::ordered_json& chapter_schema();

/// 拼提示词。chapter_id 不存在时抛。
std::string build_chapter_prompt(const models::Story& story,
                                 const std::string& chapter_id,
                                 models::StyleLine style_line);

ChapterDraft parse_chapter(const std::string& raw);

/// 把写好的一章并回故事里。
///
/// **只动这一章。** 正文落进去之后钩子要重建：段落边界重新登记成候选切点
/// （原来那些是对着空正文算的，全都作废），再把大纲里那一章的钩子按
/// hook_after 定位上去。
models::Story apply_chapter(const models::Story& story,
                            const std::string& chapter_id,
                            const ChapterDraft& draft);

}  // namespace changji::stages
