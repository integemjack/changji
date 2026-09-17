// 正在干的那些短活。
//
// 这一层的用处全在"不可见"上：2026-09-11 一次批量写作四章全挂在
// 「显存不够加载 LLM：「图像」正用着」，而顶栏 `jobs: []`、GPU 0%——
// 挡路的那张参考图整个没登记。所以这里钉的是**登记和划掉这对操作**：
// 登记漏了看不见活，划掉漏了留下一条永远在跑的幽灵记录，而那条记录会让
// 顶栏永远亮着，比不显示更糟。

#include <doctest/doctest.h>

#include <string>
#include <thread>
#include <vector>

#include "pipeline/activity.hpp"
#include "pipeline/task_board.hpp"

using changji::pipeline::Activity;
using changji::pipeline::running_activities;
using changji::pipeline::running_work;
namespace pipeline = changji::pipeline;

TEST_CASE("没活的时候是空数组，不是 null") {
    // 前端直接 v-for 它。null 的话整块渲染不出来，而且不报错。
    CHECK(running_activities().is_array());
    CHECK(running_activities().empty());
}

TEST_CASE("构造登记，析构划掉") {
    {
        Activity a{"image", "/p/one", "", "正在画参考图"};
        const auto rows = running_activities();
        REQUIRE(rows.size() == 1);
        CHECK(rows[0]["kind"] == "image");
        CHECK(rows[0]["project"] == "/p/one");
        CHECK(rows[0]["message"] == "正在画参考图");
    }
    // 出了作用域就得干净。**漏一条的代价是顶栏永远亮着**，而用户会以为
    // 引擎卡住了。
    CHECK(running_activities().empty());
}

TEST_CASE("抛异常也划得掉") {
    // 出图失败抛的是 ApiError，登记那条要是靠"跑到函数末尾"来清，
    // 一次失败就留一条幽灵。
    try {
        Activity a{"image", "/p", "", "画着"};
        REQUIRE(running_activities().size() == 1);
        throw std::runtime_error("出图失败");
    } catch (const std::exception&) {
    }
    CHECK(running_activities().empty());
}

TEST_CASE("行的形状和长跑任务那边一样") {
    Activity a{"revise", "/p", "ch01", "正在改这一段"};
    const auto row = running_activities()[0];
    // 前端一套模板画两边，少一个键就得在模板里到处判空
    for (const char* k :
         {"kind", "project", "episode_id", "stage", "current", "total",
          "message", "queued"}) {
        CAPTURE(k);
        CHECK(row.contains(k));
    }
    CHECK(row["episode_id"] == "ch01");
    CHECK(row["stage"] == "");
}

TEST_CASE("排队那句盖在原话上，清掉就露出原话") {
    Activity a{"image", "/p", "", "正在画参考图"};
    CHECK(running_activities()[0]["queued"] == false);

    a.set_note("排队中，前面还有 2 件");
    auto row = running_activities()[0];
    CHECK(row["queued"] == true);
    // 两句都要有：在排队，而且排的是哪件事
    CHECK(row["message"] == "排队中，前面还有 2 件：正在画参考图");

    a.set_note("");
    row = running_activities()[0];
    CHECK(row["queued"] == false);
    CHECK(row["message"] == "正在画参考图");
}

TEST_CASE("note_queued：写到当前线程那件活上") {
    Activity a{"image", "/p", "", "正在画参考图"};
    CHECK(changji::pipeline::current_activity() != nullptr);

    changji::pipeline::note_queued(2, "");
    CHECK(running_activities()[0]["message"] ==
          "排队中，前面还有 2 件：正在画参考图");

    // 轮到自己了但显存腾不动：点名挡路的那个槽
    changji::pipeline::note_queued(0, "LLM");
    CHECK(running_activities()[0]["message"] ==
          "排队中，等「LLM」用完：正在画参考图");

    // -1 = 不排了
    changji::pipeline::note_queued(-1, "");
    CHECK(running_activities()[0]["message"] == "正在画参考图");
}

TEST_CASE("没活在干的时候 note_queued 什么也不干，不崩") {
    // 调度器是从底下调上来的，而底下不知道上头有没有登记过活——
    // 命令行跑流水线时就一个都没有。
    CHECK(changji::pipeline::current_activity() == nullptr);
    changji::pipeline::note_queued(3, "图像");
    CHECK(running_activities().empty());
}

TEST_CASE("进度改得动，话也改得动") {
    Activity a{"image", "/p", "", "画着"};
    a.set_progress(3, 8);
    a.set_message("快好了");
    const auto row = running_activities()[0];
    CHECK(row["current"] == 3);
    CHECK(row["total"] == 8);
    CHECK(row["message"] == "快好了");
}

TEST_CASE("几件同时干，按开工顺序排") {
    // 顶栏那个列表两秒重画一次。顺序每次都跳的话，看着像有活在闪。
    Activity a{"outline", "/p", "", "一"};
    Activity b{"image", "/p", "", "二"};
    Activity c{"say", "/p", "", "三"};
    const auto rows = running_activities();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0]["message"] == "一");
    CHECK(rows[1]["message"] == "二");
    CHECK(rows[2]["message"] == "三");
}

TEST_CASE("中间那件干完了，剩下的顺序不变") {
    Activity a{"outline", "/p", "", "一"};
    {
        Activity b{"image", "/p", "", "二"};
        CHECK(running_activities().size() == 2);
    }
    Activity c{"say", "/p", "", "三"};
    const auto rows = running_activities();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0]["message"] == "一");
    CHECK(rows[1]["message"] == "三");
}

TEST_CASE("多个线程同时登记划掉，不崩不漏") {
    // 出图、写作、朗读各在各的请求线程上，真会同时开工。
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) {
        ts.emplace_back([i] {
            for (int n = 0; n < 50; ++n) {
                Activity a{"image", "/p" + std::to_string(i), "", "干着"};
                a.set_progress(n, 50);
                (void)running_activities();
            }
        });
    }
    for (auto& t : ts) t.join();
    CHECK(running_activities().empty());
}

TEST_CASE("全部在干的活：没长跑任务时就等于短活那份") {
    Activity a{"image", "/p", "", "画着"};
    const auto all = running_work();
    REQUIRE(all.size() == 1);
    CHECK(all[0]["kind"] == "image");
}

TEST_CASE("画的是哪一格参考图：target 跟着那一行走") {
    // 没有 WebSocket 的时候，设定页只能从 `/api/system` 那份表里认出
    // "这一格在画"（`refs` 那条固定频道是纯 socket 的）。少了这一栏，
    // 那几格从头到尾一动不动，而且不报错。
    Activity a{"image", "/p/one", "", "正在画参考图"};
    {
        const auto rows = running_activities();
        REQUIRE(rows.size() == 1);
        // 没说画哪一格之前是空串，不是 null——前端直接读它
        CHECK(rows[0]["target"] == "");
    }
    a.set_target("linyuan_front");
    a.set_progress(7, 20);
    const auto rows = running_activities();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["target"] == "linyuan_front");
    CHECK(rows[0]["current"] == 7);
    CHECK(rows[0]["total"] == 20);
}

TEST_CASE("长跑任务那几行也要有 target 这一栏") {
    // 顶栏和设定页都是一套代码画两边的列表，少一个键就得到处判空。
    // 同 `queued` 那一栏的理由。
    const auto all = running_work();
    CHECK(all.is_array());
    Activity a{"image", "/p/two", "", "正在画参考图"};
    for (const auto& row : running_work()) {
        CHECK(row.contains("target"));
    }
}

// ---------------------------------------------------------------------------
// 任务账本：排队中 → 正在做 → 做完的
// ---------------------------------------------------------------------------

TEST_CASE("任务账本：三个状态各归各位，耗时按自己那一段算") {
    // 用户 2026-09-17：「任务页面显示正在做的（已经用时…）、排队中的
    // （…取消图标按钮）、已经做完的（耗时）」。
    //
    // **耗时这一条钉得最紧**：出图那一批第一版是整批收尾时一起结账的，
    // 六张图的耗时于是一模一样（实测全是 96.9 秒），而真实是三十几秒到
    // 七十几秒不等。结账必须是"这一件干完那一下"。
    const std::string proj = "/tmp/任务账本用例";

    {
        pipeline::Task queued{"image", "画参考图 · 甲 正面", proj};
        pipeline::Task running{"tts", "配音 · sh001", proj};
        running.begin();

        const auto b = pipeline::task_board(proj);
        REQUIRE(b.at("queued").size() == 1);
        REQUIRE(b.at("running").size() == 1);
        CHECK(b.at("queued")[0].at("title") == "画参考图 · 甲 正面");
        CHECK(b.at("running")[0].at("title") == "配音 · sh001");
        // 排着的那一行要说得出"排了多久"——等得久的时候人想知道它是不是
        // 被忘了。
        CHECK(b.at("queued")[0].contains("waited"));
        // 正在做的不报"排了多久"：它已经不排了。
        CHECK_FALSE(b.at("running")[0].contains("waited"));
    }

    // 两件都出了作用域 = 都结账了。没 begin 过的那件记成"取消了"。
    const auto after = pipeline::task_board(proj);
    CHECK(after.at("queued").empty());
    CHECK(after.at("running").empty());
    REQUIRE(after.at("done").size() == 2);
    // **倒着报：最后结账那件在最上面。** 这两件是按声明的反序析构的
    //（`running` 声明在后、先析构），所以最后结账的是那件排着没开工的。
    CHECK(after.at("done")[0].at("title") == "画参考图 · 甲 正面");
    CHECK(after.at("done")[0].at("state") == "cancelled");
    CHECK(after.at("done")[1].at("title") == "配音 · sh001");
    CHECK(after.at("done")[1].at("state") == "done");
}

TEST_CASE("任务账本：人按的停不是失败") {
    const std::string proj = "/tmp/任务账本用例2";
    {
        pipeline::Task t{"image", "画参考图 · 乙 背面", proj};
        t.begin();
        CHECK_FALSE(t.cancelled());
        CHECK(pipeline::cancel_task(t.id()));
        CHECK(t.cancelled());
        // 底下那一层会把取消抛成一句「已停下这一张」，照样落进 error。
        t.fail("已停下这一张");
    }
    const auto b = pipeline::task_board(proj);
    REQUIRE(b.at("done").size() == 1);
    // **有 error 也算"停下了"，不是"失败"。** 报成红的话人会去找哪儿出错
    // 了，而什么都没出错——和 util/cancel_words.hpp 那条是同一件事。
    CHECK(b.at("done")[0].at("state") == "cancelled");
}

TEST_CASE("任务账本：上一级停了，每一件跟着停") {
    // 「停一件」和「停一整批」要能同时成立：任务页面上每一行有自己的叉，
    // 而整批那头还有一个停。没有 link 的话两者只能选一个。
    pipeline::CancelToken batch;
    pipeline::Task a{"image", "第一张", "/tmp/任务账本用例3"};
    pipeline::Task b{"image", "第二张", "/tmp/任务账本用例3"};
    a.token().link(&batch);
    b.token().link(&batch);

    CHECK(pipeline::cancel_task(a.id()));
    CHECK(a.cancelled());
    CHECK_FALSE(b.cancelled());   // 停一件不碰别的

    batch.request();
    CHECK(b.cancelled());         // 停整批，剩下的跟着停
}

TEST_CASE("任务账本：思考接起来存，取的时候是整份") {
    const std::string proj = "/tmp/任务账本用例4";
    std::uint64_t id = 0;
    {
        pipeline::Task t{"llm", "写第三章", proj};
        t.begin();
        id = t.id();
        t.append_thinking("先想");
        t.append_thinking("再想");
        // 账本那份**只报有没有**，不报正文——一次写作的思考几千字，而这份
        // 东西两秒推一次。正文点开才单独取。
        const auto b = pipeline::task_board(proj);
        REQUIRE(b.at("running").size() == 1);
        CHECK(b.at("running")[0].at("thinking") == true);
        CHECK(pipeline::task_thinking(id) == "先想再想");
    }
    // 结完账也还找得到：页面上「做完的」那几行也能点开看。
    CHECK(pipeline::task_thinking(id) == "先想再想");
}
