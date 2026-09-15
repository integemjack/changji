// `assets.json` 形状不对的时候，**不能读成"这个项目还没有角色"**。
//
// `characters` / `locations` 是 id → 内容 的对象。反序列化用的是
// `NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT`——类型对不上时它不报错，
// 回默认值，也就是一个空库。于是一份手改歪了的文件读出来和新项目一模一样：
// 界面显示「还没有角色」，而文件里其实有十几个角色、几十条服装、一堆参考图
// 路径。人看不出差别，接着点「照故事定妆」，merge_bible 就把它盖掉了。
//
// 手改这个文件是**写在文档里的用法**（character.cpp：「唯一的办法是手改
// assets.json」），所以这一处必须说话。
//
// 真引擎上撞到的：往 assets.json 里写了一份 `characters` 是**数组**的库，
// /api/assets 回的是 `{"characters":[],"locations":[]}`，整页显示「还没有
// 角色」，一个字都没说文件有问题。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "models/project.hpp"

using namespace changji;
using namespace changji::models;
namespace fs = std::filesystem;

namespace {

/// 建一个空项目，回它的 store。
ProjectStore fresh(const std::string& tag) {
    const auto root = fs::temp_directory_path() / "changji_assets_shape" / tag;
    std::error_code ec;
    fs::remove_all(root, ec);
    return ProjectStore::create(root, "t_" + tag, tag, StyleLine::REALISTIC);
}

void write_assets(const ProjectStore& store, const std::string& body) {
    std::ofstream(store.paths().assets_file()) << body;
}

}  // namespace

TEST_CASE("对象形状：照常读出来") {
    auto store = fresh("ok");
    write_assets(store, R"({
      "characters": {"c_lin": {"char_id": "c_lin", "name": "林晚"}},
      "locations":  {"loc_t": {"location_id": "loc_t", "name": "天台"}}
    })");

    const AssetLibrary lib = store.load_assets();
    REQUIRE(lib.characters.size() == 1);
    CHECK(lib.characters.at("c_lin").name == "林晚");
    CHECK(lib.locations.size() == 1);
}

TEST_CASE("写成数组要抛，不能悄悄变成空库") {
    // ⚠️ 这一条是要害。抛出去之后，上层 `load_assets_or_400` 那条本来就是
    // 为"文件本身是坏的"准备的——人看到的是一句话，不是一个空页面。
    auto store = fresh("array");
    write_assets(store, R"({
      "characters": [{"char_id": "c_lin", "name": "林晚"}],
      "locations":  {}
    })");

    CHECK_THROWS_AS(store.load_assets(), std::runtime_error);

    SUBCASE("那句话得说清是哪个键、现在是什么、以及别在这个状态下重新定妆") {
        try {
            store.load_assets();
            FAIL("没抛");
        } catch (const std::runtime_error& e) {
            const std::string msg = e.what();
            CHECK(msg.find("characters") != std::string::npos);
            CHECK(msg.find("array") != std::string::npos);
            CHECK(msg.find("盖掉") != std::string::npos);
        }
    }
}

TEST_CASE("locations 写歪了同样要抛") {
    auto store = fresh("loc");
    write_assets(store, R"({"characters": {}, "locations": "天台"})");
    CHECK_THROWS_AS(store.load_assets(), std::runtime_error);
}

TEST_CASE("这两个键缺了、或者是 null，都还算「这个项目还没有角色」") {
    // **别把"没有"也算成坏**：新项目、以及只写了画风的库，走的正是这条。
    auto store = fresh("absent");

    SUBCASE("整个键都没有") {
        write_assets(store, R"({"style": {"style_line": "realistic"}})");
        CHECK(store.load_assets().characters.empty());
    }
    SUBCASE("是 null") {
        write_assets(store, R"({"characters": null, "locations": null})");
        CHECK(store.load_assets().characters.empty());
    }
    SUBCASE("文件根本不在") {
        std::error_code ec;
        fs::remove(store.paths().assets_file(), ec);
        CHECK(store.load_assets().characters.empty());
    }
}
