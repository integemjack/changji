// 后台那件活的"当前 stream"和"取消令牌"。
//
// **这两样都是线程局部的**，而它们错了的表现都不报错：
//   stream 串了  → 思考推到上一件活的频道上，界面上张冠李戴；
//   令牌漏登记   → 界面上那个「停下」按下去没反应，而接口回的是 200。
// 所以单独钉一遍。

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <thread>

#include "http/job_stream.hpp"

using namespace changji;

TEST_CASE("JobScope：当前 stream 挂上、出去就摘掉") {
    CHECK(http::current_stream().empty());
    {
        const http::JobScope a{"job-1"};
        CHECK(http::current_stream() == "job-1");
        {
            // 嵌套：里层出去之后要还原成外层那个，不是清空。
            // 一件活里再起一件（批量那条）就是这个形状。
            const http::JobScope b{"job-2"};
            CHECK(http::current_stream() == "job-2");
        }
        CHECK(http::current_stream() == "job-1");
    }
    CHECK(http::current_stream().empty());
}

TEST_CASE("JobScope：抛出去也要摘干净") {
    // 处理函数里抛异常的路径有好几条。漏一条的后果是下一件活把思考推到
    // 上一件的频道上，而那不报错。
    try {
        const http::JobScope s{"job-throw"};
        CHECK(http::current_stream() == "job-throw");
        throw std::runtime_error("砸了");
    } catch (const std::exception&) {
    }
    CHECK(http::current_stream().empty());
}

TEST_CASE("按 stream 把活停掉") {
    SUBCASE("在跑的那件停得掉") {
        const http::JobScope s{"job-a"};
        CHECK_FALSE(http::current_cancel().cancelled());
        CHECK(http::cancel_job("job-a"));
        CHECK(http::current_cancel().cancelled());
    }

    SUBCASE("找不到不是错——按下去那一刻可能刚好干完") {
        CHECK_FALSE(http::cancel_job("根本没这条"));
        CHECK_FALSE(http::cancel_job(""));
    }

    SUBCASE("活干完了就停不到了，也不崩") {
        // 令牌活在那条线程的栈上。表里那一条和 JobScope 成对加减，
        // 所以这儿不会是个悬空指针。
        { const http::JobScope s{"job-b"}; }
        CHECK_FALSE(http::cancel_job("job-b"));
    }

    SUBCASE("同一个 id 被重用：停的是现在这件") {
        // 用户连点两下、前一件还没退干净时就是这个形状。析构时按地址比过
        // 再删，所以后来那件的登记不会被前一件抹掉。
        const http::JobScope outer{"job-same"};
        {
            const http::JobScope inner{"job-same"};
            CHECK(http::cancel_job("job-same"));
            CHECK(http::current_cancel().cancelled());
        }
        // 里层退了，外层那条还登记着，而且是另一个令牌（没被停过）
        CHECK_FALSE(http::current_cancel().cancelled());
        CHECK(http::cancel_job("job-same"));
        CHECK(http::current_cancel().cancelled());
    }
}

TEST_CASE("没有 JobScope 时给个哑元，别让调用方自己 new") {
    // 同步那条路和单测直接调都落在这儿。要点是**不能崩**，
    // 也不能把别人那件活停掉。
    CHECK(http::current_stream().empty());
    auto& t = http::current_cancel();
    t.reset();
    CHECK_FALSE(t.cancelled());
}

TEST_CASE("两条线程各记各的，不串") {
    // 同时写两集是常态。串了的话 A 的思考会推到 B 的频道上。
    std::atomic<bool> ok_a{false};
    std::atomic<bool> ok_b{false};
    std::thread a([&] {
        const http::JobScope s{"t-a"};
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ok_a = http::current_stream() == "t-a";
    });
    std::thread b([&] {
        const http::JobScope s{"t-b"};
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ok_b = http::current_stream() == "t-b";
    });
    a.join();
    b.join();
    CHECK(ok_a);
    CHECK(ok_b);
    CHECK(http::current_stream().empty());
}
