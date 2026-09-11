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

using changji::pipeline::Activity;
using changji::pipeline::running_activities;
using changji::pipeline::running_work;

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
          "message"}) {
        CAPTURE(k);
        CHECK(row.contains(k));
    }
    CHECK(row["episode_id"] == "ch01");
    CHECK(row["stage"] == "");
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
