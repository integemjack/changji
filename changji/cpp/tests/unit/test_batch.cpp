// 两个长任务的测试。
//
// 这两个和前面五个接口不一样：**立刻返回，活干在工作线程上**。
// 所以测的东西也不一样——不是"返回体对不对"，是：
//
//   一，起任务的那一刻回什么（started / total / 409）
//   二，跑完之后盘上是什么（建出的剧集、存下的分镜）
//   三，中途出错和中途取消各自留下什么
//
// 没走对拍语料。Python 那边这两个的产出是异步写进 WriteState 的，
// 用 TestClient 录不到中间过程，只能录到最终快照——而中间过程
// （一集写砸了接着往下写）恰恰是这里最容易写错的地方。
// 所以这里直接对着行为写断言，并在注释里说清每条对应 Python 的哪一段。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/batch.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

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

fs::path fresh_copy(const std::string& tag) {
    const fs::path dst =
        fs::temp_directory_path() / paths::from_utf8("changji_批量_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine_project(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    return dst;
}

std::string script_reply(const std::string& title) {
    return json{
        {"title", title},
        {"logline", title + "的一句话"},
        {"beats", json::array({
            json{{"kind", "action"}, {"speaker", ""}, {"text", "夜。天台。"}},
            json{{"kind", "dialogue"}, {"speaker", "林晚"}, {"text", "一句台词"}},
        })}}.dump();
}

/// 每个用例前先把全局 job 表清干净。
/// 不清的话上一个用例留下的状态会让 running 判断出错。
void reset_jobs() {
    pipeline::jobs().cancel(pipeline::JobKind::Write);
    pipeline::jobs().wait_idle();
}

/// 等这个槽真正跑完（不是 running 变 false，那个在取消时立刻就变）。
void wait_done() { pipeline::jobs().wait_idle(); }

json write_snapshot() {
    return pipeline::jobs().snapshot(pipeline::JobKind::Write);
}

}  // namespace

TEST_CASE("写整季：把每一集都建出来并落库") {
    reset_jobs();
    const fs::path root = fresh_copy("写整季");
    const models::ProjectStore store(root);
    const std::size_t before = store.load_project().episodes.size();

    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{script_reply("第一话"), script_reply("第二话"),
                                 script_reply("第三话")});

    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)},
                 {"premise", "林晚在天台等一个七年没出现的人。"},
                 {"episodes", 3}}, client);
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("started") == true);
    CHECK(r.body.at("total") == 3);

    wait_done();

    const models::Project after = store.load_project();
    CHECK(after.episodes.size() == before + 3);
    // 梗概被存下来了。下次打开界面时回填，不用凭记忆重打。
    CHECK(after.premise == "林晚在天台等一个七年没出现的人。");

    const json snap = write_snapshot();
    CHECK(snap.at("running") == false);
    CHECK(snap.at("done") == 3);
    CHECK(snap.at("total") == 3);
    CHECK(snap.at("episodes").size() == 3);
    CHECK(snap.at("message") == "写完了 3 集");
    CHECK(snap.at("error").is_null());

    // 每一集的记录都带上说话人和字数，界面靠它判断写长了没有
    for (const auto& e : snap.at("episodes")) {
        CHECK_FALSE(e.at("episode_id").get<std::string>().empty());
        CHECK(e.contains("speakers"));
        CHECK(e.contains("dialogue_chars"));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("剧集编号不跳号，而且跳过预告") {
    // 预告片挂在 trailer 上。把它也数进去的话，
    // 有了预告之后新建的第二集会跳号变成 ep03。
    reset_jobs();
    const fs::path root = fresh_copy("编号");
    models::ProjectStore store(root);
    models::Project p = store.load_project();
    // 语料项目有 ep01、ep02。再塞一个 trailer 进去。
    models::Episode trailer;
    trailer.episode_id = "trailer";
    trailer.title = "预告";
    p.episodes.push_back(trailer);
    store.save_project(p);

    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{script_reply("新一话")});
    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 1}}, client);
    });
    REQUIRE(r.status == 200);
    wait_done();

    const models::Project after = store.load_project();
    bool has_ep03 = false;
    for (const auto& ep : after.episodes) {
        if (ep.episode_id == "ep03") has_ep03 = true;
        // 不该出现 ep04——那说明 trailer 被数进去了
        CHECK(ep.episode_id != "ep04");
    }
    CHECK(has_ep03);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("一集写砸了不拖垮后面几集") {
    // 跑一晚上，早上发现第二集挂了导致后面几集都没动，
    // 那这一晚上就白熬了。
    reset_jobs();
    const fs::path root = fresh_copy("写砸");
    const models::ProjectStore store(root);
    const std::size_t before = store.load_project().episodes.size();

    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        script_reply("好的一话"),
        "抱歉，我做不到。",          // 这一集会失败
        script_reply("又一话"),
    });

    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 3}}, client);
    });
    REQUIRE(r.status == 200);
    wait_done();

    // 三集都"处理过"了，但只建出两集
    const json snap = write_snapshot();
    CHECK(snap.at("done") == 3);
    CHECK(snap.at("episodes").size() == 3);
    CHECK(store.load_project().episodes.size() == before + 2);

    // 失败那条要留在列表里，而且 episode_id 是空的——这一集根本没建出来
    int failures = 0;
    for (const auto& e : snap.at("episodes")) {
        if (e.contains("error")) {
            ++failures;
            CHECK(e.at("episode_id") == "");
            CHECK_FALSE(e.at("error").get<std::string>().empty());
        }
    }
    CHECK(failures == 1);
    // 整个任务不算失败——它跑完了
    CHECK(snap.at("error").is_null());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("同一个槽上不能并发") {
    // Python 那边写整季和批量出分镜共用一个 WriteState，
    // 所以这条限制是照抄的，不是我加的。
    reset_jobs();
    const fs::path root = fresh_copy("并发");

    // 录得足够多，让第一个任务在第二个请求进来时还在跑
    std::vector<std::string> many;
    for (int i = 0; i < 20; ++i) many.push_back(script_reply("话"));
    auto client = std::make_shared<llm::ReplayClient>(many);

    const auto r1 = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 20}}, client);
    });
    REQUIRE(r1.status == 200);

    const auto r2 = http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 1}}, client);
    });
    CHECK(r2.status == 409);
    CHECK(r2.body.at("detail") == "已经在写了");

    SUBCASE("批量出分镜说的是另一句话") {
        // 用户看到"已经在写了"会去找哪里在写剧本，
        // 而实际情况是那个槽被别的事占着。
        const auto r3 = http::guard([&] {
            return http::post_plan_all(
                json{{"project", paths::to_utf8(root)}}, client);
        });
        CHECK(r3.status == 409);
        CHECK(r3.body.at("detail") == "剧本那边还在忙");
    }

    pipeline::jobs().cancel(pipeline::JobKind::Write);
    wait_done();

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("两个长任务的停止文案不一样") {
    // 同一个槽上跑的两件事，说法不该一样。
    // "已经写好的几集留着" 和 "已经出好的分镜留着" 说的是不同的东西。
    reset_jobs();
    const fs::path root = fresh_copy("停止文案");
    std::vector<std::string> many;
    for (int i = 0; i < 20; ++i) many.push_back(script_reply("话"));

    {
        auto client = std::make_shared<llm::ReplayClient>(many);
        http::guard([&] {
            return http::post_script_series(
                json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                     {"episodes", 20}}, client);
        });
        pipeline::jobs().cancel(pipeline::JobKind::Write);
        CHECK(write_snapshot().at("error") == "已手动停止。已经写好的几集留着。");
        wait_done();
    }
    {
        // 让 ep02 也有剧本，这样 plan/all 有活干
        models::ProjectStore store(root);
        models::Project p = store.load_project();
        for (auto& ep : p.episodes) {
            if (ep.episode_id == "ep02") ep.script = "林晚：一句台词";
        }
        store.save_project(p);

        auto client = std::make_shared<llm::ReplayClient>(many);
        const auto r = http::guard([&] {
            return http::post_plan_all(
                json{{"project", paths::to_utf8(root)},
                     {"overwrite", true}}, client);
        });
        REQUIRE(r.status == 200);
        pipeline::jobs().cancel(pipeline::JobKind::Write);
        CHECK(write_snapshot().at("error") == "已手动停止。已经出好的分镜留着。");
        wait_done();
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("批量出分镜只挑有剧本又没分镜的") {
    reset_jobs();
    const fs::path root = fresh_copy("挑集");
    // 语料项目：ep01 有剧本有分镜，ep02 没剧本
    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{"{}"});

    const auto r = http::guard([&] {
        return http::post_plan_all(
            json{{"project", paths::to_utf8(root)}}, client);
    });
    // ep01 已经有分镜、ep02 没剧本，所以一个都不该有
    CHECK(r.status == 400);
    CHECK(r.body.at("detail") == "没有需要出分镜的剧集。有剧本又没分镜的才算");

    SUBCASE("勾了覆盖就把有剧本的都算上") {
        std::vector<std::string> many;
        for (int i = 0; i < 10; ++i) many.push_back("{}");
        auto c2 = std::make_shared<llm::ReplayClient>(many);
        const auto r2 = http::guard([&] {
            return http::post_plan_all(
                json{{"project", paths::to_utf8(root)},
                     {"overwrite", true}}, c2);
        });
        REQUIRE(r2.status == 200);
        CHECK(r2.body.at("episodes") == json::array({"ep01"}));
        wait_done();
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("参数校验") {
    reset_jobs();
    const fs::path root = fresh_copy("校验");
    const std::string project = paths::to_utf8(root);
    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{"{}"});

    const auto check = [&](const json& body, int want) {
        const auto r = http::guard([&] {
            return http::post_script_series(body, client);
        });
        CHECK(r.status == want);
        return r;
    };

    check(json{{"project", project}}, 422);                       // 缺 premise
    check(json{{"project", project}, {"premise", "x"}, {"episodes", 0}}, 422);
    check(json{{"project", project}, {"premise", "x"}, {"episodes", 21}}, 422);
    check(json{{"project", project}, {"premise", "x"}, {"duration_s", 0}}, 422);
    check(json{{"project", project}, {"premise", "x"}, {"duration_s", 9999}}, 422);
    check(json{{"project", project}, {"premise", "x"}, {"typo", 1}}, 422);

    // 项目不存在是 400 不是 422——那不是校验问题
    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", "Z:/没有这个目录"}, {"premise", "x"}}, client);
    });
    CHECK(r.status == 400);

    // 全都没起起来
    CHECK_FALSE(pipeline::jobs().running(pipeline::JobKind::Write));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("409 优先于项目不存在") {
    // Python 那边是先判 running 再 load_project。两个都错时回哪一个
    // 是可观测的，所以顺序要照抄。
    reset_jobs();
    const fs::path root = fresh_copy("顺序");
    std::vector<std::string> many;
    for (int i = 0; i < 20; ++i) many.push_back(script_reply("话"));
    auto client = std::make_shared<llm::ReplayClient>(many);

    http::guard([&] {
        return http::post_script_series(
            json{{"project", paths::to_utf8(root)}, {"premise", "梗概"},
                 {"episodes", 20}}, client);
    });

    const auto r = http::guard([&] {
        return http::post_script_series(
            json{{"project", "Z:/没有这个目录"}, {"premise", "x"}}, client);
    });
    CHECK(r.status == 409);   // 不是 400

    pipeline::jobs().cancel(pipeline::JobKind::Write);
    wait_done();

    std::error_code ec;
    fs::remove_all(root, ec);
}
