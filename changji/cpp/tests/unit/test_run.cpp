// POST /api/run 的测试。
//
// 后端是注入的，所以整个队列几毫秒跑完。这里测的不是画面，是**编排**：
// 谁能开跑、开不了跑时回什么码、一集挂了后面几集还跑不跑、
// 阶段名写错了错误落在哪儿。最后一条尤其容易做错——做成 400 的话
// 前端弹的是错误框，而 Python 那边它是任务列表里的一条失败记录。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "http/run.hpp"
#include "models/character.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using namespace changji;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_开跑_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

models::Shot make_shot(const std::string& id, int order) {
    models::Shot s;
    s.shot_id = id;
    s.order = order;
    s.scene_id = "sc01";
    s.first_frame_prompt = "雨夜天台";
    s.duration_s = 4.0;
    return s;
}

/// 建一个项目：episodes 里每一项是 {集号, 镜头数}。镜头数 0 表示没分镜。
models::ProjectStore make_store(
    const std::string& tag,
    const std::vector<std::pair<std::string, int>>& episodes) {
    const fs::path root = temp_root(tag);
    auto store = models::ProjectStore::create(root, "yu_ye", "雨夜天台");

    models::Project project = store.load_project();
    for (const auto& [id, n] : episodes) {
        models::Episode ep;
        ep.episode_id = id;
        ep.title = id;
        for (int i = 0; i < n; ++i) {
            ep.shots.push_back(make_shot(id + "_sh" + std::to_string(i + 1), i));
        }
        project.episodes.push_back(ep);
    }
    store.save_project(project);

    models::AssetLibrary assets;
    assets.style.global_style = "电影感";
    assets.style.aspect_ratio = "9:16";
    store.save_assets(assets);
    return store;
}

/// 记下每个后端被调了几次，顺便能装成失败。
struct Fakes {
    std::vector<std::string> frames;
    std::vector<std::string> videos;

    static void stub(const fs::path& dest) {
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary);
        out << "假的";
    }

    http::RunDeps deps() {
        http::RunDeps d;
        d.settings = [] { return config::Settings{}; };
        d.profile = [] {
            models::HardwareProfile p;
            p.vram_gb = 6.0;
            p.tiers = models::tiers_for_vram(6.0);
            p.detected = true;
            return p;
        };
        d.backends = [this](const config::Settings&) {
            pipeline::Backends b;
            b.frame = [this](const models::Shot& s, const stages::PromptBundle&,
                             const models::TierSpec&, const fs::path& dest,
                             pipeline::CancelToken&, const infer::StepCallback&) {
                frames.push_back(s.shot_id);
                stub(dest);
            };
            b.video = [this](const models::Shot& s, const stages::RenderPlan&,
                             const std::optional<fs::path>&, const fs::path& dest,
                             pipeline::CancelToken&, const infer::StepCallback&) {
                videos.push_back(s.shot_id);
                stub(dest);
            };
            return b;
        };
        return d;
    }
};

/// 开跑并等它结束。任务跑在 job 表的线程上，不等的话下一条用例会撞 409。
http::ApiResult run_and_wait(const json& body, Fakes& fakes) {
    const auto r = http::post_run(body, fakes.deps());
    pipeline::jobs().wait_idle();
    return r;
}

/// 上一条用例留下的任务收干净了再开始。
void quiesce() { pipeline::jobs().wait_idle(); }

std::string project_arg(const models::ProjectStore& store) {
    return paths::to_utf8(store.root());
}

}  // namespace

TEST_CASE("开跑立刻返回，把队列一起给出来") {
    // 同步跑完再返回的话，前端那个请求要挂几十分钟，中途还会被代理掐断。
    quiesce();
    const auto store = make_store("开跑", {{"ep01", 2}});
    Fakes fakes;

    const auto r = run_and_wait(
        {{"project", project_arg(store)}, {"episode_id", "ep01"}}, fakes);

    CHECK(r.status == 200);
    CHECK(r.body["started"] == true);
    CHECK(r.body["queue"] == json::array({"ep01"}));
    CHECK(fakes.frames.size() == 2);
    // 草稿档加成片档，两镜各两次
    CHECK(fakes.videos.size() == 4);
}

TEST_CASE("已经在跑时回 409，而且说清在跑哪一集") {
    // 只说"忙"的话用户不知道是自己刚点的那次还是昨晚那次还没停。
    quiesce();
    const auto store = make_store("撞车", {{"ep01", 1}});

    // 占住槽：这个任务等到我们放行才结束
    std::atomic<bool> release{false};
    pipeline::jobs().start(pipeline::JobKind::Run, "ep01",
                           [&release](pipeline::JobProgress&) {
                               while (!release.load()) {
                                   std::this_thread::sleep_for(
                                       std::chrono::milliseconds(1));
                               }
                           });

    Fakes fakes;
    try {
        http::post_run({{"project", project_arg(store)},
                        {"episode_id", "ep01"}},
                       fakes.deps());
        release = true;
        pipeline::jobs().wait_idle();
        FAIL("该回 409");
    } catch (const http::ApiError& e) {
        release = true;
        pipeline::jobs().wait_idle();
        CHECK(e.status() == 409);
        const std::string detail = e.what();
        CAPTURE(detail);
        CHECK(detail.find("ep01") != std::string::npos);
    }
    CHECK(fakes.frames.empty());   // 第二次请求一个镜头都没碰
}

TEST_CASE("必填字段缺了是 422，不是 400") {
    // pydantic 的校验错误是 422，而且 detail 是数组不是字符串。
    // 回成 400 加一句话的话，前端拿到的形状不对，错误提示会是空的。
    quiesce();
    Fakes fakes;

    SUBCASE("没有 project") {
        try {
            http::post_run({{"episode_id", "ep01"}}, fakes.deps());
            FAIL("该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 422);
            const auto& d = e.detail();
            REQUIRE(d.contains("detail"));
            CHECK(d["detail"][0]["loc"] == json::array({"body", "project"}));
            CHECK(d["detail"][0]["type"] == "missing");
        }
    }

    SUBCASE("没有 episode_id") {
        // all_episodes 会忽略 episode_id，但字段本身仍然是必填的——
        // Python 那边它没有默认值。
        try {
            http::post_run({{"project", "C:/x"}, {"all_episodes", true}},
                           fakes.deps());
            FAIL("该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 422);
            CHECK(e.detail()["detail"][0]["loc"] ==
                  json::array({"body", "episode_id"}));
        }
    }
}

TEST_CASE("项目路径为空或者读不出来是 400") {
    quiesce();
    Fakes fakes;

    SUBCASE("空路径") {
        try {
            http::post_run({{"project", ""}, {"episode_id", "ep01"}},
                           fakes.deps());
            FAIL("该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 400);
            CHECK(std::string(e.what()).find("项目目录") != std::string::npos);
        }
    }

    SUBCASE("目录不是项目") {
        const fs::path empty = temp_root("空目录");
        try {
            http::post_run(
                {{"project", paths::to_utf8(empty)}, {"episode_id", "ep01"}},
                fakes.deps());
            FAIL("该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 400);
        }
    }
}

TEST_CASE("all_episodes 只排有分镜的集") {
    // 没分镜的集排进去只会在任务里报一条"还没有分镜表"，
    // 而用户点的是"跑整个项目"，看到一串失败会以为整个项目坏了。
    quiesce();
    const auto store =
        make_store("整季", {{"ep01", 1}, {"ep02", 0}, {"ep03", 2}});
    Fakes fakes;

    const auto r = run_and_wait({{"project", project_arg(store)},
                                 {"episode_id", "ep99"},   // 被忽略
                                 {"all_episodes", true},
                                 {"skip_final", true}},
                                fakes);

    CHECK(r.body["queue"] == json::array({"ep01", "ep03"}));
    CHECK(fakes.frames.size() == 3);

    SUBCASE("队列进度在快照里") {
        const auto snap = pipeline::jobs().snapshot(pipeline::JobKind::Run);
        CHECK(snap["queue_total"] == 2);
        CHECK(snap["queue_done"] == 2);
    }
}

TEST_CASE("all_episodes 但一集分镜都没有时 400") {
    quiesce();
    const auto store = make_store("空项目", {{"ep01", 0}});
    Fakes fakes;
    try {
        http::post_run({{"project", project_arg(store)},
                        {"episode_id", "ep01"},
                        {"all_episodes", true}},
                       fakes.deps());
        FAIL("该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 400);
        CHECK(std::string(e.what()).find("分镜表") != std::string::npos);
    }
}

TEST_CASE("一集出错不拖垮后面几集") {
    // 量产时跑一晚上，早上发现第二集挂了导致后面十集都没动，
    // 那这一晚上就白熬了。
    quiesce();
    const auto store = make_store("挂一集", {{"ep01", 1}, {"ep02", 1}});
    {
        // 把 ep02 的分镜清掉，让它在任务里抛"还没有分镜表"
        models::Project p = store.load_project();
        p.episode_by_id("ep02")->shots.clear();
        store.save_project(p);
    }
    Fakes fakes;

    // 显式给队列：all_episodes 会把没分镜的过滤掉，这里要的正是它不被过滤
    const auto r1 = run_and_wait({{"project", project_arg(store)},
                                  {"episode_id", "ep02"},
                                  {"skip_final", true}},
                                 fakes);
    CHECK(r1.status == 200);

    const auto snap = pipeline::jobs().snapshot(pipeline::JobKind::Run);
    REQUIRE(snap["error"].is_string());
    const std::string err = snap["error"];
    CAPTURE(err);
    CHECK(err.find("ep02") != std::string::npos);
    CHECK(err.find("分镜表") != std::string::npos);
    // 失败也算跑完一集，队列要往前走，否则进度条永远停在那儿
    CHECK(snap["queue_done"] == 1);
}

TEST_CASE("stages 空数组走全流程，给了内容才只跑那几个") {
    // Python 判的是 `if req.stages:`——空列表是假值。
    // 把两者合并成"空就是全跑"会让 stages:["  "] 变成重跑整集，
    // 而那是几十分钟的差别。
    quiesce();

    SUBCASE("空数组 = 全跑") {
        const auto store = make_store("空阶段", {{"ep01", 1}});
        Fakes fakes;
        run_and_wait({{"project", project_arg(store)},
                      {"episode_id", "ep01"},
                      {"stages", json::array()}},
                     fakes);
        CHECK(fakes.frames.size() == 1);
        CHECK(fakes.videos.size() == 2);
    }

    SUBCASE("只跑首帧") {
        const auto store = make_store("只首帧", {{"ep01", 2}});
        Fakes fakes;
        run_and_wait({{"project", project_arg(store)},
                      {"episode_id", "ep01"},
                      {"stages", json::array({"frames"})}},
                     fakes);
        CHECK(fakes.frames.size() == 2);
        CHECK(fakes.videos.empty());
    }

    SUBCASE("全是空白的阶段名 = 一个都不跑") {
        const auto store = make_store("空白阶段", {{"ep01", 1}});
        Fakes fakes;
        run_and_wait({{"project", project_arg(store)},
                      {"episode_id", "ep01"},
                      {"stages", json::array({"  "})}},
                     fakes);
        CHECK(fakes.frames.empty());
        CHECK(fakes.videos.empty());
    }
}

TEST_CASE("阶段名写错时错在任务里，不是 400") {
    // Python 那边这个校验在 run_stages 内部，也就是在任务线程上。
    // 做成 400 的话前端弹的是错误框，而现在的行为是任务列表里一条失败记录。
    quiesce();
    const auto store = make_store("错阶段", {{"ep01", 1}});
    Fakes fakes;

    const auto r = run_and_wait({{"project", project_arg(store)},
                                 {"episode_id", "ep01"},
                                 {"stages", json::array({"render"})}},
                                fakes);
    CHECK(r.status == 200);   // 开跑本身是成功的

    const auto snap = pipeline::jobs().snapshot(pipeline::JobKind::Run);
    REQUIRE(snap["error"].is_string());
    const std::string err = snap["error"];
    CAPTURE(err);
    CHECK(err.find("不认识的阶段 render") != std::string::npos);
    // 要说清有哪些可选的，不然用户只能去翻文档
    CHECK(err.find("assemble、audio、draft、final、frames") != std::string::npos);
    CHECK(fakes.frames.empty());
}

TEST_CASE("阶段名两边的空白要去掉") {
    // 前端从输入框里拿的字符串常带空格，而这个字段现在没有下拉约束。
    const auto only = http::parse_stages({" frames ", "draft"});
    REQUIRE(only.size() == 2);
    CHECK(only[0] == pipeline::Stage::Frames);
    CHECK(only[1] == pipeline::Stage::Draft);
}

TEST_CASE("多余的键要忽略，不能 422") {
    // RunRequest 是个普通的 BaseModel，pydantic 默认忽略多余字段。
    // 这里 forbid 的话，前端多传一个键整个请求就挂了——
    // 而前端和后端的版本不一定同步升。
    quiesce();
    const auto store = make_store("多余键", {{"ep01", 1}});
    Fakes fakes;

    const auto r = run_and_wait({{"project", project_arg(store)},
                                 {"episode_id", "ep01"},
                                 {"skip_final", true},
                                 {"这个键后端不认识", 42}},
                                fakes);
    CHECK(r.status == 200);
    CHECK(fakes.frames.size() == 1);
}
