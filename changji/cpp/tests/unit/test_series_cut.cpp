// 成片：把各章接成一条、按每集时长在镜头边界切。
//
// 这儿只测纯算法（plan_series_cut）：接得对不对、切点落在镜头边界上、
// 每一集从哪儿到哪儿。真拼接和切文件走 ffmpeg，那一层不在单测里跑。

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "media/assemble.hpp"
#include "pipeline/series_cut.hpp"

using namespace changji;

namespace {

media::Timeline chapter(const std::string& prefix, std::vector<double> durations) {
    media::Timeline t;
    double at = 0.0;
    int i = 0;
    for (double d : durations) {
        media::TimelineEntry e;
        e.shot_id = prefix + "_sh" + std::to_string(++i);
        e.start_s = at;
        e.duration_s = d;
        at += d;
        t.entries.push_back(e);
    }
    return t;
}

}  // namespace

TEST_CASE("切集：各章接成一条，集与集首尾相接，一镜不丢不重") {
    const std::vector<std::pair<std::string, media::Timeline>> chapters = {
        {"ch01", chapter("ep01", {3.0, 4.0, 5.0})},
        {"ch02", chapter("ep02", {6.0, 2.0})},
        {"ch03", chapter("ep03", {7.0})},
    };
    const auto parts = pipeline::plan_series_cut(chapters, 8.0);
    REQUIRE_FALSE(parts.empty());

    // 首尾相接，从 0 到总长 27
    CHECK(parts.front().start_s == doctest::Approx(0.0));
    for (std::size_t i = 1; i < parts.size(); ++i) {
        CHECK(parts[i].start_s == doctest::Approx(parts[i - 1].end_s));
    }
    CHECK(parts.back().end_s == doctest::Approx(27.0));

    // 每一镜恰好出现一次，顺序不变
    std::vector<std::string> seen;
    for (const auto& p : parts) {
        CHECK_FALSE(p.shot_ids.empty());
        for (const auto& id : p.shot_ids) seen.push_back(id);
    }
    CHECK(seen == std::vector<std::string>{"ep01_sh1", "ep01_sh2", "ep01_sh3", "ep02_sh1",
                                           "ep02_sh2", "ep03_sh1"});

    // 只在镜头边界切：每一集的长度是它那几镜的和
    // 3+4=7 (再加 5 就 12 > 8) | 5 | 6+2=8 | 7
    REQUIRE(parts.size() == 4);
    CHECK(parts[0].end_s - parts[0].start_s == doctest::Approx(7.0));
    CHECK(parts[1].end_s - parts[1].start_s == doctest::Approx(5.0));
    CHECK(parts[2].end_s - parts[2].start_s == doctest::Approx(8.0));
    CHECK(parts[3].end_s - parts[3].start_s == doctest::Approx(7.0));

    // 每一集从哪一章开始：给人看的
    CHECK(parts[0].first_chapter == "ch01");
    CHECK(parts[1].first_chapter == "ch01");
    CHECK(parts[2].first_chapter == "ch02");
    CHECK(parts[3].first_chapter == "ch03");
}

TEST_CASE("切集：整部一集就是一集") {
    const std::vector<std::pair<std::string, media::Timeline>> chapters = {
        {"ch01", chapter("ep01", {3.0, 4.0})},
        {"ch02", chapter("ep02", {6.0})},
    };
    for (double per : {0.0, -1.0}) {
        CAPTURE(per);
        const auto parts = pipeline::plan_series_cut(chapters, per);
        REQUIRE(parts.size() == 1);
        CHECK(parts[0].shot_ids.size() == 3);
        CHECK(parts[0].end_s == doctest::Approx(13.0));
        CHECK(parts[0].first_chapter == "ch01");
    }
}

TEST_CASE("切集：一镜比一集还长，它自己一集") {
    const std::vector<std::pair<std::string, media::Timeline>> chapters = {
        {"ch01", chapter("ep01", {2.0, 20.0, 2.0})},
    };
    const auto parts = pipeline::plan_series_cut(chapters, 5.0);
    REQUIRE(parts.size() == 3);
    CHECK(parts[1].shot_ids == std::vector<std::string>{"ep01_sh2"});
    CHECK(parts[1].end_s - parts[1].start_s == doctest::Approx(20.0));
}

TEST_CASE("切集：没有章就没有集") {
    CHECK(pipeline::plan_series_cut({}, 60.0).empty());
}
