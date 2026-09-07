// /api/bible 和 /api/plan 的对拍测试。
//
// 语料由 tests/export_planning_golden.py 走真实路由生成，大模型换成桩。
//
// 这两个接口和三个剧本接口不同的地方：**它们写盘**。所以除了响应体，
// 还要比对落盘之后的 assets.json 和 project.json——响应体对了不代表
// 存下去的东西对，而存错了要到下一步跑分镜时才发现。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/planning.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/endpoints_planning.json";
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
        json j;
        in >> j;
        return j;
    }();
    return g;
}

fs::path pristine_project() {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/project_expectations.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    json exp;
    in >> exp;
    return paths::from_utf8(std::string(CHANGJI_GOLDEN_DIR)) /
           paths::from_utf8(exp.at("root_name").get<std::string>());
}

json read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到 " << paths::to_utf8(p));
    json j;
    in >> j;
    return j;
}

void write_json(const fs::path& p, const json& j) {
    std::ofstream out(p, std::ios::binary);
    REQUIRE(out.good());
    out << j.dump();
}

fs::path fresh_copy(const std::string& tag, bool clear_assets) {
    const fs::path dst =
        fs::temp_directory_path() / paths::from_utf8("changji_出片_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine_project(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    if (clear_assets) {
        // 只清角色和场景，style 原样留着——多写或少写一个顶层键，
        // 资产库整份加载失败，那个失败会以 400 出现在别的地方。
        json a = read_json(dst / "assets.json");
        a["characters"] = json::object();
        a["locations"] = json::object();
        write_json(dst / "assets.json", a);
    }
    return dst;
}

json localize(const json& body, const fs::path& root) {
    json out = body;
    if (out.contains("project") && out.at("project").is_string()) {
        const std::string p = out.at("project").get<std::string>();
        if (p.rfind("Z:", 0) != 0) out["project"] = paths::to_utf8(root);
    }
    return out;
}

/// 按语料里录的调用顺序吐返回。
///
/// /api/plan 一次请求里可能先调圣经再调分镜，两次的结构完全不同，
/// 所以不能只给一条。
std::vector<std::string> replies_for(const json& c) {
    std::vector<std::string> out;
    for (const auto& p : c.at("prompts")) {
        out.push_back(p.at("stage") == "bible"
                          ? c.at("llm_bible").get<std::string>()
                          : c.at("llm_storyboard").get<std::string>());
    }
    // 语料里没录到调用（早早就 4xx 了）也要给一条，
    // 否则 C++ 侧万一真调了，报的是"回放录到头了"而不是真实原因
    if (out.empty()) out.push_back("{}");
    return out;
}

}  // namespace

TEST_CASE("两个出片接口和 Python 逐条对拍") {
    int idx = 0;
    for (const auto& c : golden().at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        const std::string url = c.at("url").get<std::string>();
        CAPTURE(name);
        CAPTURE(url);

        const fs::path root =
            fresh_copy(std::to_string(idx++), c.at("clear_assets").get<bool>());
        const json body = localize(c.at("body"), root);

        llm::ReplayClient client(replies_for(c));
        pipeline::CancelToken tok;
        const http::ApiResult got = http::guard([&] {
            return url == "/api/bible" ? http::post_bible(body, client, tok)
                                       : http::post_plan(body, client, tok);
        });

        const int want_status = c.at("cpp_status").get<int>();
        if (got.status != want_status) MESSAGE("body: " << got.body.dump());
        CHECK(got.status == want_status);

        const std::string mode = c.at("compare").get<std::string>();
        if (mode == "full") {
            if (got.body != c.at("response")) {
                MESSAGE("期望 " << c.at("response").dump(1));
                MESSAGE("实得 " << got.body.dump(1));
            }
            CHECK(got.body == c.at("response"));
        } else if (mode == "detail_arr") {
            REQUIRE(got.body.is_object());
            REQUIRE(got.body.at("detail").is_array());
            REQUIRE_FALSE(got.body.at("detail").empty());
            const json& mine = got.body.at("detail")[0];
            const json& theirs = c.at("response").at("detail")[0];
            CHECK(mine.at("type") == theirs.at("type"));
            CHECK(mine.at("loc") == theirs.at("loc"));
        } else {
            REQUIRE(got.body.is_object());
            REQUIRE(got.body.contains("detail"));
            CHECK(got.body.at("detail").is_string());
            CHECK_FALSE(got.body.at("detail").get<std::string>().empty());
        }

        // ---- 提示词 ----
        const auto& want_prompts = c.at("prompts");
        REQUIRE(client.calls().size() == want_prompts.size());
        for (std::size_t i = 0; i < want_prompts.size(); ++i) {
            const std::string want = want_prompts[i].at("prompt").get<std::string>();
            const std::string mine = client.calls()[i].prompt;
            if (mine != want) {
                std::size_t k = 0;
                while (k < mine.size() && k < want.size() && mine[k] == want[k]) ++k;
                MESSAGE("提示词第 " << i << " 条（"
                        << want_prompts[i].at("stage").get<std::string>()
                        << "），第一处不同在字节 " << k);
                MESSAGE("期望…" << want.substr(k > 40 ? k - 40 : 0, 110));
                MESSAGE("实得…" << mine.substr(k > 40 ? k - 40 : 0, 110));
            }
            CHECK(mine == want);
        }

        // ---- 落盘 ----
        //
        // 响应体对了不代表存下去的东西对，而存错了要到下一步跑分镜时才发现。
        if (!c.at("assets_after").is_null()) {
            const json mine = read_json(root / "assets.json");
            if (mine != c.at("assets_after")) {
                MESSAGE("assets 期望 " << c.at("assets_after").dump(1));
                MESSAGE("assets 实得 " << mine.dump(1));
            }
            CHECK(mine == c.at("assets_after"));
        }
        if (!c.at("project_after").is_null()) {
            json mine = read_json(root / "project.json");
            json want = c.at("project_after");
            // updated_at 是墙上时钟，导出那一刻和跑测试这一刻必然不同。
            // 它有没有被刷新另有一条用例管，这里只比内容。
            mine.erase("updated_at");
            want.erase("updated_at");
            if (mine != want) {
                MESSAGE("project 期望 " << want.dump(1));
                MESSAGE("project 实得 " << mine.dump(1));
            }
            CHECK(mine == want);
        }

        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("写盘要刷新 updated_at") {
    // 上面那条用例比对 project.json 时把 updated_at 剔掉了，
    // 那就得单独确认它真的被刷新了——不刷的话，前端靠它判断
    // "项目有没有变过"的逻辑会一直认为没变。
    const fs::path root = fresh_copy("时间戳", true);
    const json before = read_json(root / "project.json");

    llm::ReplayClient cl({
        golden().at("cases")[0].at("llm_bible").get<std::string>(),
        golden().at("cases")[10].at("llm_storyboard").get<std::string>()});
    pipeline::CancelToken tok;
    const auto r = http::guard([&] {
        return http::post_plan(
            json{{"project", paths::to_utf8(root)},
                 {"script", "林晚：一句台词"}, {"episode_id", "ep01"}}, cl, tok);
    });
    REQUIRE(r.status == 200);

    const json after = read_json(root / "project.json");
    CHECK(after.at("updated_at") != before.at("updated_at"));
    // created_at 不能动
    CHECK(after.at("created_at") == before.at("created_at"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("角色圣经是合并不是替换") {
    // 角色和场景是全剧共用的库。第五集的场景要出的时候，前四集的还在里面。
    // 整个换掉的话，那些场景连同它们的空景图一起没了，而分镜表里还留着
    // 指向它们的 id，跑起来直接报「场景未注册」。
    const fs::path root = fresh_copy("合并", false);
    const json before = read_json(root / "assets.json");
    REQUIRE(before.at("characters").contains("c_lin_yuan"));

    const json& c = golden().at("cases")[0];
    llm::ReplayClient client({c.at("llm_bible").get<std::string>()});
    pipeline::CancelToken tok;
    const auto r = http::guard([&] {
        return http::post_bible(
            json{{"project", paths::to_utf8(root)}, {"script", "剧本"}},
            client, tok);
    });
    REQUIRE(r.status == 200);

    const json after = read_json(root / "assets.json");
    // 原有的角色还在
    CHECK(after.at("characters").contains("c_lin_yuan"));
    // 新的也进来了
    CHECK(after.at("characters").contains("c_lin_wan"));
    CHECK(after.at("characters").contains("c_chen_mo"));
    // 原有的场景一个都没丢
    for (const auto& kv : before.at("locations").items()) {
        CAPTURE(kv.key());
        CHECK(after.at("locations").contains(kv.key()));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("不覆盖时不退回已渲染的镜头") {
    // 只新增没覆盖的话，老镜头用的还是原来那份设定，不用动。
    // 无条件退回的话，用户点一次"补角色"就把跑了一晚上的成果全废了。
    const fs::path root = fresh_copy("不覆盖", false);
    const json& c = golden().at("cases")[0];

    pipeline::CancelToken tok;
    llm::ReplayClient client({c.at("llm_bible").get<std::string>()});
    const auto r = http::guard([&] {
        return http::post_bible(
            json{{"project", paths::to_utf8(root)}, {"script", "剧本"}},
            client, tok);
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("reset_shots") == 0);

    SUBCASE("覆盖时要退回") {
        // 外观变了等于全剧提示词都变了，已渲染的镜头和新设定对不上，
        // 留着比重跑更糟：用户以为改生效了，成片里却混着两套设定。
        const fs::path root2 = fresh_copy("要覆盖", false);
        llm::ReplayClient c2({c.at("llm_bible").get<std::string>()});
        const auto r2 = http::guard([&] {
            return http::post_bible(
                json{{"project", paths::to_utf8(root2)},
                     {"script", "剧本"},
                     {"overwrite", true}}, c2, tok);
        });
        REQUIRE(r2.status == 200);
        // 语料项目里有一个 audio_done 的镜头，它该被退回
        CHECK(r2.body.at("reset_shots").get<int>() > 0);

        std::error_code ec2;
        fs::remove_all(root2, ec2);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("plan 不校验多余字段，bible 校验") {
    // PlanRequest 没写 extra="forbid"，pydantic 默认忽略多余字段。
    // 给它加上校验就会拒掉 Python 能接受的请求。
    const fs::path root = fresh_copy("多余字段", true);
    const std::string project = paths::to_utf8(root);
    const json& plan_case = golden().at("cases")[0];
    pipeline::CancelToken tok;

    {
        // bible 要拒
        llm::ReplayClient c({"{}"});
        const auto r = http::guard([&] {
            return http::post_bible(
                json{{"project", project}, {"typo", 1}}, c, tok);
        });
        CHECK(r.status == 422);
    }
    {
        // plan 要收
        llm::ReplayClient c({
            plan_case.at("llm_bible").get<std::string>(),
            golden().at("cases")[9].at("llm_storyboard").get<std::string>()});
        const auto r = http::guard([&] {
            return http::post_plan(
                json{{"project", project}, {"script", "林晚：一句台词"},
                     {"episode_id", "ep01"}, {"typo", 1}}, c, tok);
        });
        // 不该因为 typo 而 422
        CHECK(r.status != 422);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("圣经找剧本的三级回落") {
    // 优先级：请求里给的 > 指定集的 > 第一集有内容的。
    // 三级都空才报错。角色设定是全剧共用的，拿哪一集出都行，
    // 但总得有一集写好了。
    const fs::path root = fresh_copy("找剧本", false);
    const std::string project = paths::to_utf8(root);
    const json& c = golden().at("cases")[0];
    pipeline::CancelToken tok;

    SUBCASE("请求里给了就用它") {
        llm::ReplayClient cl({c.at("llm_bible").get<std::string>()});
        http::guard([&] {
            return http::post_bible(
                json{{"project", project}, {"script", "请求里给的剧本"}}, cl, tok);
        });
        REQUIRE(cl.calls().size() == 1);
        CHECK(cl.calls()[0].prompt.find("请求里给的剧本") != std::string::npos);
    }

    SUBCASE("没给就用项目里第一集有内容的") {
        llm::ReplayClient cl({c.at("llm_bible").get<std::string>()});
        http::guard([&] {
            return http::post_bible(json{{"project", project}}, cl, tok);
        });
        REQUIRE(cl.calls().size() == 1);
        // 语料项目 ep01 有剧本、ep02 没有
        const models::ProjectStore store(root);
        const models::Project p = store.load_project();
        const models::Episode* ep = p.episode_by_id("ep01");
        REQUIRE(ep != nullptr);
        CHECK(cl.calls()[0].prompt.find(ep->script) != std::string::npos);
    }

    SUBCASE("一集都没写就报错") {
        const fs::path empty = fresh_copy("没剧本", false);
        models::ProjectStore store(empty);
        models::Project p = store.load_project();
        for (auto& ep : p.episodes) ep.script.clear();
        store.save_project(p);

        llm::ReplayClient cl({"{}"});
        const auto r = http::guard([&] {
            return http::post_bible(
                json{{"project", paths::to_utf8(empty)}}, cl, tok);
        });
        CHECK(r.status == 400);
        // 一次模型都不该调
        CHECK(cl.calls().empty());

        std::error_code ec2;
        fs::remove_all(empty, ec2);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("plan 只在资产库为空或强制时才重出圣经") {
    // 已有就不重做，避免覆盖用户改过的设定。
    const json& sb_existing = golden().at("cases")[9].at("llm_storyboard");
    const json& bible = golden().at("cases")[0].at("llm_bible");
    pipeline::CancelToken tok;

    SUBCASE("资产库有角色，只调一次分镜") {
        const fs::path root = fresh_copy("不重出", false);
        llm::ReplayClient cl({sb_existing.get<std::string>()});
        const auto r = http::guard([&] {
            return http::post_plan(
                json{{"project", paths::to_utf8(root)},
                     {"script", "林晚：一句台词"}, {"episode_id", "ep01"}},
                cl, tok);
        });
        REQUIRE(r.status == 200);
        CHECK(cl.calls().size() == 1);   // 只有分镜

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    SUBCASE("资产库是空的，先出圣经再出分镜") {
        const fs::path root = fresh_copy("要重出", true);
        llm::ReplayClient cl({bible.get<std::string>(),
                              golden().at("cases")[10]
                                  .at("llm_storyboard").get<std::string>()});
        const auto r = http::guard([&] {
            return http::post_plan(
                json{{"project", paths::to_utf8(root)},
                     {"script", "林晚：一句台词"}, {"episode_id", "ep01"}},
                cl, tok);
        });
        REQUIRE(r.status == 200);
        CHECK(cl.calls().size() == 2);

        std::error_code ec;
        fs::remove_all(root, ec);
    }
}
