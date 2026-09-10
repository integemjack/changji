// WebSocket 广播中心（服务端那一侧）的测试。
//
// **这一块原来一条测试都没有。** `test_ws_client.cpp` 测的是我们自己写的
// 客户端（连 ComfyUI 用的），和这里是两码事。
//
// 拿假指针测是安全的：Hub 里那个 `connection*` **纯粹当键用，从不解引用**
// （add / remove / subscribe / unsubscribe 全是集合操作）。唯一会解引用的
// 是 `broadcast`，所以这里一次都不调它。
//
// Python 侧没有 WebSocket——server.py 的开头写着"不用 WebSocket，
// 因为轮询在这个场景够用"。所以这条路**没法对拍**，只能自己测。

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>

#include "http/ws.hpp"

using namespace changji;

namespace {

/// 假连接。只用它的地址，不碰内容。
crow::websocket::connection* fake(std::uintptr_t n) {
    return reinterpret_cast<crow::websocket::connection*>(n);
}

}  // namespace

TEST_CASE("一个 job 可以被多个连接订阅，一个连接可以订阅多个 job") {
    // 头文件里那条"从 ComfyUI 学来的教训"就是这个：ComfyUI 按 clientId 记
    // 订阅，同一个 id 上并发跑两个任务，后连的会把先连的挤下线。
    // 这里是多对多，所以不会。
    ws::Hub h;
    auto* a = fake(0x1000);
    auto* b = fake(0x2000);
    h.add(a);
    h.add(b);
    CHECK(h.connection_count() == 2);

    h.subscribe(a, "job1");
    h.subscribe(b, "job1");
    h.subscribe(a, "job2");
    CHECK(h.subscriber_count("job1") == 2);
    CHECK(h.subscriber_count("job2") == 1);
}

TEST_CASE("断开连接要把它的订阅一起清掉") {
    // 留着的话 subs_ 里会存着一个已经没了的指针，下一次 broadcast
    // 就往野指针上发——那是崩溃，不是"消息发丢了"。
    ws::Hub h;
    auto* a = fake(0x1000);
    auto* b = fake(0x2000);
    h.add(a);
    h.add(b);
    h.subscribe(a, "job1");
    h.subscribe(b, "job1");

    h.remove(a);
    CHECK(h.connection_count() == 1);
    CHECK(h.subscriber_count("job1") == 1);

    h.remove(b);
    CHECK(h.connection_count() == 0);
    // 最后一个订阅者走了，这个键本身也要没——否则跑一天下来
    // subs_ 里全是空集合。
    CHECK(h.subscriber_count("job1") == 0);
}

TEST_CASE("没登记过的连接订阅不了") {
    // 断开之后再来的订阅要丢掉，否则 subs_ 里会留着一个已经没了的指针。
    ws::Hub h;
    h.subscribe(fake(0x9999), "job1");
    CHECK(h.subscriber_count("job1") == 0);
}

TEST_CASE("客户端发来的畸形消息一律丢掉，不崩也不影响已有订阅") {
    // **客户端发什么都可能。** 这里解析失败是静默丢弃：不回错误也不断开，
    // 畸形消息不该影响一个正在跑的任务。
    ws::Hub h;
    auto* a = fake(0x1000);
    h.add(a);
    h.subscribe(a, "job1");

    for (const char* junk : {
             "", "{", "not json", "[]", "null", "123", "\"str\"",
             R"({"type":"subscribe"})",                    // 少 job_id
             R"({"type":"subscribe","job_id":""})",        // 空 job_id
             R"({"type":"explode","job_id":"job1"})",      // 不认识的 type
             R"({"job_id":"job1"})",                       // 少 type
             R"({"type":123,"job_id":456})",               // 类型不对
         }) {
        CAPTURE(junk);
        h.handle_client_message(a, junk);
    }
    // 一条都不该产生新订阅，原来那条也不该丢。
    CHECK(h.subscriber_count("job1") == 1);
    CHECK(h.subscriber_count("") == 0);
    CHECK(h.connection_count() == 1);
}

TEST_CASE("subscribe / unsubscribe 走得通，重复操作是幂等的") {
    ws::Hub h;
    auto* a = fake(0x1000);
    h.add(a);

    h.handle_client_message(a, R"({"type":"subscribe","job_id":"job1"})");
    CHECK(h.subscriber_count("job1") == 1);
    // 重订阅不该变成两个——集合去重，前端重连后无脑重订也不会出问题。
    h.handle_client_message(a, R"({"type":"subscribe","job_id":"job1"})");
    CHECK(h.subscriber_count("job1") == 1);

    h.handle_client_message(a, R"({"type":"unsubscribe","job_id":"job1"})");
    CHECK(h.subscriber_count("job1") == 0);
    // 退订一个没订过的，安静地什么都不做。
    h.handle_client_message(a, R"({"type":"unsubscribe","job_id":"job1"})");
    CHECK(h.subscriber_count("job1") == 0);
}

// ── 进度节流 ───────────────────────────────────────────────────────
//
// 逐步回调每秒可能触发几十次，直接透传会把前端淹掉，所以 progress 要节流。
// **但 done / error 一条都不能漏**：漏一条，前端就一直显示「生成中」
// 直到用户手动刷新——比进度条不流畅严重得多。
//
// 这条规则原来没有任何测试，因为 should_throttle 是私有的，而唯一的调用方
// broadcast 要解引用 connection* 才能发消息，拿假指针会崩。为此把它挪到了
// public（见 ws.hpp 里的说明）。

TEST_CASE("progress 在间隔内只放第一条过") {
    ws::Hub h;
    // 第一条永远放过：一个任务刚开始就被节流掉的话，前端在第一个间隔里
    // 什么都看不到，看起来像没启动。
    CHECK_FALSE(h.should_throttle("job1", "progress", "progress"));
    CHECK(h.should_throttle("job1", "progress", "progress"));
    CHECK(h.should_throttle("job1", "progress", "progress"));
}

TEST_CASE("done 和 error 永远不节流") {
    ws::Hub h;
    // 先把这个 job 的节流窗口点着。
    CHECK_FALSE(h.should_throttle("job1", "progress", "progress"));
    CHECK(h.should_throttle("job1", "progress", "progress"));

    // 紧接着的 done / error 必须原样放过。
    CHECK_FALSE(h.should_throttle("job1", "done", "done"));
    CHECK_FALSE(h.should_throttle("job1", "error", "error"));
    CHECK_FALSE(h.should_throttle("job1", "start", "start"));
    // 连着来几条也一样，它们不进节流的账。
    CHECK_FALSE(h.should_throttle("job1", "done", "done"));
}

TEST_CASE("预览和进度各占各的节流桶，别互相饿死") {
    // **这条是被一个真 bug 逼出来的。** 采样进度（kind=progress）和预览图
    // （kind=preview）都用 type=progress 广播。共用一个桶的话，sd.cpp 每步
    // 先调预览、后调进度，预览抢先占了这 200ms 的槽，紧接着的真进度被丢——
    // 表现是预览在动、步数却冻在开跑那一下（实测 6 秒 6 条预览 0 条进度）。
    ws::Hub h;
    // 预览点着自己的桶
    CHECK_FALSE(h.should_throttle("run", "progress", "preview"));
    CHECK(h.should_throttle("run", "progress", "preview"));
    // 进度那一桶还是空的，第一条必须放过
    CHECK_FALSE(h.should_throttle("run", "progress", "progress"));
    CHECK(h.should_throttle("run", "progress", "progress"));
}

TEST_CASE("两个任务各算各的") {
    // 按 job_id 记，不是全局一个窗口——否则同时跑两集时，
    // 后一集的进度会被前一集压住。
    ws::Hub h;
    CHECK_FALSE(h.should_throttle("job1", "progress", "progress"));
    CHECK_FALSE(h.should_throttle("job2", "progress", "progress"));
    CHECK(h.should_throttle("job1", "progress", "progress"));
    CHECK(h.should_throttle("job2", "progress", "progress"));
}

TEST_CASE("过了间隔又放行") {
    ws::Hub h;
    CHECK_FALSE(h.should_throttle("job1", "progress", "progress"));
    CHECK(h.should_throttle("job1", "progress", "progress"));
    // kThrottleInterval 是 200 毫秒，多睡一点避开时钟粒度。
    std::this_thread::sleep_for(ws::kThrottleInterval + std::chrono::milliseconds(80));
    CHECK_FALSE(h.should_throttle("job1", "progress", "progress"));
}
