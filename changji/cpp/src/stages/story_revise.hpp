#pragma once

// 改原稿的某一段。
//
// 故事那一页的重点是**创作**，而创作不是"按一次按钮生成一整章"——是选中
// 一段觉得不对的字，说一句"这儿太赶了，铺一下"，看它改完，再决定要不要。
// 逐章展开（stages/chapter_write）解决的是"从无到有"，这一步解决的是
// "有了之后怎么调"，两件不同的事。
//
// **只改选中的那一段，别的一个字不动。** 这一条是整个设计的地基：改动范围
// 可预期，用户才敢让 AI 碰自己写了一半的稿子。让模型重写整章然后指望它
// "顺便保留别处"是做不到的——它会悄悄改掉你上周调好的一句对白，而你要到
// 三天后才发现。
//
// 纯函数：拼提示词、解析回来的那段字。落盘和重算切点是调用方的事
// （http/story_api 里那一对接口：先出草稿，人点了才落）。

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 选中的那一段。位置按 **UTF-8 字符**计，[from, to)。
struct Span {
    std::string chapter_id;
    int from_char = 0;
    int to_char = 0;
};

/// 上下文各带多少字。
///
/// 改一段话要知道它前面刚发生了什么、后面马上要发生什么，否则改出来的
/// 那段接不上——最常见的是把前面已经交代过的事又交代一遍。带太多又会
/// 让模型分不清"要改的是哪一段"，所以是几百字这个量级，不是整章。
inline constexpr int kReviseContextChars = 600;

/// 一段对话。`role` 只有 user / assistant 两种。
///
/// 对话形式改稿时，历次来回都要带上：用户说"再短一点"的时候，"一点"是
/// 相对于上一版说的，丢了上下文就无从判断。
struct ReviseTurn {
    std::string role;
    std::string text;
};

/// 取出选中那一段的原文。越界会被夹到合法范围内，不抛。
std::string span_text(const models::Story& story, const Span& span);

/// 改稿的提示词。
///
/// `instruction` 是这一次要它做什么（"太赶了，铺一下情绪"）。
/// `history` 是之前的来回，可以为空——空的就是"选中一段直接说一句"那条路，
/// 非空就是对话形式。**两条路共用同一个提示词和同一条写回路径**：两套
/// 机制都能改正文的话，迟早出现"对话改的和选中改的对同一段各有一份"。
std::string build_revise_prompt(const models::Story& story, const Span& span,
                                const std::string& instruction,
                                const std::vector<ReviseTurn>& history,
                                models::StyleLine style_line);

/// JSON Schema：回一段替换用的正文，外加一句说明改了什么。
const nlohmann::ordered_json& revise_schema();

/// 模型改完的一段。
struct Revision {
    std::string text;  ///< 拿去替换选中那一段的新正文
    std::string note;  ///< 一句话说改了什么。给对话那条路显示，不进正文
};

/// 解析模型回来的那一段。
///
/// `span_chars` 是选中那段原文有多少字，用来拦**改着改着把整章吐回来**：
/// 那样落盘之后整章内容会翻倍，而界面上只显示"改好了"。
/// 传 0 表示不检查长度。
///
/// 变短是合法的（"把这段压缩成一句"就该变短），所以只拦上限不拦下限。
Revision parse_revision(const std::string& raw, int span_chars);

/// 把新的一段接回故事里：替换正文、重算这一章的候选切点。
///
/// **不重算分集表**——那是调用方的事，因为要不要重算取决于这几集有没有
/// 已经排好的分镜，而这一层看不见项目。
models::Story apply_revision(const models::Story& story, const Span& span,
                             const std::string& text);

}  // namespace changji::stages
