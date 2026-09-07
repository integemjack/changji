// 配音时长估算与拆分的对拍。
//
// 语料由 tools/gen_audio_golden.py 生成，期望值由 Python 的真函数算出来。
//
// **这一层错了的后果是成片里两个人同时说话。** 一个镜头装不下自己的台词，
// 混音时后面的声音盖到下一镜上去。而这件事在装配之前没有任何迹象——
// 分镜表看着正常，每一段音频单独听也正常，要把整集拼出来听一遍才发现。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "stages/audio_plan.hpp"
#include "stages/render.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using namespace changji;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json load_corpus() {
    const fs::path p = fs::path(CHANGJI_GOLDEN_DIR) / "audio" / "audio_plan.json";
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "语料不在：" << paths::to_utf8(p)
                                            << "，跑一遍 tools/gen_audio_golden.py");
    std::ostringstream buf;
    buf << in.rdbuf();
    json doc = json::parse(buf.str(), nullptr, false);
    REQUIRE_FALSE(doc.is_discarded());
    return doc;
}

models::Shot shot_of(const json& j) {
    models::Shot s;
    s.shot_id = j.at("shot_id");
    s.order = j.at("order");
    s.attempts = j.at("attempts");
    s.duration_locked = j.at("duration_locked");
    if (!j.at("frame_path").is_null()) s.frame_path = j.at("frame_path");
    if (!j.at("video_path").is_null()) s.video_path = j.at("video_path");
    for (const auto& l : j.at("dialogue")) {
        models::DialogueLine line;
        line.text = l.at("text");
        line.char_id = "c_lin";
        if (!l.at("actual_duration_s").is_null()) {
            line.actual_duration_s = l.at("actual_duration_s").get<double>();
        }
        s.dialogue.push_back(line);
    }
    return s;
}

}  // namespace

TEST_CASE("常数和 Python 一致") {
    // 这几个数一动，全集的镜头时长和拆分位置都跟着变。
    const json corpus = load_corpus();
    const auto& c = corpus.at("constants");
    CHECK(stages::kCharsPerSecond == doctest::Approx(c.at("chars_per_second")));
    CHECK(stages::kLeadInS == doctest::Approx(c.at("lead_in_s")));
    CHECK(stages::kTailS == doctest::Approx(c.at("tail_s")));
    CHECK(stages::max_line_seconds(24) ==
          doctest::Approx(c.at("max_line_seconds_24fps")));
    CHECK(stages::max_line_seconds(30) ==
          doctest::Approx(c.at("max_line_seconds_30fps")));

    SUBCASE("单镜上限由帧数上限推导，不是各写一份") {
        // 早先档位表里有 8 秒和 10 秒，而实际上限是 5 秒，
        // 多出来的部分被静默截断，成片比计划短了一大截且没人发现。
        CHECK(stages::max_shot_duration_s(24) ==
              doctest::Approx(stages::kMaxFrames / 24.0));
    }
}

TEST_CASE("台词时长估算：逐条和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("duration").size() >= 12);
    for (const auto& c : corpus.at("duration")) {
        const std::string t = c.at("text");
        CAPTURE(t);
        CHECK(stages::estimate_speech_duration(t) ==
              doctest::Approx(c.at("expected").get<double>()));
    }
}

TEST_CASE("标点不发音但产生停顿") {
    // 把标点当成字算的话，一句"什么？！"会被估成四个字的长度，
    // 而它实际上要停顿快一秒。估短了的后果是配音装不进镜头。
    const double with_marks = stages::estimate_speech_duration("什么？！");
    const double without = stages::estimate_speech_duration("什么");
    CHECK(with_marks > without);
    // 两个感叹号级别的停顿是 0.7 秒，比两个字（0.43 秒）还长
    CHECK(with_marks - without == doctest::Approx(0.70));

    SUBCASE("空白全部去掉，不只是首尾") {
        CHECK(stages::estimate_speech_duration("  我 等 你  ") ==
              doctest::Approx(stages::estimate_speech_duration("我等你")));
    }

    SUBCASE("空串是 0，不是头尾留白那点") {
        // 返回 0.4 的话，没有台词的镜头也会占掉一点预算，
        // 四十镜下来整集时长多出十几秒。
        CHECK(stages::estimate_speech_duration("") == 0.0);
        CHECK(stages::estimate_speech_duration("   ") == 0.0);
    }
}

TEST_CASE("切句：逐条和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("split").size() >= 8);
    for (const auto& c : corpus.at("split")) {
        const std::string t = c.at("text");
        const double m = c.at("max_seconds");
        CAPTURE(t);
        CAPTURE(m);
        const auto got = stages::split_long_text(t, m);
        const auto want = c.at("expected").get<std::vector<std::string>>();
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) CHECK(got[i] == want[i]);
    }
}

TEST_CASE("切句不丢字") {
    // 丢字的后果是观众听到一句没说完的话，而字幕上是完整的——
    // 那比两边都缺更让人困惑。
    const std::string long_line =
        "这是一句完全没有任何标点符号的很长很长很长的台词需要按照字数硬切开来才行";
    const auto pieces = stages::split_long_text(long_line, 1.5);
    std::string joined;
    for (const auto& p : pieces) joined += p;
    CHECK(joined == long_line);
    CHECK(pieces.size() > 1);
}

TEST_CASE("切出来的每一段都不产出非法 UTF-8") {
    // 硬切按字数走。按字节切的话一个中文字会被劈成三段，
    // 那个串会送进 TTS 引擎，回来的要么是报错要么是一段乱读。
    for (const double limit : {0.3, 1.0, 2.0}) {
        CAPTURE(limit);
        for (const auto& p :
             stages::split_long_text("一二三四五六七八九十你我他她它", limit)) {
            CAPTURE(p);
            std::size_t i = 0;
            while (i < p.size()) {
                const std::size_t n =
                    text::utf8_char_len(static_cast<unsigned char>(p[i]));
                REQUIRE(i + n <= p.size());
                i += n;
            }
            CHECK(i == p.size());
        }
    }
}

TEST_CASE("新镜编号跟全集比对着发") {
    // 同一集重跑一次配音会再拆一次。只按本次的序号取名的话，
    // 第二次又会取出一个 sh001_b，于是一集里出现两个同名镜头：
    // 按 id 找镜头只能找到头一个，音频和首帧的文件名也会互相覆盖。
    const json corpus = load_corpus();
    for (const auto& c : corpus.at("free_id")) {
        const std::string base = c.at("base");
        std::set<std::string> used;
        for (const auto& u : c.at("used")) used.insert(u.get<std::string>());
        CAPTURE(base);
        CHECK(stages::free_shot_id(base, used) ==
              c.at("expected").get<std::string>());
    }
}

TEST_CASE("台词打包：逐组和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("group").size() >= 6);
    for (const auto& c : corpus.at("group")) {
        const std::string name = c.at("name");
        CAPTURE(name);
        const models::Shot s = shot_of(c.at("shot"));
        const auto got = stages::group_lines(s, stages::max_line_seconds(24));
        const auto want =
            c.at("expected").get<std::vector<std::vector<std::string>>>();

        REQUIRE(got.size() == want.size());
        for (std::size_t g = 0; g < want.size(); ++g) {
            REQUIRE(got[g].size() == want[g].size());
            for (std::size_t i = 0; i < want[g].size(); ++i) {
                CHECK(got[g][i].text == want[g][i]);
            }
        }
    }
}

TEST_CASE("一句就超预算时也要单独成组，不能丢") {
    // 丢掉的话那句话在成片里根本不存在，而分镜表上还写着。
    models::Shot s;
    s.shot_id = "sh001";
    models::DialogueLine huge;
    huge.text = "超长的一句";
    huge.actual_duration_s = 99.0;
    huge.char_id = "c_lin";
    models::DialogueLine small;
    small.text = "短的";
    small.actual_duration_s = 1.0;
    small.char_id = "c_lin";
    s.dialogue = {huge, small};

    const auto groups = stages::group_lines(s, stages::max_line_seconds(24));
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].size() == 1);
    CHECK(groups[0][0].text == "超长的一句");
    CHECK(groups[1][0].text == "短的");
}

TEST_CASE("拆镜头：逐个案例和 Python 对上") {
    const json corpus = load_corpus();
    REQUIRE(corpus.at("split_shots").size() >= 3);

    for (const auto& c : corpus.at("split_shots")) {
        const std::string name = c.at("name");
        CAPTURE(name);

        std::vector<models::Shot> shots;
        for (const auto& j : c.at("shots")) shots.push_back(shot_of(j));

        const auto got =
            stages::split_overlong_shots(shots, stages::max_line_seconds(24));
        const auto& want = c.at("expected");

        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            CAPTURE(i);
            CHECK(got[i].shot_id == want[i].at("shot_id").get<std::string>());
            CHECK(got[i].order == want[i].at("order").get<int>());
            CHECK(got[i].attempts == want[i].at("attempts").get<int>());
            CHECK(got[i].dialogue.size() == want[i].at("dialogue").size());
        }
    }
}

TEST_CASE("拆出来的新镜不继承产物") {
    // 继承的话，新镜带着原镜的 frame_path 和 video_path，
    // 流水线看到"已经有产物"就跳过它——成片里那一段是重复的画面。
    models::Shot s;
    s.shot_id = "sh001";
    s.order = 0;
    s.frame_path = "frames/sh001.png";
    s.video_path = "shots/draft/sh001.mp4";
    s.attempts = 2;
    s.duration_locked = true;
    s.status = models::ShotStatus::DRAFT_DONE;
    s.gate_notes = {"上一轮的记录"};
    for (int i = 0; i < 3; ++i) {
        models::DialogueLine l;
        l.text = "第" + std::to_string(i) + "句";
        l.actual_duration_s = 3.0;
        l.char_id = "c_lin";
        s.dialogue.push_back(l);
    }

    const auto out =
        stages::split_overlong_shots({s}, stages::max_line_seconds(24));
    REQUIRE(out.size() >= 2);

    // 原镜保留自己的产物
    CHECK(out[0].shot_id == "sh001");
    CHECK(out[0].frame_path.has_value());

    // 新镜一概清空
    for (std::size_t i = 1; i < out.size(); ++i) {
        CAPTURE(out[i].shot_id);
        CHECK_FALSE(out[i].frame_path.has_value());
        CHECK_FALSE(out[i].video_path.has_value());
        CHECK(out[i].attempts == 0);
        CHECK_FALSE(out[i].duration_locked);
        CHECK(out[i].status == models::ShotStatus::PLANNED);
        CHECK(out[i].gate_notes.empty());
    }

    SUBCASE("order 拆完统一重排") {
        // 不重排的话新镜和原镜同号，排序不稳定时成片里两镜的先后是随机的。
        for (std::size_t i = 0; i < out.size(); ++i) {
            CHECK(out[i].order == static_cast<int>(i));
        }
    }
}

TEST_CASE("重跑时新镜编号不撞老的") {
    models::Shot s;
    s.shot_id = "sh001";
    s.order = 0;
    for (int i = 0; i < 3; ++i) {
        models::DialogueLine l;
        l.text = "第" + std::to_string(i) + "句";
        l.actual_duration_s = 3.0;
        l.char_id = "c_lin";
        s.dialogue.push_back(l);
    }
    models::Shot old_split;
    old_split.shot_id = "sh001_b";   // 上一轮拆出来的
    old_split.order = 1;

    const auto out = stages::split_overlong_shots(
        {s, old_split}, stages::max_line_seconds(24));

    std::set<std::string> ids;
    for (const auto& o : out) {
        CAPTURE(o.shot_id);
        CHECK(ids.insert(o.shot_id).second);   // 没有重名
    }
}

TEST_CASE("概览只列前五个紧的镜头") {
    // 四十镜里有二十个紧的时候，列全了没人会读。
    std::vector<stages::ShotAudioPlan> plans;
    for (int i = 0; i < 8; ++i) {
        stages::ShotAudioPlan p;
        p.shot_id = "sh00" + std::to_string(i + 1);
        p.speech_duration_s = 4.8;
        p.locked_duration_s = 5.0;
        p.slack_s = 0.2;   // 紧
        p.lines = 2;
        plans.push_back(p);
    }
    const std::string s = stages::summarize(plans);
    CAPTURE(s);
    CHECK(s.find("配音完成 16 句") != std::string::npos);
    CHECK(s.find("8 个镜头留白不足半秒") != std::string::npos);
    CHECK(s.find("sh005") != std::string::npos);
    CHECK(s.find("sh006") == std::string::npos);   // 第六个之后不列

    SUBCASE("没有台词的镜头不算进紧的名单") {
        // 没台词的镜头留白当然充足，报进去是纯噪音。
        std::vector<stages::ShotAudioPlan> silent;
        stages::ShotAudioPlan p;
        p.shot_id = "sh001";
        p.slack_s = 0.0;
        p.lines = 0;
        silent.push_back(p);
        CHECK(stages::summarize(silent).find("留白不足") == std::string::npos);
    }

    SUBCASE("一个都没有时也有话说") {
        CHECK(stages::summarize({}) == "没有需要配音的镜头");
    }
}
