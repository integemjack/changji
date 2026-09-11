#pragma once

// 一段正文是不是复读机。
//
// **为什么需要这个：字数守卫抓不住它。** 2026-09-11 实跑，一章写了 1124 字、
// 稳稳过了 600 字的下限，而里面这一句：
//
//     「你早就走了，我只是还在等。」
//
// 一字不差地出现了**八次**，中间夹着「沈悠看着他，眼里满是不舍」这类同义
// 句循环。模型进了退化循环，而我们只量了长度、没看内容，于是它一路存进
// story.json，再被切成集、写成剧本、排成分镜、配成音、渲成片——**一整条
// 流水线为一段复读机跑了一个多小时**。
//
// 这和 `reject_silent_audio` 是同一类事，那里的话在这儿一样成立：
// **不能只信"这一步没报错"，必须验证产出物本身。**
//
// 纯函数，不碰模型。判据刻意做成"能说清楚为什么"的形状——阈值会调，
// 而调的时候得看得懂它在说什么。

#include <cstddef>
#include <string>
#include <vector>

namespace changji::stages {

/// 多长的句子才算数。
///
/// 短句重复是正常的：「他说。」「为什么？」「我知道。」在对白里反复出现
/// 一点也不奇怪。退化循环里复读的是**完整的长句**。
inline constexpr std::size_t kRepeatMinSentenceChars = 8;

/// 同一句话出现几次算废。
///
/// 三次：一句完整的长句在一章里一字不差地出现三遍，正常写作里几乎只会是
/// 刻意的回环，而那种通常带变化（换个主语、加半句）。而模型退化时动辄
/// 七八次——实跑那次是八次。取三是让它**离正常远、离故障近**。
inline constexpr int kRepeatMaxSame = 3;

/// 去重之后还剩多少字才算数。
///
/// 抓另一种形状：没有哪一句重复到三次，但整段翻来覆去就那么几句话。
/// 0.7 是宽的——正常小说里完全重复的句子接近于零，真掉到七成以下时，
/// 那一段读起来已经明显在原地打转了。
inline constexpr double kRepeatMinUniqueRatio = 0.7;

/// 一段正文的复读体检结果。
struct RepetitionReport {
    bool ok = true;
    /// 说清楚是哪一句、重复了几次。**要能直接给用户看**——"这一章废了"
    /// 而不说废在哪儿，用户只会觉得这个工具随机报错。
    std::string detail;

    /// 重复最多的那一句和它的次数。
    std::string worst;
    int worst_count = 0;
    /// 去重之后剩下的字数占原文的比例。
    double unique_ratio = 1.0;
};

/// 按句号、问号、感叹号、换行切句。
///
/// **引号不特殊对待**：「你早就走了。」会被切成 `你早就走了。` 和一个光秃秃
/// 的 `」`。那样反而正好——前半段正是要拿去比对的那一句，而剩下的引号只有
/// 一个字符，够不着 kRepeatMinSentenceChars，不进统计。为了这点事去做引号
/// 配对，是给一个不存在的问题写代码。
std::vector<std::string> split_sentences(const std::string& text);

/// 查一段正文有没有在复读。
RepetitionReport check_repetition(const std::string& text);

}  // namespace changji::stages
