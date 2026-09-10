// 项目新建、删除、改梗概的测试。
//
// 删项目那个是整套接口里**唯一不可逆**的操作，所以三道闸每一道都单独测，
// 而且每道都测"闸挡住了"和"闸放行了"两边——只测放行的话，闸失效了
// 测试照样绿，而那时候一次误点就能把跑了一夜的成片全删了。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/projects.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// 取规范形式再比路径。
///
/// macOS 上 /var 是 /private/var 的软链，而 ProjectPaths 构造时会
/// weakly_canonical——于是"传进去的路径"和"回来的路径"字面上不相等，
/// 尽管指的是同一个目录。两边都规范化，比的才是同一件事。
fs::path canon(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}

/// 一个临时的项目库。settings.workspace 指到这里。
struct Workspace {
    fs::path root;
    config::Settings settings;

    explicit Workspace(const std::string& tag) {
        root = fs::temp_directory_path() / paths::from_utf8("changji_库_" + tag);
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        settings.workspace = paths::to_utf8(root);
    }
    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
};

/// 在库里建一个真项目，返回它的目录。
fs::path make_project(const Workspace& ws, const std::string& name) {
    const fs::path dir = ws.root / paths::from_utf8(name);
    models::ProjectStore::create(dir, text::project_slug(name), name);
    return dir;
}

}  // namespace

TEST_CASE("新建项目：只填名字就落在项目库里") {
    // 让用户去猜容器里的绝对路径是没道理的，他也不知道项目库挂在哪。
    Workspace ws("新建");
    const auto r = http::guard([&] {
        return http::post_new_project(
            json{{"path", "雨夜天台"}, {"title", "雨夜天台"}}, ws.settings);
    });
    REQUIRE(r.status == 200);

    const fs::path made = paths::from_utf8(r.body.at("root").get<std::string>());
    CHECK(fs::is_directory(made));
    // **两边都取规范形式再比。** ProjectPaths 的构造函数会
    // weakly_canonical 一下（那是有意的，projects.cpp 的"在不在库里"
    // 靠它），而 macOS 上 /var 是 /private/var 的软链——
    // 临时目录拿到的是 /var/folders/...，规范化之后变成 /private/var/...，
    // 直接比字符串就永远不相等。Windows 和 Linux 上两者本来就一样。
    CHECK(canon(made.parent_path()) == canon(ws.root));
    CHECK(models::ProjectStore(made).exists());

    const models::Project p = models::ProjectStore(made).load_project();
    CHECK(p.title == "雨夜天台");
    // 纯中文目录名要走 sha1 退路，不能是空的 id
    CHECK_FALSE(p.project_id.empty());
    CHECK(p.project_id.rfind("p-", 0) == 0);
}

TEST_CASE("新建项目：带路径分隔符的当绝对/相对路径用") {
    Workspace ws("带路径");
    const fs::path elsewhere =
        fs::temp_directory_path() / paths::from_utf8("changji_别处");
    std::error_code ec;
    fs::remove_all(elsewhere, ec);

    const auto r = http::guard([&] {
        return http::post_new_project(
            json{{"path", paths::to_utf8(elsewhere)}}, ws.settings);
    });
    REQUIRE(r.status == 200);
    // 没被塞进项目库
    CHECK(canon(paths::from_utf8(r.body.at("root").get<std::string>())) ==
          canon(elsewhere));
    CHECK(models::ProjectStore(elsewhere).exists());
    fs::remove_all(elsewhere, ec);
}

TEST_CASE("新建项目的几种拒绝") {
    Workspace ws("拒绝");
    const auto try_new = [&](const json& body) {
        return http::guard([&] {
            return http::post_new_project(body, ws.settings);
        });
    };

    CHECK(try_new(json{{"path", ""}}).status == 400);
    CHECK(try_new(json{{"path", "   "}}).status == 400);
    CHECK(try_new(json{{"path", "x"}, {"style_line", "别的"}}).status == 400);
    CHECK(try_new(json{{"title", "缺 path"}}).status == 422);

    SUBCASE("同名的要 409 不是 400") {
        // 前端据此提示"换个名字"，400 会被当成参数错。
        CHECK(try_new(json{{"path", "重名"}}).status == 200);
        CHECK(try_new(json{{"path", "重名"}}).status == 409);
    }
}

TEST_CASE("删项目：三道闸") {
    Workspace ws("删除");
    const fs::path proj = make_project(ws, "要删的");

    const auto try_del = [&](const std::string& path,
                             const std::string& confirm) {
        return http::guard([&] {
            return http::post_delete_project(
                json{{"path", path}, {"confirm_name", confirm}}, ws.settings);
        });
    };

    SUBCASE("闸一：不在项目库里的不给删") {
        const fs::path outside =
            fs::temp_directory_path() / paths::from_utf8("changji_库外项目");
        std::error_code ec;
        fs::remove_all(outside, ec);
        models::ProjectStore::create(outside, "outside", "库外");

        const auto r = try_del(paths::to_utf8(outside), "changji_库外项目");
        CHECK(r.status == 403);
        // 一定还在
        CHECK(fs::is_directory(outside));
        fs::remove_all(outside, ec);
    }

    SUBCASE("闸一：项目库自己也不给删") {
        // 等于的话这个接口就能删掉整个项目库，
        // 那是"一次误点删掉所有项目"，比删错一个严重得多。
        const auto r = try_del(paths::to_utf8(ws.root), ws.root.filename().string());
        CHECK(r.status == 403);
        CHECK(fs::is_directory(ws.root));
    }

    SUBCASE("闸一：拿 .. 绕出去也不行") {
        // 字面上看着在库里面，实际指向外面。
        const std::string sneaky =
            paths::to_utf8(ws.root / paths::from_utf8("要删的") / "..") + "/..";
        const auto r = try_del(sneaky, "x");
        CHECK(r.status == 403);
        CHECK(fs::is_directory(ws.root));
    }

    SUBCASE("闸二：不是项目的目录不给删") {
        // 指到一个普通目录上的话，删掉的可能是用户放素材的地方。
        const fs::path plain = ws.root / paths::from_utf8("只是个目录");
        std::error_code ec;
        fs::create_directories(plain, ec);
        {
            std::ofstream out(plain / "重要资料.txt", std::ios::binary);
            out << "别删我";
        }
        const auto r = try_del(paths::to_utf8(plain), "只是个目录");
        CHECK(r.status == 404);
        CHECK(fs::is_regular_file(plain / "重要资料.txt"));
    }

    SUBCASE("闸三：名字对不上不给删") {
        const auto r = try_del(paths::to_utf8(proj), "要删的x");
        CHECK(r.status == 400);
        CHECK(r.body.at("detail").get<std::string>().find("要删的") !=
              std::string::npos);
        CHECK(fs::is_directory(proj));

        // 空的确认名也不行
        CHECK(try_del(paths::to_utf8(proj), "").status == 400);
        CHECK(fs::is_directory(proj));
    }

    SUBCASE("三道都过才真删") {
        const auto r = try_del(paths::to_utf8(proj), "要删的");
        REQUIRE(r.status == 200);
        CHECK_FALSE(fs::exists(proj));
        // 项目库本身还在
        CHECK(fs::is_directory(ws.root));
    }
}

TEST_CASE("删项目会连素材和成片一起删") {
    // 这一条是在确认"不可逆"的范围有多大——文档里写了，测试里也要有，
    // 否则将来有人改成"只删 project.json"，测试照样绿。
    Workspace ws("删干净");
    const fs::path proj = make_project(ws, "带素材的");
    std::error_code ec;
    fs::create_directories(proj / "output", ec);
    {
        std::ofstream out(proj / "output" / "成片.mp4", std::ios::binary);
        out << "假的";
    }

    const auto r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(proj)}, {"confirm_name", "带素材的"}},
            ws.settings);
    });
    REQUIRE(r.status == 200);
    CHECK_FALSE(fs::exists(proj / "output" / "成片.mp4"));
    CHECK_FALSE(fs::exists(proj));
}

TEST_CASE("正在跑的时候不让删项目") {
    // 跑到一半删项目，工作线程下一次写盘会写到一个不存在的目录上，
    // 报的错和"删项目"八竿子打不着。
    Workspace ws("跑着删");
    const fs::path proj = make_project(ws, "在跑的");

    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
    std::atomic<bool> release{false};
    pipeline::jobs().start(pipeline::JobKind::Run, "ep01",
                           [&release](pipeline::JobProgress& p) {
                               while (!release.load() && !p.cancelled()) {
                                   std::this_thread::sleep_for(
                                       std::chrono::milliseconds(1));
                               }
                           });

    const auto r = http::guard([&] {
        return http::post_delete_project(
            json{{"path", paths::to_utf8(proj)}, {"confirm_name", "在跑的"}},
            ws.settings);
    });
    CHECK(r.status == 409);
    CHECK(fs::is_directory(proj));

    release = true;
    pipeline::jobs().cancel(pipeline::JobKind::Run);
    pipeline::jobs().wait_idle();
}

TEST_CASE("改梗概") {
    Workspace ws("梗概");
    const fs::path proj = make_project(ws, "改梗概的");

    const auto r = http::guard([&] {
        return http::post_project_premise(
            json{{"project", paths::to_utf8(proj)},
                 {"premise", "  林晚在天台等一个七年没出现的人。  "}});
    });
    REQUIRE(r.status == 200);
    // 两端空白要去掉
    CHECK(r.body.at("premise") == "林晚在天台等一个七年没出现的人。");
    CHECK(models::ProjectStore(proj).load_project().premise ==
          "林晚在天台等一个七年没出现的人。");

    SUBCASE("超长的按字符截到 2000") {
        // 按字节截的话中文会被切出半个字，那半个字节进 JSON 时
        // nlohmann 会抛异常——存梗概这件事本身又炸一次。
        std::string long_premise;
        for (int i = 0; i < 3000; ++i) long_premise += "很";
        const auto r2 = http::guard([&] {
            return http::post_project_premise(
                json{{"project", paths::to_utf8(proj)},
                     {"premise", long_premise}});
        });
        REQUIRE(r2.status == 200);
        const std::string got = r2.body.at("premise").get<std::string>();
        CHECK(text::utf8_len(got) == 2000);
        CHECK(got.size() == 6000);   // 三字节一个，没切half
    }

    SUBCASE("项目不存在是 400") {
        const auto r2 = http::guard([&] {
            return http::post_project_premise(
                json{{"project", "Z:/没有这个目录"}, {"premise", "x"}});
        });
        CHECK(r2.status == 400);
    }
}

TEST_CASE("项目 id 的 slug 规则和角色的不一样") {
    // 分隔符是连字符不是下划线，退路前缀是 "p-" 且取 8 位。
    // 合并的话其中一边的 id 会悄悄变形，而 id 变形意味着老项目打不开。
    CHECK(text::project_slug("Rainy Night") == "rainy-night");
    CHECK(text::project_slug("雨夜天台").rfind("p-", 0) == 0);
    CHECK(text::project_slug("雨夜天台").size() == 10);   // "p-" + 8 位
    CHECK(text::project_slug("a_b-c") == "a_b-c");
    CHECK(text::project_slug("--x--") == "x");

    // 角色那个用下划线，前缀是 "x" 且 6 位
    CHECK(text::slug("Lin Wan") == "lin_wan");
    CHECK(text::slug("林晚").rfind("x", 0) == 0);
    CHECK(text::slug("林晚").size() == 7);
}
