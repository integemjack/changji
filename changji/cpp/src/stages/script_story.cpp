#include "stages/script_story.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "stages/script.hpp"
// 画风那两句和正片那一份共用——同一部剧不该因为换了条写作路线就换语气。
#include "stages/script_prompt.inc.hpp"
#include "stages/script_story_prompt.inc.hpp"
#include "util/text.hpp"

namespace changji::stages {

using namespace changji::models;

namespace {

/// 和 script.cpp 的同名函数一致：秒数印成整数，60.0 印成 "60"。
/// std::to_string 对 double 给六位小数，会印成 "60.000000"。
std::string format_f0(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return std::string(buf);
}

/// 取 [from, to) 那一段，**按 UTF-8 字符**。
///
/// 按字节切会把汉字劈成三段，截出来的是非法 UTF-8——它会一路流到提示词、
/// 分镜和字幕，最后表现成「整轨字幕不显示」这种离得很远的故障。
std::string slice_chars(const std::string& s, int from, int to) {
    const std::vector<std::string> chars = text::utf8_chars(s);
    const int n = static_cast<int>(chars.size());
    const int a = std::clamp(from, 0, n);
    const int b = std::clamp(to, a, n);
    std::string out;
    for (int i = a; i < b; ++i) out += chars[i];
    return out;
}

/// 章节在 story.chapters 里的下标，找不到返回 -1。
int index_of(const Story& story, const std::string& chapter_id) {
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        if (story.chapters[i].chapter_id == chapter_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

std::vector<std::string> episode_chapters(const Story& story,
                                          const EpisodePlan& plan) {
    std::vector<std::string> out;
    const int a = index_of(story, plan.from_chapter);
    if (a < 0) return out;
    // 末章找不到时就只算起始那一章。分集表和故事对不上是坏数据，
    // 但不该让写剧本这件事整个做不成。
    const int b0 = index_of(story, plan.to_chapter);
    const int b = b0 < a ? a : b0;
    for (int i = a; i <= b; ++i) out.push_back(story.chapters[i].chapter_id);
    return out;
}

std::string episode_text(const Story& story, const EpisodePlan& plan) {
    const int a = index_of(story, plan.from_chapter);
    if (a < 0) return {};
    const int b0 = index_of(story, plan.to_chapter);
    const int b = b0 < a ? a : b0;

    std::string out;
    for (int i = a; i <= b; ++i) {
        const Chapter& c = story.chapters[i];
        const int len = c.text_len();
        // 头一章从 from_char 起；末章到 to_char 止；中间的整章都要。
        const int from = (i == a) ? plan.from_char : 0;
        const int to = (i == b) ? plan.to_char : len;
        const std::string part = slice_chars(c.text, from, to);
        if (text::strip_ws(part).empty()) continue;
        if (!out.empty()) out += "\n\n";
        out += part;
    }
    return out;
}

std::string script_tail(const std::string& script) {
    const std::vector<std::string> chars = text::utf8_chars(script);
    const std::size_t n = chars.size();
    if (n <= prompt::kStoryPrevTailMaxChars) return text::strip_ws(script);

    std::string tail;
    for (std::size_t i = n - prompt::kStoryPrevTailMaxChars; i < n; ++i) {
        tail += chars[i];
    }
    // 从半行中间开始的话，模型会把那半句当成一个完整的拍子去接。
    // 有换行就从第一个换行之后起。
    const std::size_t nl = tail.find('\n');
    if (nl != std::string::npos && nl + 1 < tail.size()) {
        tail = tail.substr(nl + 1);
    }
    return text::strip_ws(tail);
}

std::string render_script_context(const Story& story, const EpisodePlan& plan,
                                  const std::string& previous_tail) {
    std::string out;

    if (!story.logline.empty()) out += "【这个故事】" + story.logline + "\n";
    if (!story.tone.empty()) out += "【调子】" + story.tone + "\n";
    if (!out.empty()) out += "\n";

    out += "【人物】\n";
    for (const auto& c : story.characters) {
        out += c.name;
        if (!c.identity.empty()) out += "：" + c.identity;
        out += "。";
        if (!c.want.empty()) out += "他要的是：" + c.want + "。";
        out += "\n";
    }

    if (!story.relations.empty()) {
        out += "\n【关系】\n";
        for (const auto& r : story.relations) {
            out += r.a + " — " + r.b;
            if (!r.kind.empty()) out += "：" + r.kind;
            out += "。";
            if (!r.tension.empty()) out += r.tension + "。";
            out += "\n";
        }
    }

    // 前情提要：本集**之前**那些章，每章一句。
    //
    // 这是治失忆的那一味药。老路线带的是前三集的原文，取三集、截 4000 字符，
    // 写第五集时第一集已经不在上下文里了。压缩成每章一句之后，二十章也塞得下。
    //
    // **只取之前的。** 把后面的章也塞进去，模型会把还没发生的事当成已经
    // 发生的写——这个错在成片里表现成"剧透了自己"。
    const int first = [&] {
        for (std::size_t i = 0; i < story.chapters.size(); ++i) {
            if (story.chapters[i].chapter_id == plan.from_chapter) {
                return static_cast<int>(i);
            }
        }
        return 0;
    }();
    std::string recap;
    for (int i = 0; i < first; ++i) {
        const Chapter& c = story.chapters[i];
        recap += std::to_string(i + 1) + " " + c.title;
        if (!c.summary.empty()) recap += "：" + text::collapse_ws(c.summary);
        recap += "\n";
    }
    if (!recap.empty()) {
        out += "\n【前情提要】（之前发生过的事，不要再演一遍）\n";
        out += text::truncate_utf8(recap, prompt::kStoryRecapMaxChars);
    }

    const std::string tail = text::strip_ws(previous_tail);
    if (!tail.empty()) {
        out += "\n【上一集是这么结束的】\n";
        out += text::truncate_utf8(tail, prompt::kStoryPrevTailMaxChars);
        out += "\n";
    }

    // 这一集要拍的。有正文用正文，没展开正文就用章节梗概——大纲阶段就
    // 能先把剧本写出来，不必等逐章展开。
    out += "\n【这一集】\n";
    const std::string body = episode_text(story, plan);
    if (!body.empty()) {
        out += text::truncate_utf8(body, prompt::kStoryEpisodeMaxChars);
    } else {
        for (const auto& id : episode_chapters(story, plan)) {
            const Chapter* c = story.chapter_by_id(id);
            if (c == nullptr) continue;
            out += c->title;
            if (!c->summary.empty()) out += "：" + text::collapse_ws(c->summary);
            out += "\n";
        }
    }

    // 停在哪。**这一条是老路线完全没有的**：原来模型不知道自己该停在
    // 什么地方，结尾全凭它自己找一个落点，下一集接不接得上看运气。
    if (!plan.hook.empty()) {
        out += "\n【这一集要停在】" + plan.hook + "\n";
    }

    return out;
}

std::string build_script_prompt_from_story(
    const Story& story, const EpisodePlan& plan, StyleLine style_line,
    const std::vector<std::string>& characters,
    const std::string& previous_tail) {
    const char* hint = style_line == StyleLine::ANIME
                           ? prompt::kScriptHintAnime
                           : prompt::kScriptHintRealistic;

    std::string out;
    out += prompt::kStoryScriptSeg0;
    out += format_f0(plan.target_duration_s);
    out += prompt::kStoryScriptSeg1;
    out += hint;
    out += prompt::kStoryScriptSeg2;
    out += std::to_string(budget_chars(plan.target_duration_s));
    out += prompt::kStoryScriptRules;

    if (!characters.empty()) {
        out += prompt::kStoryScriptCharsPre;
        for (std::size_t i = 0; i < characters.size(); ++i) {
            if (i > 0) out += "、";
            out += characters[i];
        }
        out += prompt::kStoryScriptCharsPost;
    }

    out += prompt::kStoryScriptContextHead;
    out += render_script_context(story, plan, previous_tail);
    out += prompt::kStoryScriptTail;
    return out;
}

}  // namespace changji::stages
