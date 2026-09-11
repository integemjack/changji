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
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 一个钩子：这一章里可以收一集的地方。
struct DraftHook {
    std::string text;  ///< 这里悬着的是什么
    /// 这个位置前面那句原文，照抄十到二十个字。程序靠它在正文里定位。
    ///
    /// 让模型报字符偏移是行不通的——它数不准，报出来的数会落在别的段落上。
    /// 让它抄一句原文，程序自己去查，这是唯一可靠的定位手段。
    std::string after;
};

/// 模型写回来的一章。
struct ChapterDraft {
    std::string text;
    /// 这一章里所有可以收一集的地方，按先后。
    ///
    /// **不是只有章尾那一个。** 一章要切成好几集，只给一个钩子的话，
    /// 其余几集只能收在无名的段落边界上——端到端实跑时 12 集里只有 3 集
    /// 停在真悬念上，就是这么来的。
    std::vector<DraftHook> hooks;
};

/// 章节正文在模型那份 JSON 里的字段名。
///
/// **不能各写各的。** 这个名字有两个读者：`chapter_schema()` 写进 schema，
/// 而流式那一层（`stages::JsonFieldStreamer`）要照着它从 token 流里把正文
/// 抠出来推给编辑器。2026-09-11 就栽在这儿——schema 从 `text` 改成
/// `paragraphs`，流式那边没跟着改，于是它一个字都抠不出来：编辑器整整
/// 一两分钟一动不动，而**后端不报任何错**（正文照样解析、落库、重算分集），
/// 查起来毫无线索。
///
/// 现在两边都用这一个常量，再配一条用例钉住它确实是 schema 里的键。
inline constexpr const char* kChapterBodyField = "paragraphs";

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

/// 一段大约多少字。
///
/// 2026-09-11 量过起点主流长篇（《诡秘之主》第 2~6 章）：段长中位 33 字，
/// 八成以上的段只有一句话。不给这个数，模型会写成百字长段、一章二三十段，
/// 而分集是按段落边界切的，段少刀就没地方下。
inline constexpr int kCharsPerParagraph = 35;

/// 这一章该写多少段。从字数算，塞进提示词给模型一个形状。
int chapter_target_paras(const models::Story& story);

/// 这一章要标几个钩子。
///
/// **按它会被切成几集算**：每一集的结尾都该落在一个真钩子上。写死一个
/// 「章尾」是不够的——那只够最后一集用。
int chapter_hook_count(const models::Story& story);

/// 请求里带的 JSON Schema。
///
/// **正文是 paragraphs 数组，段数由 schema 卡住**（minItems / maxItems）。本地
/// 后端把 schema 转成 GBNF 硬约束，远端靠 response_format。2026-09-11 实跑：
/// 正文只是一个 text 字符串时，14B 三章里两章把梗概原样抄进去就收工（一百
/// 来字），另一次写到 8192 token 都没收口——「写满三千字」这句它根本不听。
/// 段数进了语法就是下限和上限：它没法只写一段，也没法写个没完。
nlohmann::ordered_json chapter_schema(int target_paras);

/// 拼提示词。chapter_id 不存在时抛。
std::string build_chapter_prompt(const models::Story& story,
                                 const std::string& chapter_id,
                                 models::StyleLine style_line);

/// 正文至少要有目标篇幅的这么多，否则算模型没写。
///
/// 五分之一给得很松——**要拦的是"根本没写"，不是"写短了"**。
/// 2026-09-11 实跑时模型把章标题填进了正文字段，四章各写出 1~2 个字，
/// 而这些**被静默存了下来**：故事看着有四章，分集只切出一集，
/// 到写剧本那一步才会发现无米下锅。和剧本那边「整集一句台词都没有」
/// 是同一类闸门——宁可报错重来，不要留一份看着正常的空壳。
inline constexpr double kChapterMinRatio = 0.2;

/// 解析模型返回。
///
/// `min_chars` 给 0 表示不查长度（拼提示词的单测用得着）。
ChapterDraft parse_chapter(const std::string& raw, int min_chars = 0);

/// 把写好的一章并回故事里。
///
/// **只动这一章。** 正文落进去之后钩子要重建：段落边界重新登记成候选切点
/// （原来那些是对着空正文算的，全都作废），再把模型标的那几个钩子按各自的
/// after 定位上去。大纲里那一章的钩子说法留着挂在章尾——那是这一章整体
/// 该停在哪，和中间几集收在哪不是一回事。
models::Story apply_chapter(const models::Story& story,
                            const std::string& chapter_id,
                            const ChapterDraft& draft);

}  // namespace changji::stages
