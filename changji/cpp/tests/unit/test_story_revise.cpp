// 改原稿的某一段。
//
// 这一组用例守的是同一件事：**改动范围可预期**。用户敢把写了一半的稿子
// 交给 AI，全靠"它只动我圈出来的那几行"；一旦它顺手改了别处，哪怕改得更
// 好，这个功能也就没人敢按第二次——因为他没法知道还有哪儿被动过。
//
// 位置一律按 UTF-8 字符算。按字节算的话切线会落在汉字中间，截出来的是
// 非法 UTF-8，一路流到提示词和字幕，最后表现成「整轨字幕不显示」。

#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "models/story.hpp"
#include "stages/story_revise.hpp"
#include "util/text.hpp"

using namespace changji;
using namespace changji::models;
using namespace changji::stages;
using json = nlohmann::json;

namespace {

/// 三段中文，段间空行。字符数是刻意数过的：
/// 「第一段。」4 字 + 「\n\n」2 + 「第二段在中间。」7 + 「\n\n」2 + 「第三段。」4
Story a_story() {
    Story s;
    s.logline = "深夜便利店，前任推门进来";
    s.tone = "克制";
    StoryCharacter c;
    c.name = "林晚";
    c.identity = "便利店夜班";
    s.characters.push_back(c);

    Chapter ch;
    ch.chapter_id = "ch01";
    ch.title = "伞与雨";
    ch.text = "第一段。\n\n第二段在中间。\n\n第三段。";
    // 一个有说法的钩子在第三段之前，一个在章尾
    ch.hooks.push_back({4, ""});           // 无名，第一段后
    ch.hooks.push_back({15, "他推门进来"});  // 有说法，第二段后
    ch.hooks.push_back({19, "伞留在门口"});  // 有说法，章尾
    s.chapters.push_back(ch);
    return s;
}

Span mid_span() {
    // 「第二段在中间。」在 [6, 13)
    return Span{"ch01", 6, 13};
}

}  // namespace

TEST_CASE("取出来的就是选中那一段，按字符不按字节") {
    const Story s = a_story();
    CHECK(span_text(s, mid_span()) == "第二段在中间。");
    // 越界夹住，不抛——这是给提示词拼上下文用的，抛了整条路走不下去
    CHECK(span_text(s, Span{"ch01", -50, 4}) == "第一段。");
    CHECK(span_text(s, Span{"ch01", 0, 9999}) == s.chapters[0].text);
    CHECK(span_text(s, Span{"没这一章", 0, 4}).empty());
}

TEST_CASE("提示词里要有选中那段、前后文、和人物名") {
    const Story s = a_story();
    const std::string p =
        build_revise_prompt(s, mid_span(), "这儿太赶了，铺一下情绪", {},
                            StyleLine::REALISTIC);

    CHECK(p.find("第二段在中间。") != std::string::npos);
    // 前后文：改一段话要知道它前面刚发生了什么、后面马上要发生什么，
    // 否则最常见的毛病是把前面已经交代过的事又交代一遍
    CHECK(p.find("第一段。") != std::string::npos);
    CHECK(p.find("第三段。") != std::string::npos);
    // **人物名必须带。** 不带的话模型会把"他"改成一个自己顺手起的名字，
    // 而那个名字在全剧其它地方一次都没出现过
    CHECK(p.find("林晚") != std::string::npos);
    CHECK(p.find("这儿太赶了，铺一下情绪") != std::string::npos);
    // 这一条是整个设计的地基
    CHECK(p.find("只改选中的那一段") != std::string::npos);
}

TEST_CASE("对话形式：之前的来回要带上") {
    const Story s = a_story();
    const std::vector<ReviseTurn> history = {
        {"user", "铺一下情绪"},
        {"assistant", "把那段拉长了一点"},
        {"user", "再短一点"},
    };
    const std::string p = build_revise_prompt(s, mid_span(), "再短一点", history,
                                              StyleLine::REALISTIC);
    // 用户说"再短一点"的时候，"一点"是相对上一版说的。丢了这段历史，
    // 模型只能从原文重新出发，于是改了三轮还在原地。
    CHECK(p.find("铺一下情绪") != std::string::npos);
    CHECK(p.find("把那段拉长了一点") != std::string::npos);
    CHECK(p.find("作者：") != std::string::npos);
    CHECK(p.find("你：") != std::string::npos);
}

TEST_CASE("解析：要 text，空的不行") {
    CHECK(parse_revision(json{{"text", "改好的一段"}}.dump(), 7).text ==
          "改好的一段");
    CHECK(parse_revision(json{{"text", "改好的"}, {"note", "铺了情绪"}}.dump(), 7)
              .note == "铺了情绪");

    CHECK_THROWS(parse_revision("模型今天想聊点别的", 7));
    CHECK_THROWS(parse_revision(json{{"note", "只有说明"}}.dump(), 7));
    CHECK_THROWS(parse_revision(json{{"text", "   "}}.dump(), 7));
}

TEST_CASE("解析：拦住把整章抄回来") {
    // 那样落盘之后整章内容会翻倍，而界面上只显示"改好了"——多出来的那一份
    // 要等写剧本时才发现，那时候已经隔了好几步。
    std::string huge;
    for (int i = 0; i < 400; ++i) huge += "很长的一段话。";
    CHECK_THROWS(parse_revision(json{{"text", huge}}.dump(), 7));

    // **变短是合法的。**「把这段压缩成一句」就该变短，拦下限会把一个正当
    // 的要求变成报错。
    CHECK(parse_revision(json{{"text", "一句。"}}.dump(), 700).text == "一句。");
    // 传 0 表示不检查长度
    CHECK(parse_revision(json{{"text", huge}}.dump(), 0).text == huge);
}

TEST_CASE("写回去：只换选中那一段，前后一个字不动") {
    const Story s = a_story();
    const Story next = apply_revision(s, mid_span(), "换过的第二段。");

    CHECK(next.chapters[0].text == "第一段。\n\n换过的第二段。\n\n第三段。");
    // 别的字段不受影响
    CHECK(next.chapters[0].title == s.chapters[0].title);
    CHECK(next.logline == s.logline);
    // 原来那份没被改（值语义，不是就地改）
    CHECK(s.chapters[0].text == "第一段。\n\n第二段在中间。\n\n第三段。");
}

TEST_CASE("写回去：有说法的钩子跟着挪，别被无名的顶掉") {
    const Story s = a_story();
    // 换成更长的一段：后面的钩子要往后挪
    const Story next = apply_revision(s, mid_span(), "换过的第二段长一些。");
    const int delta = 10 - 7;  // 新的 10 字，原来 7 字

    auto named_at = [](const Story& st, const std::string& what) -> int {
        for (const auto& h : st.chapters[0].hooks) {
            if (h.text == what) return h.at_char;
        }
        return -1;
    };

    // **有说法的钩子是一集停在哪的全部依据**（实跑里它把"停在真悬念上"
    // 的比例从 25% 抬到 56%）。图省事整章重算 paragraph_hooks 的话，这些
    // 会被一批无名的段落边界悄悄顶掉——每改一段就掉一批，界面上毫无反应。
    CHECK(named_at(next, "他推门进来") == 15 + delta);
    CHECK(named_at(next, "伞留在门口") == 19 + delta);
    // 位置是单调不减的：分集算法在有序集合里找最近切点，乱序会找错
    for (std::size_t i = 1; i < next.chapters[0].hooks.size(); ++i) {
        CHECK(next.chapters[0].hooks[i - 1].at_char <=
              next.chapters[0].hooks[i].at_char);
    }
}

TEST_CASE("写回去：落在被换掉那段里面的钩子丢掉") {
    Story s = a_story();
    // 在选中区间**内部**加一个有说法的钩子
    s.chapters[0].hooks.push_back({9, "这句话马上要没了"});

    const Story next = apply_revision(s, mid_span(), "换过的。");
    for (const auto& h : next.chapters[0].hooks) {
        // 它指着的那句话已经不在了，留着的话分集会切在一个不存在的悬念上
        CHECK(h.text != "这句话马上要没了");
    }
}

TEST_CASE("写回去：越界夹住，不越过正文两头") {
    const Story s = a_story();
    const int len = s.chapters[0].text_len();

    const Story a = apply_revision(s, Span{"ch01", -10, 4}, "开头。");
    CHECK(a.chapters[0].text.rfind("开头。", 0) == 0);

    const Story b = apply_revision(s, Span{"ch01", len, len + 99}, "结尾。");
    CHECK(b.chapters[0].text.size() > s.chapters[0].text.size());
    CHECK(b.chapters[0].text.find("第三段。结尾。") != std::string::npos);

    CHECK_THROWS(apply_revision(s, Span{"没这一章", 0, 1}, "x"));
}

TEST_CASE("写回去：整章换掉也行，故事照样校验得过") {
    const Story s = a_story();
    const Story next =
        apply_revision(s, Span{"ch01", 0, s.chapters[0].text_len()},
                       "整章重写了一遍。\n\n第二段。");
    CHECK(next.chapters[0].text == "整章重写了一遍。\n\n第二段。");
    CHECK(next.validate().empty());
    // 钩子全在被换掉的范围里，所以有说法的一个都不剩，只有新算的段落边界
    for (const auto& h : next.chapters[0].hooks) CHECK(h.text.empty());
}
