// 体检报告会不会把人支到不需要的地方去。
//
// **报告说错了比不说更糟。** 它的全部意义就是告诉人"还差什么"，
// 底下那句「有 N 项必须先解决才能出片」会被当成待办照做。
//
// 2026-09-08 抓到两处，都是把外部依赖当成无条件必需：
//
//   一，`[models].engine = "sd"` 时报告仍然说「出图和出视频都要用
//       ComfyUI」并判 FAIL。而那条路走进程内的 sd.cpp，`run_deps.cpp`
//       里 `if (engine != "comfy") return b;` 在碰任何 ComfyUI 工作流
//       之前就返回了——**等于把人支去装一个 34 GB 的依赖，而他根本
//       不需要**。
//   二，`tts.backend = "local"` 掉进了 ComfyUI 那条路，报"无法判断"。
//       而那时候两份 GGUF 就在盘上，判得出来。
//
// 判断本身拆进了 `doctor/needs.hpp`：`doctor.cpp` 链 httplib，
// 测试目标不收它（CMakeLists 里写着这条），**放在那里面的逻辑没法测**。
// 拆出来之后这几条用例才有地方落。

#include <doctest/doctest.h>

#include "config/settings.hpp"
#include "doctor/needs.hpp"

using namespace changji;

TEST_CASE("engine = sd 时 ComfyUI 不是硬依赖") {
    config::Settings s;
    s.models.engine = "sd";
    s.tts.backend = "local";

    const auto need = doctor::comfy_need(s);
    CHECK_FALSE(need.for_video);
    CHECK_FALSE(need.for_tts);
    CHECK_FALSE(need.required());
    CHECK(need.who() == "当前配置用不到它");
}

TEST_CASE("engine = comfy 时就是硬依赖") {
    // 反方向也要钉：上一条不能是靠"永远返回不需要"过的。
    config::Settings s;
    s.models.engine = "comfy";
    s.tts.backend = "local";

    const auto need = doctor::comfy_need(s);
    CHECK(need.for_video);
    CHECK(need.required());
    CHECK(need.who() == "出图和出视频要用它");
}

TEST_CASE("只有配音用 ComfyUI 时，话要指到配音上") {
    // 说成"出图和出视频"会让人往错的方向查——他会去翻出图的配置，
    // 而问题在配音那一段。
    config::Settings s;
    s.models.engine = "sd";
    s.tts.backend = "comfy";

    const auto need = doctor::comfy_need(s);
    CHECK_FALSE(need.for_video);
    CHECK(need.for_tts);
    CHECK(need.required());
    CHECK(need.who() == "配音要用它");
}

TEST_CASE("两边都用 ComfyUI 时一句话说全") {
    config::Settings s;
    s.models.engine = "comfy";
    s.tts.backend = "comfy";

    const auto need = doctor::comfy_need(s);
    CHECK(need.required());
    CHECK(need.who() == "出图出片和配音都要用它");
}

TEST_CASE("认得出配音走的是进程内那条路") {
    config::Settings s;
    s.tts.backend = "local";
    CHECK(doctor::tts_is_local(s));

    for (const char* other : {"comfy", "http", "estimate"}) {
        s.tts.backend = other;
        CAPTURE(other);
        CHECK_FALSE(doctor::tts_is_local(s));
    }
}
