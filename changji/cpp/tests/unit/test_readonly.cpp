// 阶段 2 只读接口的对拍测试。
//
// 语料是**真实 FastAPI 路由的响应**，不是照着路由代码重拼的 dict——
// 那样只能测出写脚本的人对代码的理解。export_golden.py 起一个 TestClient
// 打真接口，拿到什么存什么，含三条错误分支。
//
// 比较标准按方案第三节：结构兼容，key 顺序不管。nlohmann 的 operator==
// 是顺序无关的深比较，正好对应那条标准。

#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path
                    << "（先跑 cpp/tests/export_golden.py）");
    json j;
    in >> j;
    return j;
}

/// 语料是用固定的 vram_gb_override 导出的，这边必须给同一个值。
/// 不钉的话档位会跟着跑测试那台机器的显卡走，换台机器就红。
config::Settings pinned_settings(double vram) {
    config::Settings s;
    s.vram_gb_override = vram;
    return s;
}

/// 按 url 把用例分派到对应的接口函数。
http::ApiResult dispatch(const json& c, const config::Settings& s) {
    const std::string url = c.at("url").get<std::string>();
    const json& p = c.at("params");
    const auto param = [&](const char* k) -> std::string {
        return p.contains(k) ? p.at(k).get<std::string>() : std::string();
    };

    if (url == "/api/hardware") return http::guard([&] { return http::get_hardware(s); });
    if (url == "/api/settings") return http::guard([&] { return http::get_settings(s); });
    if (url == "/api/projects") return http::guard([&] { return http::get_projects(s); });
    if (url == "/api/project")
        return http::guard([&] { return http::get_project(param("path")); });
    if (url == "/api/assets")
        return http::guard([&] { return http::get_assets(param("path")); });
    if (url == "/api/shots")
        return http::guard([&] {
            return http::get_shots(param("path"), param("episode_id"));
        });

    FAIL("语料里有没实现的接口: " << url);
    return {500, json::object()};
}

}  // namespace

TEST_CASE("六个只读接口的响应与 Python 一致") {
    const json g = load_golden("endpoints_readonly");
    const auto settings = pinned_settings(g.at("pinned_vram_gb").get<double>());

    for (const auto& c : g.at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);

        const http::ApiResult got = dispatch(c, settings);

        // 状态码必须一致。错误分支尤其重要：前端按状态码分流，
        // 400 和 404 走的是两条不同的提示。
        CHECK(got.status == c.at("status").get<int>());

        // body 深比较，key 顺序不管
        const json& want = c.at("body");
        if (got.body != want) {
            // 失败时把差异指出来，否则一整个 JSON 对比看不出哪儿不对
            MESSAGE("期望: " << want.dump(2));
            MESSAGE("实得: " << got.body.dump(2));
        }
        CHECK(got.body == want);
    }
}

TEST_CASE("错误响应的形状与 FastAPI 的 HTTPException 一致") {
    const json g = load_golden("endpoints_readonly");
    const auto settings = pinned_settings(g.at("pinned_vram_gb").get<double>());

    int checked = 0;
    for (const auto& c : g.at("cases")) {
        if (c.at("status").get<int>() < 400) continue;
        ++checked;
        CAPTURE(c.at("name").get<std::string>());

        const http::ApiResult got = dispatch(c, settings);
        // FastAPI 的 HTTPException 回的是 {"detail": "..."}，
        // 前端只认这个形状，多一层或换个键名都会让错误提示变成空白
        REQUIRE(got.body.is_object());
        REQUIRE(got.body.contains("detail"));
        CHECK(got.body.at("detail").is_string());
        CHECK_FALSE(got.body.at("detail").get<std::string>().empty());
    }
    CHECK_MESSAGE(checked >= 3, "语料里应该有至少三条错误分支");
}

TEST_CASE("项目路径为空时是 400 不是 500") {
    // 这一条单独拎出来：漏了的话表现是用户没选项目时看到
    // "Internal Server Error"，而不是"没有指定项目目录"
    const auto r = http::guard([] { return http::get_project(""); });
    CHECK(r.status == 400);
    CHECK(r.body.at("detail").get<std::string>() == "没有指定项目目录");
}

TEST_CASE("接口层不会因为异常而崩") {
    // guard 的契约：任何异常都变成 5xx 的 JSON，不能穿出去。
    // 阶段 0 的 doctor 就是因为异常穿到 std::terminate 而静默死掉的。
    const auto r = http::guard([]() -> http::ApiResult {
        throw std::runtime_error("故意抛一个");
    });
    CHECK(r.status == 500);
    REQUIRE(r.body.contains("detail"));
    // 真实信息要带出来。FastAPI 默认那句 "Internal Server Error"
    // 什么也没说，用户报障时只能贴一张没用的截图。
    CHECK(r.body.at("detail").get<std::string>().find("故意抛一个") !=
          std::string::npos);
}
