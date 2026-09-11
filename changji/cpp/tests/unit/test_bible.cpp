// 角色圣经阶段的对拍测试。
//
// 语料由 tests/export_bible_golden.py 调**真实的 Python 函数**生成。
// 在这儿手写一份"期望输出"只能证明我理解得和自己一致，
// 证明不了和 Python 一致——而后者才是对拍要回答的问题。
//
// 提示词那几条是这里最重要的：拼接要求**逐字节**一致。差一个字不会报错，
// 只会让模型的输出悄悄变一点，而那种漂移要跑几十个镜头才看得出来。

#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "stages/bible.hpp"
#include "stages/json_extract.hpp"
#include "util/text.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/stage_bible.json";
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

}  // namespace

TEST_CASE("提示词和 Python 逐字节一致") {
    for (const auto& c : golden().at("prompts")) {
        const std::string style = c.at("style_line").get<std::string>();
        const std::string script = c.at("script").get<std::string>();
        const std::string want = c.at("prompt").get<std::string>();
        CAPTURE(style);

        const std::string got = stages::build_bible_prompt(script, line_from(style));
        if (got != want) {
            // 长文本直接比失败时看不出差在哪，先报第一个不同的位置
            std::size_t i = 0;
            while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
            MESSAGE("第一处不同在字节 " << i);
            MESSAGE("期望…" << want.substr(i > 40 ? i - 40 : 0, 90));
            MESSAGE("实得…" << got.substr(i > 40 ? i - 40 : 0, 90));
            MESSAGE("长度 期望 " << want.size() << " 实得 " << got.size());
        }
        CHECK(got == want);
    }
}

TEST_CASE("slug 和 Python 一致") {
    for (const auto& c : golden().at("slugs")) {
        const std::string in = c.at("input").get<std::string>();
        CAPTURE(in);
        CHECK(text::slug(in) == c.at("output").get<std::string>());
    }
}

TEST_CASE("SHA-1 自检") {
    // slug 的退路依赖它。摘要算错的话，中文角色名会得到一个稳定但错误的 id——
    // 稳定意味着测试不会因为随机性失败，错误意味着永远和 Python 对不上。
    // 用几个标准测试向量钉死。
    CHECK(text::sha1_hex("") ==
          "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(text::sha1_hex("abc") ==
          "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(text::sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // 长度卡在分组边界上，补位写错的话只有这几个会不对。
    // 55 是"补位刚好塞得下"，56 是"刚好塞不下、要多加一整个分组"，
    // 64 是整分组，119 跨两个分组。全部用 hashlib 核对过。
    CHECK(text::sha1_hex(std::string(55, 'a')) ==
          "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    CHECK(text::sha1_hex(std::string(56, 'a')) ==
          "c2db330f6083854c99d4b5bfb6e8f29f201be699");
    CHECK(text::sha1_hex(std::string(64, 'a')) ==
          "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    CHECK(text::sha1_hex(std::string(119, 'a')) ==
          "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56");
}

TEST_CASE("字段清洗和 Python 一致") {
    for (const auto& c : golden().at("cleans")) {
        const std::string in = c.at("input").get<std::string>();
        CAPTURE(in);
        CHECK(text::clean_field(in) == c.at("output").get<std::string>());
    }
}

TEST_CASE("从模型输出里抠 JSON") {
    for (const auto& c : golden().at("extracts")) {
        const std::string raw = c.at("raw").get<std::string>();
        CAPTURE(raw);
        if (c.at("ok").get<bool>()) {
            json got;
            REQUIRE_NOTHROW(got = stages::extract_json(raw));
            CHECK(got == c.at("value"));
        } else {
            // 只比"是否失败"。错误文字不算契约。
            CHECK_THROWS(stages::extract_json(raw));
        }
    }
}

TEST_CASE("抠不出 JSON 时的错误消息按字符截，不按字节") {
    // 2026-09-11 实跑：模型吐了一大段中文没收口，错误消息里 raw.substr(0, 400)
    // 截在半个汉字上，进了任务快照之后 /api/script/series 序列化 JSON 直接 500，
    // 批量写正文的进度就再也看不见了。
    std::string raw;
    for (int i = 0; i < 300; ++i) raw += "这是一段没有大括号的中文，";
    try {
        stages::extract_json(raw);
        FAIL("应该抛");
    } catch (const std::exception& e) {
        const std::string msg = e.what();
        CHECK(msg.find("找不到合法 JSON") != std::string::npos);
        // 能装进 JSON 再 dump 出来，就说明没截在字节中间
        CHECK_NOTHROW(json(msg).dump());
        CHECK(text::utf8_len(msg) < 450);
    }
}

TEST_CASE("默认负向提示词") {
    for (const auto& c : golden().at("negatives")) {
        const std::string style = c.at("style_line").get<std::string>();
        CAPTURE(style);
        CHECK(stages::default_negative(line_from(style)) ==
              c.at("output").get<std::string>());
    }
}

TEST_CASE("解析出的资产库和 Python 一致") {
    for (const auto& c : golden().at("parses")) {
        const std::string style = c.at("style_line").get<std::string>();
        CAPTURE(style);
        const models::AssetLibrary lib = stages::parse_bible(
            c.at("raw").get<std::string>(), line_from(style),
            c.at("aspect_ratio").get<std::string>());

        json got;
        to_json(got, lib);
        if (got != c.at("assets")) {
            MESSAGE("期望 " << c.at("assets").dump(1));
            MESSAGE("实得 " << got.dump(1));
        }
        CHECK(got == c.at("assets"));
    }
}

TEST_CASE("该报错的都报错") {
    for (const auto& c : golden().at("parse_failures")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string raw = c.at("raw").get<std::string>();
        if (c.at("raises").get<bool>()) {
            CHECK_THROWS_AS(
                stages::parse_bible(raw, models::StyleLine::REALISTIC, "9:16"),
                stages::BibleError);
        } else {
            CHECK_NOTHROW(
                stages::parse_bible(raw, models::StyleLine::REALISTIC, "9:16"));
        }
    }
}

TEST_CASE("抠不出 JSON 时抛的是 BibleError，这是有意和 Python 不一样") {
    // Python 那边 bible._parse 里 _extract_json 抛的是 StoryboardError，
    // 而 /api/bible 只 catch BibleError——于是"模型吐了一坨不是 JSON 的东西"
    // 这个最常见的失败会穿到最外面变成 500，用户看到的是
    // "Internal Server Error" 而不是那句能照着做的提示。
    //
    // 这里统一成 BibleError，也就是 400。语料里记着 Python 的
    // python_http_status = 500，就是为了让这条差异有据可查而不是被忘掉。
    for (const auto& c : golden().at("parse_failures")) {
        if (!c.at("raises").get<bool>()) continue;
        CAPTURE(c.at("name").get<std::string>());
        CHECK_THROWS_AS(
            stages::parse_bible(c.at("raw").get<std::string>(),
                                models::StyleLine::REALISTIC, "9:16"),
            stages::BibleError);
    }

    // 确认语料里真有那条 500，别哪天 Python 改好了这里还留着注释
    bool has_500 = false;
    for (const auto& c : golden().at("parse_failures")) {
        if (c.value("python_http_status", 0) == 500) has_500 = true;
    }
    CHECK_MESSAGE(has_500,
                  "Python 侧似乎已经修好了，这条差异说明该从方案里删掉");
}

TEST_CASE("schema 结构和 Python 一致") {
    // 这个 schema 要塞进请求体约束模型输出。字段名错一个，
    // 模型就会按错的名字产出，然后解析阶段拿到一堆空字符串。
    CHECK(json(stages::bible_schema()) == golden().at("schema"));
}

TEST_CASE("UTF-8 截断不会切出半个汉字") {
    const std::string s = "林晚站在天台边缘";
    for (std::size_t n = 0; n <= 10; ++n) {
        const std::string got = text::truncate_utf8(s, n);
        CAPTURE(n);
        // 长度必须是 3 的倍数（全是三字节汉字），切一半就不是了
        CHECK(got.size() % 3 == 0);
        CHECK(got.size() <= s.size());
        // 必须是原串的前缀
        CHECK(s.compare(0, got.size(), got) == 0);
    }
    CHECK(text::truncate_utf8(s, 100) == s);
    CHECK(text::truncate_utf8("", 10).empty());
    // 中英混排
    CHECK(text::truncate_utf8("ab林c", 3) == "ab林");
}
