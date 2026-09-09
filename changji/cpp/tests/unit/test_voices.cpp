// GET /api/voices 的测试。
//
// **这个文件 2026-09-10 大幅缩水。** 原来它测的是"在 ComfyUI 工作流里
// 找音色下拉框"——靠输入名认不靠节点类型认那一套。ComfyUI 拆掉之后
// 那段逻辑不存在了，剩下的接口只有一件事：**把"音色怎么配"说清楚**，
// 而且**要按后端说对**。说错了比不说更糟：让用 local 的人去查一个
// 他根本没在用的外部服务。
#include <doctest/doctest.h>

#include <string>

#include "http/voices.hpp"

using namespace changji;

TEST_CASE("音色接口：两条后端各说各的") {
    SUBCASE("进程内配音：指向参考音频") {
        const auto r = http::get_voices("/tmp/proj", "local");
        CHECK(r.status == 200);
        CHECK(r.body["voices"].empty());
        const std::string err = r.body["error"];
        CHECK(err.find("参考音频") != std::string::npos);
        // **别把人指去查外部服务**——他没在用
        CHECK(err.find("base_url") == std::string::npos);
    }
    SUBCASE("外部服务：指向那个服务自己的音色名") {
        const auto r = http::get_voices("/tmp/proj", "http");
        CHECK(r.status == 200);
        CHECK(r.body["voices"].empty());
        const std::string err = r.body["error"];
        CHECK(err.find("voice_id") != std::string::npos);
    }
    SUBCASE("**任何情况都回 200**") {
        // 角色页打开时顺带拉这个接口。回 500 的话整页弹错误框，
        // 而用户可能根本没打算配音。
        for (const char* b : {"local", "http", "认不出的"}) {
            CHECK(http::get_voices("/tmp/proj", b).status == 200);
        }
    }
    SUBCASE("没给项目路径还是要拦") {
        CHECK_THROWS_AS(http::get_voices("", "local"), http::ApiError);
    }
}
