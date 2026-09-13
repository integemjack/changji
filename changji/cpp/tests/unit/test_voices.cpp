// GET /api/voices 的测试。
//
// **这个文件 2026-09-10 大幅缩水。** 原来它测的是"在 ComfyUI 工作流里
// 找音色下拉框"——靠输入名认不靠节点类型认那一套。ComfyUI 拆掉之后
// 那段逻辑不存在了，剩下的接口只有一件事：**把"音色怎么配"说清楚**，
// 而且**要按后端说对**。说错了比不说更糟：让用 local 的人去查一个
// 他根本没在用的外部服务。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "http/voices.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

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

TEST_CASE("项目里存了参考音色时，这个接口就是那份清单") {
    // **进程内配音没有服务端的音色列表**，所以"有哪些音色"等于"这个项目
    // 存了哪几段人声"。2026-09-13 之前这个接口只回一句说明，角色页那一栏
    // 是个空文本框，用户得手打路径——打错的表现只是配音失败。
    const fs::path root =
        fs::temp_directory_path() / paths::from_utf8("changji_音色");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "voices", ec);

    const auto touch = [&](const char* name) {
        std::ofstream(root / "voices" / paths::from_utf8(name), std::ios::binary)
            << "假的音频";
    };
    touch("c_lin_wan.wav");
    touch("c_chen_mo.mp3");
    touch("笔记.txt");   // 不是音频，不该进清单

    const auto r = http::get_voices(paths::to_utf8(root), "local");
    CHECK(r.status == 200);
    const auto list = r.body["voices"].get<std::vector<std::string>>();
    REQUIRE(list.size() == 2);
    // **排过序**：目录遍历的顺序是文件系统给的，不排的话同一个项目在不同
    // 机器上这一栏顺序不一样，看着像数据变了。
    CHECK(list[0] == "voices/c_chen_mo.mp3");
    CHECK(list[1] == "voices/c_lin_wan.wav");
    // 有清单就没有那句"还没有参考音色"
    CHECK_FALSE(r.body.contains("error"));
    // 界面靠 kind 决定这一栏画成什么，不该靠 backend 猜
    CHECK(r.body["kind"] == "clip");

    SUBCASE("外部服务那条不看目录：它的音色是名字不是文件") {
        const auto h = http::get_voices(paths::to_utf8(root), "http");
        CHECK(h.body["voices"].empty());
        CHECK(h.body["kind"] == "name");
    }

    fs::remove_all(root, ec);
}
