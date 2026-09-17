// 首帧和出片之间那根线。
//
// 错了不报错：出片那层早了一步就是拿没有首帧的镜头去出片（出来是纯文生
// 视频，混在成片里最难发现）；晚了一步就是首帧全出完才动——正是要改掉的
// 「大量 GPU 空闲」。所以这一段要能反复撞。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "pipeline/shot_flow.hpp"

using namespace changji::pipeline;

TEST_CASE("不在等的名单上：一问就放行") {
    ShotFlow flow({"sh2"});
    CancelToken tok;
    CHECK(flow.ready("sh1"));
    CHECK(flow.wait_ready("sh1", tok));
    CHECK_FALSE(flow.ready("sh2"));
}

TEST_CASE("在名单上：划了才放行，划之前一直等") {
    ShotFlow flow({"sh1"});
    CancelToken tok;
    std::atomic<bool> passed{false};
    std::thread t([&] {
        flow.wait_ready("sh1", tok);
        passed.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(passed.load());   // 没划，不该过
    flow.mark_ready("sh1");
    t.join();
    CHECK(passed.load());
    CHECK(flow.ready("sh1"));
}

TEST_CASE("首帧那层收工：没划的也全放行，别让出片那层吊着") {
    ShotFlow flow({"sh1", "sh2"});
    CancelToken tok;
    flow.close();
    CHECK(flow.wait_ready("sh1", tok));
    CHECK(flow.wait_ready("sh2", tok));
}

TEST_CASE("等的过程里按了停下：回 false，而且不用等到划") {
    ShotFlow flow({"sh1"});
    CancelToken tok;
    std::atomic<bool> got{true};
    std::thread t([&] { got.store(flow.wait_ready("sh1", tok)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    tok.request();
    t.join();
    CHECK_FALSE(got.load());
}
