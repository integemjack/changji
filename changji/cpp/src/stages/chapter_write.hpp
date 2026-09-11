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

/// 一章的基准篇幅。
///
/// **一章是一个完整的故事单元，不是一集。** 三千字是网文一章的常见体量，
/// 也和粘贴导入那边按字数切章用的 kImportTargetChars 对齐——AI 写的章和
/// 人粘进来的章该是同一个数量级，不然同一部剧里两种来源的章长得不一样。
inline constexpr int kChapterTargetChars = 3000;

/// 一章至少要切得出这么多集。
///
/// 光有上面那个基准不够：每集选 180 秒时一集能吃 2700 字，三千字的章又变成
/// 一章一集了。两个取大的，保证**在任何每集时长下，一章都跨好几集**。
inline constexpr int kEpisodesPerChapter = 3;

/// 这一章该写多长。
///
/// **章的篇幅由故事本身定，和每集多长无关**——每集多长只决定这一章切成
/// 几集。早先这里是反过来的：按「它要撑起几集 × 每集容量」算，而大纲阶段
/// 一章一集，算出来永远是一集的量，于是分集算法的活（把章切成集）等于
/// 没做。端到端实跑时露的馅：四章写出来 340/621/399/371 字，切出来正好
/// 四集。
int chapter_target_chars(const models::Story& story);

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
