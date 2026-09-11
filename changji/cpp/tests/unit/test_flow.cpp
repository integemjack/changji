// 引导流程那七步的判定。
//
// **这里最容易出的错不是算错，是对不上号。** 侧边栏按 `key` 认人：
// `flow_steps()` 给出格子，`flow_assess()` 给出每格打不打勾。两边的 key
// 少一个对不上，那一格就永远是灰的——不报错，用户也不知道为什么走不下去。
// 2026-09-10 把分镜和制作合成一步时正是这个风险：改了列表忘了改判定。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <set>
#include <string>

#include "http/flow.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

json a_project() {
    return json{{"project_id", "p1"},
                {"episodes", json::array({json{{"episode_id", "ep01"},
                                               {"script", "有内容"}}})}};
}

json a_shot(const char* id, const char* status) {
    return json{{"shot_id", id}, {"status", status}, {"duration_s", 3.0}};
}

std::set<std::string> keys_of(const json& steps) {
    std::set<std::string> out;
    for (const auto& s : steps) out.insert(s.at("key").get<std::string>());
    return out;
}

}  // namespace

TEST_CASE("每一格都得有对应的判定，一个都不能少") {
    // 这就是那条真正要守的不变式。少一个 key 的表现是"某一格永远打不上勾"，
    // 而不是任何一种报错。
    const auto steps = keys_of(http::flow_steps());
    const json done =
        http::flow_assess(a_project(), json::array({a_shot("a", "planned")}),
                          json::array(), "ep01")
            .at("done");

    for (const auto& key : steps) {
        CAPTURE(key);
        CHECK_MESSAGE(done.contains(key), "这一格没人给它打勾");
    }
    // 反过来也查一遍：判定里多出来的 key 是死代码，
    // 说明列表那边删过一步而判定这边忘了跟。
    for (const auto& [key, _] : done.items()) {
        CAPTURE(key);
        CHECK_MESSAGE(steps.count(key) == 1, "判定里多出来一个没有格子的 key");
    }
}

TEST_CASE("镜头、成片、上传合成了一步「这一集」") {
    // 几页合成一页之后，侧边栏留几格指向同一个地方只会让人以为点错了。
    const auto steps = keys_of(http::flow_steps());
    CHECK(steps.count("episode") == 1);
    CHECK(steps.count("storyboard") == 0);
    CHECK(steps.count("production") == 0);
    CHECK(steps.count("shots") == 0);
    CHECK(steps.count("film") == 0);
    CHECK(steps.count("publish") == 0);
    CHECK(steps.count("script") == 0);  // 剧本大纲那一格也没有了
    CHECK(steps.count("characters") == 0);  // 角色和场景并进「设定」了
    CHECK(steps.count("scenes") == 0);
    CHECK(steps.count("assets") == 1);
    CHECK(http::flow_steps().size() == 4);
}

TEST_CASE("「故事」这一格") {
    const auto steps = keys_of(http::flow_steps());
    CHECK(steps.count("story") == 1);

    // 老项目没有 story.json，不给 story 也得判得出来，而且别的格子不受影响
    const auto none = http::flow_assess(a_project(), json::array(),
                                        json::array(), "ep01")
                          .at("done");
    CHECK_FALSE(none.at("story").get<bool>());
    CHECK(none.at("project").get<bool>());

    // 判据不看梗概：梗概是写故事的输入，光有梗概什么都还没发生
    const json only_premise = json{{"premise", "深夜便利店"},
                                   {"chapters", json::array()}};
    CHECK_FALSE(http::flow_assess(a_project(), json::array(), json::array(),
                                  "ep01", only_premise)
                    .at("done")
                    .at("story")
                    .get<bool>());

    const json with_chapters = json{
        {"chapters", json::array({json{{"chapter_id", "ch01"}}})},
        {"plan", json::array({json{{"episode_id", "ep01"}},
                              json{{"episode_id", "ep02"}}})}};
    const auto got = http::flow_assess(a_project(), json::array(),
                                       json::array(), "ep01", with_chapters);
    CHECK(got.at("done").at("story").get<bool>());
    CHECK(got.at("counters").at("chapters").get<int>() == 1);
    CHECK(got.at("counters").at("plannedEpisodes").get<int>() == 2);
}

TEST_CASE("「这一集」这一格：判据是装配出片子了") {
    const auto all_done = json::array(
        {a_shot("a", "final_done"), a_shot("b", "locked")});

    // 镜头全出完了，但还没装配——这一格不算完
    CHECK_FALSE(http::flow_assess(a_project(), all_done, json::array(), "ep01")
                    .at("done")
                    .at("episode")
                    .get<bool>());

    // 产物里有这一集的片子才算
    const auto outputs = json::array({json{{"name", "ep01.mp4"}}});
    const auto got = http::flow_assess(a_project(), all_done, outputs, "ep01");
    CHECK(got.at("done").at("episode").get<bool>());

    // 镜头出没出完另外报一个数，进度条和「还差几镜」用它
    CHECK(got.at("counters").at("shotsDone").get<bool>());
    CHECK_FALSE(http::flow_assess(a_project(),
                                  json::array({a_shot("a", "planned")}),
                                  outputs, "ep01")
                    .at("counters")
                    .at("shotsDone")
                    .get<bool>());
}

TEST_CASE("shotsDone：光有分镜表不算出完") {
    // 这个数原来是「镜头」那一格的判定，三格合一之后降级成一个计数，
    // 但那条规矩照旧：光有分镜表就算出完的话，界面上说这一集拍完了，
    // 而实际上一帧画面都还没出。
    const auto only_planned = json::array({a_shot("a", "planned")});
    CHECK_FALSE(http::flow_assess(a_project(), only_planned, json::array(),
                                  "ep01")
                    .at("counters")
                    .at("shotsDone")
                    .get<bool>());

    const auto all_done = json::array(
        {a_shot("a", "final_done"), a_shot("b", "locked")});
    CHECK(http::flow_assess(a_project(), all_done, json::array(), "ep01")
              .at("counters")
              .at("shotsDone")
              .get<bool>());

    // 一个镜头都没有时 0 == 0 会让"全都做完了"意外成立，单独挡一下
    CHECK_FALSE(http::flow_assess(a_project(), json::array(), json::array(),
                                  "ep01")
                    .at("counters")
                    .at("shotsDone")
                    .get<bool>());
}
