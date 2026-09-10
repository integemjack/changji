// 任务表的测试。
//
// 这里测两类东西，两类都容易出错但出错方式完全不同：
//
// 一是**快照形状**，跟 Python 的 RunState/WriteState.snapshot() 对拍。
//    出错的话前端读到 undefined，表现是进度条不动或者报错框空白。
//
// 二是**并发**。这类 bug 单跑一遍测不出来——要靠重复和多线程去撞。
//    下面几个用例刻意跑几百上千轮，就是为了让竞态有机会暴露。
//    如果哪天 CI 上偶发失败，那不是"抖动"，是真有竞态。

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pipeline/jobs.hpp"

using namespace changji::pipeline;
using json = nlohmann::json;

namespace {

/// 一个立刻结束的任务体。
JobTable::Body noop() {
    return [](JobProgress&) {};
}

/// 等到任务跑完。超时就失败——挂住比断言失败更难查。
///
/// 只能用来等**自然结束**的任务。取消的任务不行：cancel() 里 running
/// 立刻变 false，这个函数会在工作线程还活着的时候就返回。
/// 等取消要用 t.wait_idle()。
bool wait_done(JobTable& t, JobKind k, int timeout_ms = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (t.running(k)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

TEST_CASE("Run 快照的字段和 Python 一致") {
    JobTable t;
    const json s = t.snapshot(JobKind::Run);

    // 字段名逐个对，少一个前端就读到 undefined
    for (const char* k : {"running", "episode_id", "stage", "current", "total",
                          "message", "elapsed_s", "output", "outputs",
                          "queue_done", "queue_total", "error", "events"}) {
        CAPTURE(k);
        CHECK_MESSAGE(s.contains(k), "快照缺字段");
    }
    CHECK(s.size() == 13);  // 也不能多，多出来的字段说明混进了 Write 的

    // 空闲时的初值
    CHECK(s.at("running") == false);
    CHECK(s.at("episode_id").is_null());
    CHECK(s.at("error").is_null());
    CHECK(s.at("output").is_null());
    CHECK(s.at("outputs").is_array());
    CHECK(s.at("events").is_array());
    CHECK(s.at("queue_total") == 1);  // 只跑一集时是 1/1，不是 0/0
    CHECK(s.at("elapsed_s") == 0.0);
}

TEST_CASE("Write 快照只有六个字段") {
    // Write 的形状比 Run 小得多。早先版本把两种任务塞进同一个结构，
    // 顺手把 Run 的字段全吐出去了——前端拿 events 去渲染，
    // 结果写作任务的进度条画出来是空的。
    JobTable t;
    const json s = t.snapshot(JobKind::Write);
    CHECK(s.size() == 6);
    for (const char* k : {"running", "done", "total", "message", "episodes", "error"}) {
        CAPTURE(k);
        CHECK(s.contains(k));
    }
    CHECK_FALSE(s.contains("events"));
    CHECK_FALSE(s.contains("queue_total"));
    CHECK(s.at("episodes").is_array());
}

TEST_CASE("同种任务不能并发，不同种可以") {
    JobTable t;
    std::atomic<bool> release{false};
    auto blocker = [&release](JobProgress&) {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };

    CHECK(t.start(JobKind::Run, "ep_01", blocker) == true);
    // 第二个同种的要被拒，调用方据此回 409
    CHECK(t.start(JobKind::Run, "ep_02", noop()) == false);

    // 写作跟跑流水线是两个槽，Python 那边的注释明写着可以同时进行
    CHECK(t.start(JobKind::Write, "", blocker) == true);
    CHECK(t.start(JobKind::Write, "", noop()) == false);

    CHECK(t.running(JobKind::Run));
    CHECK(t.running(JobKind::Write));
    // 被拒的那次不能污染已有状态
    CHECK(t.snapshot(JobKind::Run).at("episode_id") == "ep_01");

    release = true;
    REQUIRE(wait_done(t, JobKind::Run));
    REQUIRE(wait_done(t, JobKind::Write));

    // 跑完之后同一个槽要能再起
    CHECK(t.start(JobKind::Run, "ep_03", noop()) == true);
    REQUIRE(wait_done(t, JobKind::Run));
}

TEST_CASE("取消") {
    JobTable t;
    std::atomic<int> loops{0};
    CHECK(t.cancel(JobKind::Run) == false);  // 没在跑，对应 {"stopped": false}

    t.start(JobKind::Run, "ep_01",
            [&loops](JobProgress& p) {
                while (!p.cancelled()) {
                    ++loops;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });

    // 等它真的转起来再取消，不然测的是"启动前就取消"这个另一回事
    while (loops.load() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(t.cancel(JobKind::Run) == true);

    // running 必须**立刻**是 false，不等线程退出。
    // Python 那边 stop_run() 就是 task.cancel() 紧跟 running = False，
    // 前端点完停止马上拉 /api/run，看到的得是"停了"。
    CHECK(t.running(JobKind::Run) == false);
    CHECK(t.snapshot(JobKind::Run).at("error") == kRunStoppedMessage);

    t.wait_idle();

    SUBCASE("取消一个不影响另一个") {
        std::atomic<bool> release{false};
        std::atomic<bool> write_saw_cancel{false};
        t.start(JobKind::Write, "",
                [&](JobProgress& p) {
                    while (!release.load()) {
                        if (p.cancelled()) write_saw_cancel = true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                });
        t.start(JobKind::Run, "ep_01",
                [](JobProgress& p) {
                    while (!p.cancelled()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        t.cancel(JobKind::Run);
        CHECK(write_saw_cancel.load() == false);
        release = true;
        t.wait_idle();
    }
}

TEST_CASE("异常被兜住，不会崩掉整个服务") {
    // 流水线里任何一步抛异常都不该带走进程。Python 那边是 try/except
    // 包着整个 run()，这里靠工作线程里的 catch。
    JobTable t;
    t.start(JobKind::Run, "ep_01",
            [](JobProgress&) { throw std::runtime_error("模型文件读不了"); });
    REQUIRE(wait_done(t, JobKind::Run));

    const json s = t.snapshot(JobKind::Run);
    CHECK(s.at("running") == false);
    CHECK(s.at("error") == "模型文件读不了");

    SUBCASE("非标准异常也兜住") {
        t.start(JobKind::Run, "ep_02",
                [](JobProgress&) { throw 42; });
        REQUIRE(wait_done(t, JobKind::Run));
        CHECK(t.snapshot(JobKind::Run).at("error") == "未知异常");
    }
}

TEST_CASE("事件环上限 500，快照只回最后 80 条") {
    JobTable t;
    t.start(JobKind::Run, "ep_01",
            [](JobProgress& p) {
                for (int i = 0; i < 600; ++i) {
                    Event e;
                    e.stage = "render";
                    e.kind = "progress";
                    e.message = "第 " + std::to_string(i) + " 条";
                    e.current = i;
                    e.total = 600;
                    p.report(e);
                }
            });
    REQUIRE(wait_done(t, JobKind::Run));

    const json s = t.snapshot(JobKind::Run);
    const json& ev = s.at("events");
    CHECK(ev.size() == kSnapshotEvents);
    // 留下的必须是**最后** 80 条：600 条里存了后 500 条（100..599），
    // 再取后 80 条就是 520..599
    CHECK(ev.front().at("current") == 520);
    CHECK(ev.back().at("current") == 599);
    CHECK(ev.back().at("message") == "第 599 条");

    // 事件字段形状
    for (const char* k : {"at", "stage", "kind", "message", "shot_id",
                          "current", "total"}) {
        CAPTURE(k);
        CHECK(ev.back().contains(k));
    }
    CHECK(ev.back().at("shot_id").is_null());  // 没给就是 null，不是空串
    CHECK(ev.back().at("at").get<double>() > 0.0);  // 时间戳由 record 补

    // 进度跟着最后一条走
    CHECK(s.at("current") == 599);
    CHECK(s.at("total") == 600);
    CHECK(s.at("stage") == "render");
}

TEST_CASE("没有总数的事件不清空进度条") {
    // 一条纯日志事件（total=0）夹在进度里，不该把已有的 current/total 抹掉。
    // 抹掉的话前端进度条会一跳一跳地闪回零。
    JobTable t;
    t.start(JobKind::Run, "ep_01",
            [](JobProgress& p) {
                Event e;
                e.stage = "render"; e.kind = "progress";
                e.current = 3; e.total = 10; e.message = "画第 3 个镜头";
                p.report(e);

                Event log;
                log.stage = "render"; log.kind = "warn";
                log.message = "显存吃紧，转成分块解码";
                p.report(log);  // total 保持 0
            });
    REQUIRE(wait_done(t, JobKind::Run));

    const json s = t.snapshot(JobKind::Run);
    CHECK(s.at("current") == 3);
    CHECK(s.at("total") == 10);
    CHECK(s.at("message") == "显存吃紧，转成分块解码");  // 消息还是要更新
    CHECK(s.at("events").size() == 2);
}

TEST_CASE("消息汇收到的内容") {
    JobTable t;
    std::mutex mu;
    std::vector<std::pair<std::string, json>> got;
    t.set_sink([&](const std::string& id, const json& m) {
        std::lock_guard lg(mu);
        got.emplace_back(id, m);
    });

    t.start(JobKind::Run, "ep_01",
            [](JobProgress& p) {
                Event e;
                e.stage = "render"; e.kind = "progress";
                e.current = 1; e.total = 2; e.message = "开画";
                e.shot_id = "s_001";
                p.report(e);
            });
    REQUIRE(wait_done(t, JobKind::Run));

    // 取一份拷贝再断言。直接在锁里断言的话，SUBCASE 展开时外层的
    // lock_guard 还活着，子用例里再锁同一把就是递归加锁——
    // 非递归 mutex 在 MSVC 上抛 "resource deadlock would occur"。
    auto drain = [&mu, &got] {
        std::lock_guard lg(mu);
        auto copy = got;
        got.clear();
        return copy;
    };

    const auto msgs = drain();
    REQUIRE(msgs.size() == 2);  // 一条进度 + 一条完成

    CHECK(msgs[0].second.at("type") == "progress");
    CHECK(msgs[0].second.at("stage") == "render");
    CHECK(msgs[0].second.at("step") == 1);     // 注意字段名是 step 不是 current
    CHECK(msgs[0].second.at("total") == 2);
    CHECK(msgs[0].second.at("shot_id") == "s_001");

    // 完成消息**必须**发出去。被节流吞掉的话前端永远停在"跑着"。
    CHECK(msgs[1].second.at("type") == "done");
    CHECK(msgs[0].first == msgs[1].first);  // 同一个 job_id
    CHECK(msgs[1].second.at("job_id") == msgs[1].first);

    SUBCASE("异常时发 error") {
        t.start(JobKind::Run, "ep_02",
                [](JobProgress&) { throw std::runtime_error("炸了"); });
        REQUIRE(wait_done(t, JobKind::Run));
        const auto m = drain();
        REQUIRE(m.size() == 1);
        CHECK(m[0].second.at("type") == "error");
        CHECK(m[0].second.at("message") == "炸了");
    }

    SUBCASE("取消时也发 error") {
        t.start(JobKind::Run, "ep_03",
                [](JobProgress& p) {
                    while (!p.cancelled()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        t.cancel(JobKind::Run);
        t.wait_idle();
        const auto m = drain();
        REQUIRE(m.size() == 1);
        CHECK(m[0].second.at("type") == "error");
        // 不是"已取消"——cancel() 已经把 error 写成了给用户看的那句话，
        // 工作线程结束时不能覆盖掉
        CHECK(m[0].second.at("message") == kRunStoppedMessage);
    }
}

TEST_CASE("反复起停不会漏 join") {
    // 上一轮的 std::thread 没 join 就被赋值，std::thread 的赋值运算符
    // 会调 terminate。跑一遍看不出来，要连着起停几百次。
    JobTable t;
    for (int i = 0; i < 300; ++i) {
        CAPTURE(i);
        REQUIRE(t.start(JobKind::Run, "ep_" + std::to_string(i), noop()));
        REQUIRE(wait_done(t, JobKind::Run));
    }
    CHECK(t.snapshot(JobKind::Run).at("episode_id") == "ep_299");
}

TEST_CASE("多线程同时抢同一个槽，只能有一个成功") {
    // start() 里为了 join 上一轮线程要临时解锁，那个窗口曾经能让
    // 两个 start() 一起通过 running 检查。这个用例专门撞它。
    for (int round = 0; round < 50; ++round) {
        CAPTURE(round);
        JobTable t;
        std::atomic<bool> release{false};
        std::atomic<int> wins{0};
        std::vector<std::thread> racers;

        for (int i = 0; i < 8; ++i) {
            racers.emplace_back([&] {
                const bool ok = t.start(
                    JobKind::Run, "ep",
                    [&release](JobProgress&) {
                        while (!release.load()) {
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                        }
                    });
                if (ok) ++wins;
            });
        }
        for (auto& th : racers) th.join();
        CHECK(wins.load() == 1);

        release = true;
        t.wait_idle();
    }
}

TEST_CASE("一边跑一边查快照不会撕裂") {
    // 工作线程在写 state，HTTP 线程在读快照。没锁好的话读到的是半新半旧，
    // 或者直接读到已经被 pop_front 的 deque 元素。
    JobTable t;
    std::atomic<bool> stop_polling{false};
    std::atomic<int> polls{0};

    std::thread poller([&] {
        while (!stop_polling.load()) {
            const json s = t.snapshot(JobKind::Run);
            // 光是能取出来不算数，字段得完整。
            //
            // **这里必须是 CHECK 不是 REQUIRE。** REQUIRE 失败要靠抛异常
            // 中止用例，而这是工作线程——异常出不去，整轮跑会被就地截断。
            // 实测过：把这条改成必失败的 REQUIRE，输出是
            //
            //     test cases: 215 | 214 passed | 1 failed | 277 skipped
            //
            // 492 条里只跑了 215 条，剩下 277 条根本没执行，而汇总行写的是
            // "1 failed"——看见这行的人会去修那一条然后重跑，
            // **不会知道另外 277 条压根没跑过**。
            // 换成 CHECK 之后同样的失败是：492 全跑完，1 条挂，179 个断言挂。
            CHECK(s.contains("events"));
            CHECK(s.at("events").is_array());
            if (!s.contains("events") || !s.at("events").is_array()) continue;
            for (const auto& e : s.at("events")) {
                CHECK(e.contains("current"));
            }
            ++polls;
        }
    });

    t.start(JobKind::Run, "ep_01",
            [](JobProgress& p) {
                for (int i = 0; i < 3000; ++i) {
                    Event e;
                    e.stage = "render"; e.kind = "progress";
                    e.current = i; e.total = 3000;
                    e.message = "第 " + std::to_string(i);
                    p.report(e);
                }
            });
    REQUIRE(wait_done(t, JobKind::Run, 30000));
    stop_polling = true;
    poller.join();
    CHECK(polls.load() > 0);
}

TEST_CASE("wait_idle 等得住两个槽") {
    JobTable t;
    std::atomic<int> finished{0};
    auto slow = [&finished](JobProgress&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ++finished;
    };
    t.start(JobKind::Run, "ep_01", slow);
    t.start(JobKind::Write, "", slow);
    t.wait_idle();
    CHECK(finished.load() == 2);
    CHECK_FALSE(t.running(JobKind::Run));
    CHECK_FALSE(t.running(JobKind::Write));
}

TEST_CASE("析构时先取消再等，不会挂住") {
    // 关服务时如果有个跑到一半的任务，析构不能干等——那可能是几十分钟。
    std::atomic<bool> saw_cancel{false};
    const auto t0 = std::chrono::steady_clock::now();
    {
        JobTable t;
        t.start(JobKind::Run, "ep_01",
                [&saw_cancel](JobProgress& p) {
                    while (!p.cancelled()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    saw_cancel = true;
                });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }  // 析构在这里
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(saw_cancel.load());
    CHECK(ms < 2000);  // 真挂住的话这里会超时
}

TEST_CASE("两种任务的停止文案不一样") {
    // 这两句是前端直接显示给用户的，各自说的是各自的事。
    // 合并成一句的话，有一半场合用户看到的提示是错的。
    JobTable t;
    std::atomic<bool> release{false};
    auto blocker = [&release](JobProgress& p) {
        while (!release.load() && !p.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    t.start(JobKind::Run, "ep_01", blocker);
    t.start(JobKind::Write, "", blocker);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    CHECK(t.cancel(JobKind::Run) == true);
    CHECK(t.cancel(JobKind::Write) == true);
    CHECK(t.snapshot(JobKind::Run).at("error") ==
          "已手动停止。已完成的镜头会保留，下次从这里继续。");
    CHECK(t.snapshot(JobKind::Write).at("error") ==
          "已手动停止。已经写好的几集留着。");

    release = true;
    t.wait_idle();
}

TEST_CASE("job_id 每次都不一样") {
    // WebSocket 按 job_id 订阅。撞了的话新任务的进度会推给订阅旧任务的连接。
    JobTable t;
    std::set<std::string> ids;
    for (int i = 0; i < 100; ++i) {
        REQUIRE(t.start(JobKind::Run, "ep", noop()));
        ids.insert(t.job_id(JobKind::Run));
        REQUIRE(wait_done(t, JobKind::Run));
    }
    CHECK(ids.size() == 100);
    // 前缀区分任务种类，看日志时有用
    CHECK(ids.begin()->rfind("run-", 0) == 0);

    REQUIRE(t.start(JobKind::Write, "", noop()));
    CHECK(t.job_id(JobKind::Write).rfind("write-", 0) == 0);
    REQUIRE(wait_done(t, JobKind::Write));
}

// ── 写整季那条路上的几个 setter ─────────────────────────────────────
//
// coverage_audit.py 查出来的：set_done / add_episode / add_output /
// set_episode_id / set_error 一个都没被碰过。上面那些用例把快照形状、
// 取消、事件环、并发都盖住了，唯独**累积**这件事没人验。
//
// 累积错了的表现都是"界面上少了东西"，不报错：
//   - add_episode 要是覆盖而不是追加，写整季只看得到最后一集
//   - set_error 要是终止任务，写砸一集会让前面几集白写
//     （jobs.hpp 那行注释写的就是这条）

TEST_CASE("写整季：每集追加一条，不是覆盖") {
    JobTable t;
    CHECK(t.start(JobKind::Write, "", [](JobProgress& p) {
        p.set_total(3);
        p.add_episode(json{{"episode_id", "ep01"}, {"title", "第一集"}});
        p.set_done(1);
        p.add_episode(json{{"episode_id", "ep02"}, {"title", "第二集"}});
        p.set_done(2);
    }));
    REQUIRE(wait_done(t, JobKind::Write));

    const json s = t.snapshot(JobKind::Write);
    REQUIRE(s.at("episodes").is_array());
    CHECK(s.at("episodes").size() == 2);
    CHECK(s.at("episodes")[0].at("episode_id") == "ep01");
    CHECK(s.at("episodes")[1].at("episode_id") == "ep02");
    CHECK(s.at("done") == 2);
    CHECK(s.at("total") == 3);
}

TEST_CASE("set_error 不终止任务，后面几集照跑") {
    // jobs.hpp：「记一条错误。**不终止任务**——写整季时一集写砸了
    // 不该让前面几集白写。」跑一晚上，早上发现第二集挂了导致后面十集
    // 都没动，那一晚上就白熬了。
    JobTable t;
    std::atomic<int> reached{0};
    CHECK(t.start(JobKind::Write, "", [&reached](JobProgress& p) {
        p.add_episode(json{{"episode_id", "ep01"}});
        p.set_error("ep02：大模型没返回镜头列表");
        reached = 1;
        // 出错之后还能继续往里写。
        p.add_episode(json{{"episode_id", "ep03"}});
        reached = 2;
    }));
    REQUIRE(wait_done(t, JobKind::Write));

    CHECK(reached.load() == 2);
    const json s = t.snapshot(JobKind::Write);
    CHECK(s.at("episodes").size() == 2);
    const std::string err = s.at("error").get<std::string>();
    CHECK(err.find("ep02") != std::string::npos);
}

TEST_CASE("跑流水线：产物是追加的，当前集号会更新") {
    JobTable t;
    CHECK(t.start(JobKind::Run, "ep01", [](JobProgress& p) {
        p.set_episode_id("ep01");
        p.add_output("output/ep01.mp4");
        p.set_episode_id("ep02");
        p.add_output("output/ep02.mp4");
    }));
    REQUIRE(wait_done(t, JobKind::Run));

    const json s = t.snapshot(JobKind::Run);
    // 集号是"当前跑到哪一集"，覆盖是对的。
    CHECK(s.at("episode_id") == "ep02");
    // 产物是整批的清单，必须都在——批量跑完之后界面靠它列出成片。
    REQUIRE(s.at("outputs").is_array());
    CHECK(s.at("outputs").size() == 2);
    CHECK(s.at("outputs")[0] == "output/ep01.mp4");
    CHECK(s.at("outputs")[1] == "output/ep02.mp4");
}

TEST_CASE("没跑过的槽，快照里的累积字段是空的不是缺的") {
    // 前端拿 .length 去渲染。缺字段的话是 undefined，取 .length 直接抛，
    // 整页白屏——而"还没开始跑"是最常见的状态。
    JobTable t;
    const json run = t.snapshot(JobKind::Run);
    REQUIRE(run.at("outputs").is_array());
    CHECK(run.at("outputs").empty());

    const json write = t.snapshot(JobKind::Write);
    REQUIRE(write.at("episodes").is_array());
    CHECK(write.at("episodes").empty());
}

// ---------------------------------------------------------------------------
// 多卡之后几镜同时报进度，快照里的 current 不能来回蹦。
// ---------------------------------------------------------------------------

TEST_CASE("同一阶段里 current 只进不退，换阶段从头来") {
    JobTable table;
    table.start(JobKind::Run, "ep01", [&](JobProgress& p) {
        auto at = [&](const char* stage, int cur, int total) {
            Event e;
            e.stage = stage;
            e.kind = "progress";
            e.current = cur;
            e.total = total;
            p.report(e);
        };
        // 六路并发时先到的未必序号小
        at("draft", 3, 12);
        at("draft", 11, 12);
        at("draft", 7, 12);     // 不能把 11 拉回 7
        CHECK(table.snapshot(JobKind::Run)["current"] == 11);
        at("draft", 12, 12);
        CHECK(table.snapshot(JobKind::Run)["current"] == 12);
        // 进了下一阶段，1/5 就是 1/5
        at("final", 1, 5);
        CHECK(table.snapshot(JobKind::Run)["current"] == 1);
        CHECK(table.snapshot(JobKind::Run)["total"] == 5);
    });
    table.wait_idle();
}

TEST_CASE("WebSocket 消息原样带 kind") {
    // 界面靠它分清 shot_done 和 progress。type 只有两种取值，分不出来。
    JobTable table;
    std::vector<nlohmann::json> msgs;
    table.set_sink([&](const std::string&, const nlohmann::json& m) {
        msgs.push_back(m);
    });
    table.start(JobKind::Run, "ep01", [&](JobProgress& p) {
        Event e;
        e.stage = "draft";
        e.kind = "shot_done";
        e.shot_id = "sh1";
        e.current = 1;
        e.total = 3;
        p.report(e);
    });
    table.wait_idle();
    // 任务函数返回之后表还会再广播一条 {"type":"done"}，所以不能拿
    // 最后一条——按 shot_id 找那条我们发的。
    const auto it = std::find_if(msgs.begin(), msgs.end(), [](const auto& m) {
        return m.value("shot_id", "") == "sh1";
    });
    REQUIRE(it != msgs.end());
    CHECK(it->value("kind", "") == "shot_done");
    CHECK(it->value("type", "") == "progress");   // 旧字段没动
}

TEST_CASE("这一镜自己的步数只走 WebSocket，不进 /api/run 的事件") {
    // **两个进度，别混。**
    //   current/total = 整集的位置（第 21 镜 / 共 22 镜）
    //   shot_step/shot_steps = 这一镜自己（第 4 步 / 共 6 步）
    //
    // 镜头墙上每张牌画的是后者。以前 WebSocket 里只有前者，牌子上那条
    // 进度条于是从这一镜开始就是 95%、跑完还是 95%——不动而且是错的，
    // 比没有更糟。
    //
    // **同时它不能进 to_json()**：那个是 /api/run 用的，逐字节和 Python
    // 对拍，而 Python 的事件只有 at/stage/kind/message/shot_id/current/total。
    // WebSocket 那条 Python 侧根本没有，所以只在那边加安全。
    JobTable table;
    std::vector<nlohmann::json> msgs;
    table.set_sink([&](const std::string&, const nlohmann::json& m) {
        msgs.push_back(m);
    });
    table.start(JobKind::Run, "ep01", [&](JobProgress& p) {
        Event e;
        e.stage = "final";
        e.kind = "progress";
        e.shot_id = "sh1";
        e.current = 21;
        e.total = 22;
        e.shot_step = 4;
        e.shot_steps = 6;
        p.report(e);

        Event bare;   // 不带单镜步数的（"准备中"那几条）
        bare.stage = "final";
        bare.kind = "progress";
        bare.shot_id = "sh2";
        bare.current = 21;
        bare.total = 22;
        p.report(bare);
    });
    table.wait_idle();

    const auto find = [&](const char* id) {
        return std::find_if(msgs.begin(), msgs.end(), [id](const auto& m) {
            return m.value("shot_id", "") == id;
        });
    };

    const auto one = find("sh1");
    REQUIRE(one != msgs.end());
    CHECK(one->value("shot_step", -1) == 4);
    CHECK(one->value("shot_steps", -1) == 6);
    // 整集那一对没被顶掉
    CHECK(one->value("step", -1) == 21);
    CHECK(one->value("total", -1) == 22);

    // **没给就不加。** 补个 0 会让每张牌上都挂一条永远空着的槽，
    // 而界面正是靠"有没有这两个字段"决定画进度还是画走马灯。
    const auto two = find("sh2");
    REQUIRE(two != msgs.end());
    CHECK_FALSE(two->contains("shot_step"));
    CHECK_FALSE(two->contains("shot_steps"));

    // /api/run 那份一个字都不能多
    Event e;
    e.stage = "final";
    e.kind = "progress";
    e.shot_id = "sh1";
    e.current = 21;
    e.total = 22;
    e.shot_step = 4;
    e.shot_steps = 6;
    const auto rest = e.to_json();
    CHECK_FALSE(rest.contains("shot_step"));
    CHECK_FALSE(rest.contains("shot_steps"));
    CHECK(rest.size() == 7);   // at/stage/kind/message/shot_id/current/total
}
