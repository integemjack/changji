// 字幕断行与 ASS 输出的对拍。
//
// 语料由 tools/gen_subtitles_golden.py 生成，期望值由 Python 的真函数算出来。
//
// ASS 的**整份文件内容**都比对：它要喂给 libass，多一个空格少一个逗号
// 都可能让整条字幕轨不渲染，而那要到成片出来才看得见。
//
// 断行错了更隐蔽——不会不渲染，只是断在词中间，观感差一点。
// 一集几十条字幕，人不会逐条去看。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "media/subtitles.hpp"
#include "util/text.hpp"
#include "util/paths.hpp"

using namespace changji;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json load_corpus() {
    const fs::path p =
        fs::path(CHANGJI_GOLDEN_DIR) / "subtitles" / "subtitles.json";
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(),
                    "语料不在：" << paths::to_utf8(p)
                                 << "，跑一遍 tools/gen_subtitles_golden.py");
    std::ostringstream buf;
    buf << in.rdbuf();
    json doc = json::parse(buf.str(), nullptr, false);
    REQUIRE_FALSE(doc.is_discarded());
    return doc;
}

std::vector<media::SubtitleCue> cues_of(const json& arr) {
    std::vector<media::SubtitleCue> out;
    for (const auto& c : arr) {
        media::SubtitleCue cue;
        cue.start_s = c.at("start_s");
        cue.end_s = c.at("end_s");
        cue.text = c.at("text");
        cue.style = c.at("style");
        out.push_back(cue);
    }
    return out;
}

/// 两份文本第一处不一样在哪儿。直接比的话报错只说"两个大串不等"。
std::string first_diff(const std::string& got, const std::string& want) {
    const std::size_t n = std::min(got.size(), want.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (got[i] != want[i]) {
            const std::size_t from = i > 40 ? i - 40 : 0;
            return "第 " + std::to_string(i) + " 字节起不同：\n  得到 ..." +
                   got.substr(from, 90) + "\n  期望 ..." + want.substr(from, 90);
        }
    }
    if (got.size() != want.size()) {
        return "长度不同：" + std::to_string(got.size()) + " vs " +
               std::to_string(want.size()) + "，多出来的是：" +
               (got.size() > want.size() ? got.substr(n, 80) : want.substr(n, 80));
    }
    return {};
}

}  // namespace

TEST_CASE("显示宽度：全角算一，半角算半") {
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("width")) {
        const std::string t = c.at("text");
        CAPTURE(t);
        CHECK(media::display_width(t) ==
              doctest::Approx(c.at("expected").get<double>()));
    }

    SUBCASE("区间表是从 Python 的 unicodedata 导出的，不是手写的") {
        // 手写必然有出入，而出入的表现不是报错，是某几句字幕断行位置
        // 和 Python 不一样——那要逐帧比对成片才看得出来。
        CHECK(media::is_wide(U'你'));
        CHECK(media::is_wide(U'，'));
        CHECK(media::is_wide(U'Ａ'));      // 全角拉丁字母
        CHECK(media::is_wide(U'🎉'));      // 基本平面之外
        CHECK_FALSE(media::is_wide(U'a'));
        CHECK_FALSE(media::is_wide(U'，' - 0xFEE0));   // 对应的半角逗号
        CHECK_FALSE(media::is_wide(U'①'));            // 圈号不是 W/F
    }
}

TEST_CASE("中文断行：逐个案例和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("wrap").size() >= 13);

    for (const auto& c : corpus.at("wrap")) {
        const std::string t = c.at("text");
        const double w = c.at("max_width");
        const int lines = c.at("max_lines");
        CAPTURE(t);
        CAPTURE(w);
        CAPTURE(lines);

        const auto got = media::wrap_chinese(t, w, lines);
        const auto want = c.at("expected").get<std::vector<std::string>>();
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            CHECK(got[i] == want[i]);
        }
    }
}

TEST_CASE("断行不丢字") {
    // 超过行数上限时，塞不下的部分并进最后一行。丢字是最糟的：
    // 观众看到的是一句没说完的话，而且不会有任何报错。
    const std::string long_line =
        "第一句话说完了，第二句话也说完了，第三句话还在继续说个不停。";
    const auto lines = media::wrap_chinese(long_line, 10.0, 2);
    std::string joined;
    for (const auto& l : lines) joined += l;
    // 断行只会去掉行首行尾的空白，中文没有空格，所以应该一个字不少
    CHECK(joined == long_line);
}

TEST_CASE("断行不产出非法 UTF-8") {
    // 按字节算下标的话，一个中文字会被劈成三段——那个串会进 ASS 文件，
    // 播放器直接不显示整条字幕轨。
    const auto lines = media::wrap_chinese(
        "这是一句没有任何标点符号的很长很长的中文台词需要按宽度断开", 7.0, 2);
    for (const auto& l : lines) {
        CAPTURE(l);
        // 每个字符的首字节都不能是续接字节（10xxxxxx）
        CHECK((static_cast<unsigned char>(l.front()) & 0xC0) != 0x80);
        // 长度必须是完整字符之和
        std::size_t i = 0;
        while (i < l.size()) {
            const std::size_t n =
                text::utf8_char_len(static_cast<unsigned char>(l[i]));
            REQUIRE(i + n <= l.size());
            i += n;
        }
        CHECK(i == l.size());
    }
}

TEST_CASE("ASS 时间格式") {
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("ass_time")) {
        const double t = c.at("input");
        CAPTURE(t);
        CHECK(media::ass_time(t) == c.at("expected").get<std::string>());
    }

    SUBCASE("负数按 0 算") {
        // 时间轴上出现负数说明上游算错了，但字幕文件本身不能因此不合法——
        // 一个 -1:59:59 会让 libass 整份文件都不解析。
        CHECK(media::ass_time(-5.0) == "0:00:00.00");
    }
}

TEST_CASE("ASS 整份文件逐字节和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("ass").size() >= 5);

    for (const auto& c : corpus.at("ass")) {
        const std::string name = c.at("name");
        CAPTURE(name);
        const std::string got = media::build_ass(cues_of(c.at("cues")));
        const std::string want = c.at("expected");
        const std::string diff = first_diff(got, want);
        CHECK_MESSAGE(diff.empty(), name << "：" << diff);
    }
}

TEST_CASE("写出来的字幕文件带 UTF-8 BOM") {
    // 一些播放器靠它才认出中文，没有的话按本地代码页解，出来的是一屏乱码。
    const fs::path dir =
        fs::temp_directory_path() / paths::from_utf8("changji_字幕");
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path dest = dir / paths::from_utf8("第一集.ass");

    media::write_ass(dest, {media::SubtitleCue{0.0, 2.0, "你好", "dialogue"}});

    std::ifstream in(dest, std::ios::binary);
    REQUIRE(in.good());
    std::string head(3, '\0');
    in.read(head.data(), 3);
    CHECK(head == "\xEF\xBB\xBF");

    SUBCASE("父目录会自动建出来") {
        CHECK(fs::exists(dest));
    }
}

TEST_CASE("字幕自检：逐个案例和 Python 对上") {
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("validate")) {
        const std::string name = c.at("name");
        CAPTURE(name);
        const auto got = media::validate_cues(cues_of(c.at("cues")));
        const auto want = c.at("expected").get<std::vector<std::string>>();
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            CHECK(got[i] == want[i]);
        }
    }
}
