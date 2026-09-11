#include "stages/story_revise.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "stages/story_import.hpp"
#include "stages/story_revise_prompt.inc.hpp"
#include "util/text.hpp"

using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

/// 按 **UTF-8 字符**切。按字节切会把汉字劈成三段，截出来的是非法 UTF-8，
/// 它会一路流到提示词和字幕，最后表现成「整轨字幕不显示」这种离得很远的
/// 故障。和 script_story.cpp 里那个是同一件事。
std::string slice_chars(const std::string& s, int from, int to) {
    const std::vector<std::string> chars = text::utf8_chars(s);
    const int n = static_cast<int>(chars.size());
    const int a = std::clamp(from, 0, n);
    const int b = std::clamp(to, a, n);
    std::string out;
    for (int i = a; i < b; ++i) out += chars[i];
    return out;
}

const Chapter* find(const Story& story, const std::string& chapter_id) {
    return story.chapter_by_id(chapter_id);
}

}  // namespace

std::string span_text(const Story& story, const Span& span) {
    const Chapter* c = find(story, span.chapter_id);
    if (c == nullptr) return {};
    return slice_chars(c->text, span.from_char, span.to_char);
}

const ordered& revise_schema() {
    static const ordered schema = [] {
        ordered props = ordered::object();
        props["text"] = {
            {"type", "string"},
            {"description",
             "改完的这一段正文，直接拿去替换选中的那一段。"
             "只写这一段，不要把整章抄回来，也不要带章节标题"}};
        props["note"] = {
            {"type", "string"},
            {"description", "一句话说你改了什么。不进正文"}};
        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = ordered::array({"text"});
        return s;
    }();
    return schema;
}

std::string build_revise_prompt(const Story& story, const Span& span,
                                const std::string& instruction,
                                const std::vector<ReviseTurn>& history,
                                StyleLine style_line) {
    const Chapter* c = find(story, span.chapter_id);
    if (c == nullptr) throw std::runtime_error("没有这一章：" + span.chapter_id);

    std::string out;
    out += prompt::kReviseHead;
    out += style_line == StyleLine::ANIME ? prompt::kReviseHintAnime
                                          : prompt::kReviseHintRealistic;
    out += prompt::kReviseRules;

    // ---- 压缩的全局记忆。只要名字和身份 ----
    //
    // 改一段话用不着整份人物表，但**名字必须带**：不带的话模型会把"他"
    // 改成一个自己顺手起的名字，而那个名字在全剧其它地方一次都没出现过。
    if (!story.logline.empty()) out += "【这个故事】" + story.logline + "\n";
    if (!story.tone.empty()) out += "【调子】" + story.tone + "\n";
    if (!story.characters.empty()) {
        out += "【人物】";
        for (std::size_t i = 0; i < story.characters.size(); ++i) {
            if (i > 0) out += "；";
            out += story.characters[i].name;
            if (!story.characters[i].identity.empty()) {
                out += "（" + story.characters[i].identity + "）";
            }
        }
        out += "\n";
    }

    // ---- 上下文 ----
    const std::string before =
        slice_chars(c->text, span.from_char - kReviseContextChars,
                    span.from_char);
    const std::string after =
        slice_chars(c->text, span.to_char, span.to_char + kReviseContextChars);
    out += "\n【这一章】" + c->title + "\n";
    if (!before.empty()) out += "\n【选中那段前面】\n……" + before + "\n";
    out += "\n【选中要改的那一段】\n" + span_text(story, span) + "\n";
    if (!after.empty()) out += "\n【选中那段后面】\n" + after + "……\n";

    // ---- 来回 ----
    //
    // 用户说"再短一点"的时候，"一点"是相对上一版说的。丢了这段历史，
    // 模型只能从原文重新出发，于是改了三轮还在原地。
    if (!history.empty()) {
        out += prompt::kReviseHistoryHead;
        for (const auto& t : history) {
            const std::string who = t.role == "assistant" ? "你" : "作者";
            out += who + "：" + text::collapse_ws(t.text) + "\n";
        }
    }

    out += prompt::kReviseTaskHead;
    out += text::strip_ws(instruction);
    out += prompt::kReviseTail;
    return out;
}

Revision parse_revision(const std::string& raw, int span_chars) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text::strip_ws(raw));
    } catch (const std::exception&) {
        throw std::runtime_error("模型没回 JSON，改稿这一步走不下去");
    }
    if (!j.is_object() || !j.contains("text") || !j.at("text").is_string()) {
        throw std::runtime_error("模型回的东西里没有 text 那一段");
    }

    Revision r;
    r.text = text::strip_ws(j.at("text").get<std::string>());
    if (j.contains("note") && j.at("note").is_string()) {
        r.note = text::clean_field(j.at("note").get<std::string>());
    }
    if (r.text.empty()) throw std::runtime_error("改完是空的，没法替换");

    // **拦住"改着改着把整章吐回来"。** 那样落盘之后整章内容会翻倍，而界面
    // 上只显示"改好了"——多出来的那一份要等写剧本时才发现，那时候已经隔了
    // 好几步。变短是合法的（"把这段压缩成一句"就该变短），所以只拦上限。
    if (span_chars > 0) {
        const int cap = std::max(600, span_chars * 6);
        const int got = static_cast<int>(text::utf8_len(r.text));
        if (got > cap) {
            throw std::runtime_error(
                "改完有 " + std::to_string(got) + " 个字，而选中的只有 " +
                std::to_string(span_chars) +
                " 个——八成是把整章抄回来了。再试一次");
        }
    }
    return r;
}

Story apply_revision(const Story& story, const Span& span,
                     const std::string& text_in) {
    Story next = story;
    Chapter* c = next.chapter_by_id(span.chapter_id);
    if (c == nullptr) throw std::runtime_error("没有这一章：" + span.chapter_id);

    const int len = c->text_len();
    const int a = std::clamp(span.from_char, 0, len);
    const int b = std::clamp(span.to_char, a, len);

    const int delta = static_cast<int>(text::utf8_len(text_in)) - (b - a);
    c->text = slice_chars(c->text, 0, a) + text_in +
              slice_chars(c->text, b, len);

    // **候选切点必须跟着挪。** 它们是字符偏移，改一段字之后后面每一个都
    // 错位了——不管的话分集的切线会落在句子中间，而这件事不报任何错，
    // 只在成片里表现成"这一集从半句话开始"。
    //
    // **有说法的那些要保住。** 它们是读懂剧情标出来的真钩子，一集停在哪
    // 全靠它们（实跑里有名钩子把"停在真悬念上"的比例从 25% 抬到 56%）。
    // 图省事整章重算 paragraph_hooks 的话，这些会被一批无名的段落边界
    // 悄悄顶掉——每改一段就掉一批，而界面上一点反应都没有。
    std::vector<Hook> kept;
    for (const Hook& h : c->hooks) {
        if (h.text.empty()) continue;  // 无名的下面按新正文重算就是了
        if (h.at_char <= a) {
            // 切点落在改动之前：它说的是"这儿之前那段字悬着什么"，
            // 而那段字一个都没动。照留。
            kept.push_back(h);
        } else if (h.at_char > b) {
            Hook moved = h;
            moved.at_char = h.at_char + delta;
            kept.push_back(moved);
        }
        // (a, b] 这一段里的丢掉。**右端是闭的**：正好落在 b 上的那个切点，
        // 说的是"刚被换掉的那段字悬着什么"——字换了，那句说明就成了假的，
        // 而假的比没有更糟：分集会照着它把一集停在一个已经不存在的悬念上。
    }

    std::set<int> taken;
    for (const Hook& h : kept) taken.insert(h.at_char);
    for (const Hook& h : paragraph_hooks(c->text)) {
        if (taken.count(h.at_char) == 0) kept.push_back(h);
    }
    std::sort(kept.begin(), kept.end(),
              [](const Hook& x, const Hook& y) { return x.at_char < y.at_char; });
    c->hooks = std::move(kept);
    return next;
}

}  // namespace changji::stages
