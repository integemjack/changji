// 从正在生成的 JSON 里边生边抠一个字段。
//
// 这一组的重点是**流式独有的那几种切法**：一个转义序列被切在两个 token
// 中间、一个 \uXXXX 被切成四段、一个汉字的三个字节分三次到。攒齐了再解析
// 的代码永远碰不到这些，而它们错了的表现是正文里凭空多出几个反斜杠、
// 或者冒出一个乱码字——而那些字会**原样落进正文**，一路活到分镜表里去。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "stages/json_stream.hpp"

using changji::stages::JsonFieldStreamer;
using json = nlohmann::json;

namespace {

/// 按给定的块大小喂进去，返回解出来的全部。
std::string stream_in(const std::string& raw, std::size_t chunk) {
    JsonFieldStreamer s("text");
    std::string got;
    for (std::size_t i = 0; i < raw.size(); i += chunk) {
        got += s.feed(raw.substr(i, chunk));
    }
    return got;
}

}  // namespace

TEST_CASE("整段进来：抠出 text 的值") {
    const std::string raw = json{{"text", "第一段。"}, {"hooks", json::array()}}.dump();
    JsonFieldStreamer s("text");
    CHECK(s.feed(raw) == "第一段。");
    CHECK(s.done());
    // 收完之后再喂什么都不吐——后面那串 hooks 不该进正文
    CHECK(s.feed("随便什么").empty());
}

TEST_CASE("一个字节一个字节进来，结果一样") {
    // **这是这组用例存在的理由。** llama.cpp 一次给一个 token，而一个汉字
    // 可能被切成三段、一个 \n 可能和它的反斜杠分两次到。
    const std::string raw =
        json{{"text", "第一段。\n\n第二段：他说「走吧」。"}}.dump();
    const std::string want = "第一段。\n\n第二段：他说「走吧」。";
    for (std::size_t chunk : {1u, 2u, 3u, 5u, 17u, 4096u}) {
        CAPTURE(chunk);
        CHECK(stream_in(raw, chunk) == want);
    }
}

TEST_CASE("转义：\\n \\t \\\" \\\\ 都还原") {
    const std::string want = "换行\n制表\t引号\"反斜杠\\斜杠/";
    const std::string raw = json{{"text", want}}.dump();
    for (std::size_t chunk : {1u, 2u, 3u, 4096u}) {
        CAPTURE(chunk);
        CHECK(stream_in(raw, chunk) == want);
    }
}

TEST_CASE("\\uXXXX 被切成四段也认得") {
    // 有的后端会把中文编成 中文。四位十六进制分几次到都要收得住。
    const std::string raw = R"({"text":"中文"})";
    for (std::size_t chunk : {1u, 2u, 3u, 5u, 4096u}) {
        CAPTURE(chunk);
        CHECK(stream_in(raw, chunk) == "中文");
    }
}

TEST_CASE("代理对：BMP 外的字也拼得回来") {
    // emoji 和生僻字是一对 \uXXXX。只认高位不等低位的话，吐出去的是
    // 半个码点——那就是一段非法 UTF-8，一路流到提示词和字幕。
    const std::string raw = R"({"text":"😀好"})";
    for (std::size_t chunk : {1u, 3u, 7u, 4096u}) {
        CAPTURE(chunk);
        const std::string got = stream_in(raw, chunk);
        CHECK(got == "\xF0\x9F\x98\x80好");
    }
}

TEST_CASE("前面别的字段不吐出去") {
    // 只推 text。JSON 外壳和 hooks 那一串不该出现在编辑器里。
    const std::string raw =
        R"({"title":"这一章的名字","text":"正文在这儿","hooks":[{"text":"钩子"}]})";
    for (std::size_t chunk : {1u, 4u, 4096u}) {
        CAPTURE(chunk);
        CHECK(stream_in(raw, chunk) == "正文在这儿");
    }
}

TEST_CASE("同名键在嵌套对象里不算数") {
    // hooks 里每一条也有 text。它们在 text 收完之后才出现，所以 done 了就
    // 不会被误收——这一条把那个顺序钉住。
    const std::string raw =
        R"({"text":"正文","hooks":[{"text":"钩子一"},{"text":"钩子二"}]})";
    CHECK(stream_in(raw, 1) == "正文");
    CHECK(stream_in(raw, 4096) == "正文");
}

TEST_CASE("值不是字符串就跳过，别把数字当正文") {
    const std::string raw = R"({"text":123,"other":"x"})";
    CHECK(stream_in(raw, 1).empty());
}

TEST_CASE("没生完也拿得到已经生出来的那些") {
    // 这才是常态：模型还在写，JSON 还没闭合。
    JsonFieldStreamer s("text");
    CHECK(s.feed(R"({"text":"写到一)") == "写到一");
    CHECK_FALSE(s.done());
    CHECK(s.feed("半") == "半");
    CHECK(s.text() == "写到一半");
    CHECK_FALSE(s.done());
}
