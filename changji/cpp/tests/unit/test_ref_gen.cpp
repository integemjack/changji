// 生成参考图的接口。
//
// **真出图这一段测不了**——那要一张显卡、十几 GB 权重和几十秒。这里测的
// 是它前面那一层：参数校验（走不到出图就该拦下）、和种子那条"重出还是那
// 张图"的规矩。
//
// 校验这一层单独测是有理由的：它每一条都该在**借显存之前**就拦下来。漏一
// 条的话表现不是"报错"，而是"转了几十秒然后报一句参数不对"——错的原因，
// 而且是在最贵的地方报的。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "http/ref_gen.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    json j;
    in >> j;
    return j;
}

fs::path fresh_copy(const std::string& tag) {
    const json exp = load_golden("project_expectations");
    const fs::path src = paths::from_utf8(std::string(CHANGJI_GOLDEN_DIR)) /
                         paths::from_utf8(exp.at("root_name").get<std::string>());
    const fs::path dst =
        fs::temp_directory_path() / paths::from_utf8("changji_出参考图_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    return dst;
}

/// 跑一次，把 ApiError 的状态码取出来。没抛就是 0。
int status_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const http::ApiError& e) {
        return e.status();
    }
    return 0;
}

}  // namespace

TEST_CASE("参数不对的时候，一步都不许走到出图") {
    // 这几条都该在借显存之前就被拦下。走到出图那一步的话，测试机上会是
    // 「等几十秒然后报一句 sd.cpp 没链」——错的原因，错的时间。
    SUBCASE("朝向认不出来") {
        CHECK(status_of([] {
            http::post_character_reference_generate(
                {{"project", "/nowhere"}, {"char_id", "c_x"}, {"slot", "侧躺"}});
        }) == 400);
    }
    SUBCASE("没给项目") {
        CHECK(status_of([] {
            http::post_character_reference_generate(
                {{"project", ""}, {"char_id", "c_x"}});
        }) == 400);
    }
    SUBCASE("缺 char_id") {
        CHECK(status_of([] {
            http::post_character_reference_generate({{"project", "/nowhere"}});
        }) == 400);
    }
    SUBCASE("缺 location_id") {
        CHECK(status_of([] {
            http::post_location_reference_generate({{"project", "/nowhere"}});
        }) == 400);
    }
}

TEST_CASE("库里没有这个人 / 这个地方，回 404 而不是硬画一张") {
    const fs::path root = fresh_copy("找不到");
    CHECK(status_of([&] {
        http::post_character_reference_generate(
            {{"project", paths::to_utf8(root)}, {"char_id", "c_根本没有"}});
    }) == 404);
    CHECK(status_of([&] {
        http::post_location_reference_generate(
            {{"project", paths::to_utf8(root)}, {"location_id", "loc_没有"}});
    }) == 404);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("不给 seed 就按名字算，同一个槽位每次都是同一个") {
    // 这条是"重出还是那张图"的全部依据。用户点刷新往往只是想确认刚才那张
    // 存下来了，每点一次换一张脸的话他会以为自己弄坏了什么。
    const std::int64_t a = http::ref_seed(json::object(), "c_lin_wan_front");
    const std::int64_t b = http::ref_seed(json::object(), "c_lin_wan_front");
    CHECK(a == b);

    // 三个朝向各是各的种子：同一个种子出三张只会得到三张一样的正面。
    CHECK(a != http::ref_seed(json::object(), "c_lin_wan_three_quarter"));
    CHECK(a != http::ref_seed(json::object(), "c_lin_wan_back"));
    // 换个人也得换
    CHECK(a != http::ref_seed(json::object(), "c_other_front"));
}

TEST_CASE("给了 seed 就用它，负数和超范围的折回来") {
    CHECK(http::ref_seed({{"seed", 12345}}, "随便") == 12345);
    // sd.cpp 那边只收非负，直接透传负数会被当成"随机"，而"随机"正是这个
    // 接口不想要的
    CHECK(http::ref_seed({{"seed", -1}}, "随便") == 2147483647);
    CHECK(http::ref_seed({{"seed", 2147483648LL}}, "随便") == 0);
    // 不是整数就当没给：按名字算，而不是把 1.5 截成 1
    CHECK(http::ref_seed({{"seed", 1.5}}, "随便") ==
          http::ref_seed(json::object(), "随便"));
}
