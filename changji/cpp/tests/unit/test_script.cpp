// 剧本 / 选题 / 预告片阶段的对拍测试。
//
// 语料由 tests/export_script_golden.py 调**真实的 Python 函数**生成。
//
// 这一阶段的难点在文本清洗那三个函数。它们要按 UTF-8 **字符**做事——
// 括号匹配、长度判断、截断都是。按字节做会把汉字劈成半个，
// 而那个半个字节会一路流进提示词和字幕。

#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "stages/script.hpp"
#include "util/text.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/stage_script.json";
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
        json j;
        in >> j;
        return j;
    }();
    return g;
}

models::StyleLine line_from(const std::string& s) {
    return s == "anime" ? models::StyleLine::ANIME : models::StyleLine::REALISTIC;
}

std::vector<std::string> strs(const json& j) {
    return j.get<std::vector<std::string>>();
}

/// 长文本比对失败时报第一处不同，直接比看不出差在哪。
void check_text(const std::string& got, const std::string& want) {
    if (got != want) {
        std::size_t i = 0;
        while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
        MESSAGE("第一处不同在字节 " << i << "，长度 期望 " << want.size()
                                  << " 实得 " << got.size());
        MESSAGE("期望…" << want.substr(i > 40 ? i - 40 : 0, 110));
        MESSAGE("实得…" << got.substr(i > 40 ? i - 40 : 0, 110));
    }
    CHECK(got == want);
}

}  // namespace

TEST_CASE("削掉整段外面套的括号") {
    for (const auto& c : golden().at("wrappers")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(stages::strip_wrapper(in) == c.at("out").get<std::string>());
    }
}

TEST_CASE("只削套住整段的那一层") {
    // 光数左右个数不够：「（甲说）乙答（丙笑）」左右各两个，数目相等，
    // 但首尾那两个并不是一对，削掉就把中间的括号弄错位了。
    CHECK(stages::strip_wrapper("（甲说）乙答（丙笑）") == "（甲说）乙答（丙笑）");
    CHECK(stages::strip_wrapper("（前）中间（后）") == "（前）中间（后）");
    // 真的套住整段的要削
    CHECK(stages::strip_wrapper("（他犹豫了一下）") == "他犹豫了一下");
    CHECK(stages::strip_wrapper("（（（三层）））") == "三层");
    // 削完是空的就不削
    CHECK(stages::strip_wrapper("（）") == "（）");
}

TEST_CASE("削掉开头的时间码") {
    for (const auto& c : golden().at("timecodes")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(stages::strip_leading_timecode(in) ==
              c.at("out").get<std::string>());
    }
}

TEST_CASE("时间码要既有数字又有单位才削") {
    // 只看数字的话，「（第3次）他又问」开头也会被削掉
    CHECK(stages::strip_leading_timecode("（他犹豫了）他开口") == "（他犹豫了）他开口");
    CHECK(stages::strip_leading_timecode("（第3次）他又问") == "（第3次）他又问");
    CHECK(stages::strip_leading_timecode("[0-3秒] 画面特写：雨水") == "画面特写：雨水");
}

TEST_CASE("说话人归一") {
    for (const auto& c : golden().at("speakers")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(stages::normalize_speaker(in) == c.at("out").get<std::string>());
    }
}

TEST_CASE("none 和旁白不能当成角色名") {
    // 原样当名字用的话，成片字幕上会出现「none：寂静」
    CHECK(stages::normalize_speaker("none").empty());
    CHECK(stages::normalize_speaker("None").empty());
    CHECK(stages::normalize_speaker("旁白").empty());
    CHECK(stages::normalize_speaker("（无）").empty());
    // 正常名字要留着
    CHECK(stages::normalize_speaker("林晚") == "林晚");
    CHECK(stages::normalize_speaker("（林晚）") == "林晚");
}

TEST_CASE("对白字数预算") {
    for (const auto& c : golden().at("budgets")) {
        const double in = c.at("in").get<double>();
        CAPTURE(in);
        CHECK(stages::budget_chars(in) == c.at("out").get<int>());
    }
    // 下限 20 字。时长再短也得能说一句话。
    CHECK(stages::budget_chars(0.0) == 20);
    CHECK(stages::budget_chars(-100.0) == 20);
}

TEST_CASE("三个提示词都和 Python 逐字节一致") {
    for (const auto& c : golden().at("prompts")) {
        const std::string kind = c.at("kind").get<std::string>();
        const auto line = line_from(c.at("style_line").get<std::string>());
        CAPTURE(kind);
        CAPTURE(c.at("style_line").get<std::string>());

        std::string got;
        if (kind == "script") {
            CAPTURE(c.at("duration_s").get<double>());
            got = stages::build_script_prompt(
                c.at("premise").get<std::string>(),
                c.at("duration_s").get<double>(), line,
                c.at("previous").get<std::string>(),
                strs(c.at("characters")));
        } else if (kind == "premise") {
            got = stages::build_premise_prompt(
                c.at("keywords").get<std::string>(), line,
                c.at("count").get<int>(), strs(c.at("existing")));
        } else {
            got = stages::build_trailer_prompt(
                c.at("premise").get<std::string>(),
                c.at("duration_s").get<double>(), line,
                c.at("episodes").get<std::string>(),
                strs(c.at("characters")));
        }
        check_text(got, c.at("prompt").get<std::string>());
    }
}

TEST_CASE("时长格式化是银行家舍入") {
    // Python 的 f"{x:.0f}" 和 C 的 %.0f 都是 round-half-to-even。
    // 用四舍五入的话 0.5 会变成 1，提示词里那个数字就和 Python 不一样了。
    const std::string p0 = stages::build_script_prompt(
        "梗概", 0.5, models::StyleLine::REALISTIC);
    CHECK(p0.find("总时长约 0 秒") != std::string::npos);
    const std::string p1 = stages::build_script_prompt(
        "梗概", 1.5, models::StyleLine::REALISTIC);
    CHECK(p1.find("总时长约 2 秒") != std::string::npos);
    const std::string p2 = stages::build_script_prompt(
        "梗概", 2.5, models::StyleLine::REALISTIC);
    CHECK(p2.find("总时长约 2 秒") != std::string::npos);
}

TEST_CASE("超长的前情和剧集要按字符截断") {
    // 按字节截会把最后一个汉字劈成半个，那半个字节直接进提示词。
    const std::string long_prev(5000, 'x');  // 先用 ASCII 确认长度逻辑
    const std::string p = stages::build_script_prompt(
        "梗概", 60.0, models::StyleLine::REALISTIC, long_prev);
    CHECK(p.find(std::string(4000, 'x')) != std::string::npos);
    CHECK(p.find(std::string(4001, 'x')) == std::string::npos);

    SUBCASE("中文的截断点在字符边界上") {
        std::string cn;
        for (int i = 0; i < 5000; ++i) cn += "很";
        const std::string q = stages::build_script_prompt(
            "梗概", 60.0, models::StyleLine::REALISTIC, cn);
        // 截出来的那段必须是 4000 个完整的「很」
        std::string want;
        for (int i = 0; i < 4000; ++i) want += "很";
        CHECK(q.find(want) != std::string::npos);
        CHECK(q.find(want + "很") == std::string::npos);
    }
}

TEST_CASE("解析出的剧本和 Python 一致") {
    for (const auto& c : golden().at("parses")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const stages::ScriptDraft d =
            stages::parse_script(c.at("raw").get<std::string>());

        CHECK(d.title == c.at("title").get<std::string>());
        CHECK(d.logline == c.at("logline").get<std::string>());

        json beats = json::array();
        for (const stages::Beat& b : d.beats) {
            beats.push_back({{"kind", b.kind},
                             {"speaker", b.speaker},
                             {"text", b.text}});
        }
        if (beats != c.at("beats")) {
            MESSAGE("期望 " << c.at("beats").dump(1));
            MESSAGE("实得 " << beats.dump(1));
        }
        CHECK(beats == c.at("beats"));
        CHECK(d.speakers() == strs(c.at("speakers")));
        CHECK(d.dialogue_chars() == c.at("dialogue_chars").get<std::size_t>());
        check_text(d.render(), c.at("render").get<std::string>());
    }
}

TEST_CASE("解析时的三处兜底") {
    const json& c = golden().at("parses").at(0);
    const stages::ScriptDraft d =
        stages::parse_script(c.at("raw").get<std::string>());

    // 一，说了话却没说是谁说的，降级成动作而不是丢掉。
    //     丢掉的话这句台词就从成片里消失了，那比配错声音还糟。
    bool found_demoted = false;
    for (const auto& b : d.beats) {
        if (b.text == "远处传来汽笛声。") {
            CHECK(b.kind == "action");
            CHECK(b.speaker.empty());
            found_demoted = true;
        }
    }
    CHECK(found_demoted);

    // 二，开头的时间码和外层引号都被削掉了
    for (const auto& b : d.beats) {
        CHECK(b.text.rfind("[0-3秒]", 0) != 0);
        CHECK(b.text.rfind("“", 0) != 0);
    }

    // 三，空文本的那一拍整条丢掉
    for (const auto& b : d.beats) CHECK_FALSE(b.text.empty());
}

TEST_CASE("剧本该报错的都报错") {
    for (const auto& c : golden().at("parse_failures")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string raw = c.at("raw").get<std::string>();
        if (c.at("raises").get<bool>()) {
            CHECK_THROWS_AS(stages::parse_script(raw), stages::ScriptError);
        } else {
            CHECK_NOTHROW(stages::parse_script(raw));
        }
    }
}

TEST_CASE("解析出的选题和 Python 一致") {
    for (const auto& c : golden().at("premise_parses")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const auto ideas =
            stages::parse_premises(c.at("raw").get<std::string>());

        json got = json::array();
        for (const auto& i : ideas) {
            got.push_back({{"title", i.title},
                           {"premise", i.premise},
                           {"hook", i.hook}});
        }
        if (got != c.at("ideas")) {
            MESSAGE("期望 " << c.at("ideas").dump(1));
            MESSAGE("实得 " << got.dump(1));
        }
        CHECK(got == c.at("ideas"));
    }
}

TEST_CASE("选题该报错的都报错") {
    for (const auto& c : golden().at("premise_failures")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string raw = c.at("raw").get<std::string>();
        if (c.at("raises").get<bool>()) {
            CHECK_THROWS_AS(stages::parse_premises(raw), stages::ScriptError);
        } else {
            CHECK_NOTHROW(stages::parse_premises(raw));
        }
    }
}

TEST_CASE("渲染成后面几步认的写法") {
    for (const auto& c : golden().at("renders")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        stages::ScriptDraft d;
        d.title = "t";
        d.logline = "l";
        for (const auto& b : c.at("beats")) {
            d.beats.push_back(stages::Beat{b.at("kind").get<std::string>(),
                                           b.at("speaker").get<std::string>(),
                                           b.at("text").get<std::string>()});
        }
        check_text(d.render(), c.at("render").get<std::string>());
        CHECK(d.speakers() == strs(c.at("speakers")));
        CHECK(d.dialogue_chars() == c.at("dialogue_chars").get<std::size_t>());
    }
}

TEST_CASE("对白字数按字符不按字节") {
    // 中文一个字三字节。用 size() 的话预算判断会偏大三倍，
    // 每一集都会被判成"超出很多"。
    stages::ScriptDraft d;
    d.beats.push_back(stages::Beat{"dialogue", "林晚", "你说过会来的"});
    CHECK(d.dialogue_chars() == 6);
    CHECK(text::utf8_len("你说过会来的") == 6);
}

TEST_CASE("两个 schema 和 Python 一致") {
    CHECK(json(stages::script_schema()) == golden().at("script_schema"));
    CHECK(json(stages::premise_schema()) == golden().at("premise_schema"));
}
