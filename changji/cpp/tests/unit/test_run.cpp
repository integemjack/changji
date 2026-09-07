// POST /api/run 的测试。
//
// 后端是注入的，所以整个队列几毫秒跑完。这里测的不是画面，是**编排**：
// 谁能开跑、开不了跑时回什么码、一集挂了后面几集还跑不跑、
// 阶段名写错了错误落在哪儿。最后一条尤其容易做错——做成 400 的话
// 前端弹的是错误框，而 Python 那边它是任务列表里的一条失败记录。

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
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
        d.backends = [this](const config::Settings&,
                            const models::ProjectStore&) {
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

// ---- GET /api/run/preview ----
//
// 预演的价值在于**报大不报小**。报小了的预演比没有预演更糟：
// 人以为十几秒，走开了，回来发现还在跑第三集。

namespace {

/// 反斜杠。写成字面量的话，这个文件经过的每一层转义都可能吃掉一次。
constexpr char kBackslash = static_cast<char>(92);

models::HardwareProfile preview_profile(bool measured) {
    models::HardwareProfile p;
    p.vram_gb = 6.0;
    p.tiers = models::tiers_for_vram(6.0);
    p.detected = true;
    if (measured) {
        p.tiers[models::Tier::DRAFT].measured_seconds = 30.0;
        p.tiers[models::Tier::FINAL].measured_seconds = 300.0;
    } else {
        // tiers_for_vram 是**一定**会填 measured_seconds 的（按显存推的
        // 静态表），所以"没标定"要手动造出来。真会出现的场合是配置里
        // 手写了档位参数、而那台机器还没跑过一镜。
        for (auto& [tier, spec] : p.tiers) spec.measured_seconds.reset();
    }
    return p;
}

/// 找 stages 里某一阶段的镜头数。没有那一项返回 -1。
int stage_shots(const nlohmann::json& body, const std::string& name) {
    for (const auto& s : body.at("stages")) {
        if (s.at("stage") == name) return s.at("shots").get<int>();
    }
    return -1;
}

void set_status(const models::ProjectStore& store, const std::string& shot_id,
                models::ShotStatus st) {
    models::Project p = store.load_project();
    for (auto& ep : p.episodes) {
        if (models::Shot* s = ep.shot_by_id(shot_id)) s->status = st;
    }
    store.save_project(p);
}

}  // namespace

TEST_CASE("预演：一个镜头会一路走完后面所有阶段") {
    // 只按当前状态归到一个阶段的话，会告诉人"配音 2 镜，粗估 16 秒"，
    // 而实际上那两镜还要出首帧、跑草稿、跑成片，得等十几分钟。
    const auto store = make_store("预演", {{"ep01", 2}});

    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         false, false, preview_profile(false));
    REQUIRE(r.status == 200);
    // 两镜都是 PLANNED，四个阶段各两镜
    CHECK(stage_shots(r.body, "audio") == 2);
    CHECK(stage_shots(r.body, "frames") == 2);
    CHECK(stage_shots(r.body, "draft") == 2);
    CHECK(stage_shots(r.body, "final") == 2);
    CHECK(r.body["shots"] == 2);
    CHECK(r.body["idle"] == false);
    CHECK(r.body["episodes"] == json::array({"ep01"}));
}

TEST_CASE("预演：已完成的镜头不算进去") {
    const auto store = make_store("预演跳过", {{"ep01", 3}});
    set_status(store, "ep01_sh1", models::ShotStatus::FINAL_DONE);
    set_status(store, "ep01_sh2", models::ShotStatus::FRAME_DONE);

    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         false, false, preview_profile(false));
    // sh1 完成了，sh2 从草稿开始，sh3 从配音开始
    CHECK(stage_shots(r.body, "audio") == 1);
    CHECK(stage_shots(r.body, "frames") == 1);
    CHECK(stage_shots(r.body, "draft") == 2);
    CHECK(stage_shots(r.body, "final") == 2);

    SUBCASE("全完成时 idle 为真，stages 是空的") {
        set_status(store, "ep01_sh2", models::ShotStatus::FINAL_DONE);
        set_status(store, "ep01_sh3", models::ShotStatus::FINAL_DONE);
        const auto r2 = http::get_run_preview(project_arg(store), "ep01", false,
                                              false, false,
                                              preview_profile(false));
        CHECK(r2.body["idle"] == true);
        CHECK(r2.body["stages"].empty());
        // 没事可做时不报时间，报"0 秒"会让人以为是估算坏了
        CHECK(r2.body["estimate_text"] == "");
        CHECK(r2.body["estimate_s"] == 0);
    }
}

TEST_CASE("预演：人工确认过的镜头不重跑，除非明确要求") {
    // LOCKED 是用户说"这一镜就这样了"。force 之外任何情况下动它，
    // 都等于把人工确认的结果覆盖掉。
    const auto store = make_store("锁定", {{"ep01", 2}});
    set_status(store, "ep01_sh1", models::ShotStatus::LOCKED);

    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         false, false, preview_profile(false));
    CHECK(stage_shots(r.body, "audio") == 1);

    SUBCASE("force 时连锁定的也算") {
        const auto r2 = http::get_run_preview(project_arg(store), "ep01", false,
                                              false, true,
                                              preview_profile(false));
        CHECK(stage_shots(r2.body, "audio") == 2);
        CHECK(stage_shots(r2.body, "final") == 2);
    }
}

TEST_CASE("预演：skip_final 时不算成片档") {
    const auto store = make_store("跳成片预演", {{"ep01", 2}});
    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         true, false, preview_profile(true));
    CHECK(stage_shots(r.body, "final") == -1);   // 整项都不出现
    CHECK(stage_shots(r.body, "draft") == 2);

    // 时间里也不能含成片档：2*30 草稿 + 2*8 配音 + 2*12 首帧 = 100
    CHECK(r.body["estimate_s"] == 100);
    CHECK(r.body["estimate_text"] == "2 分钟");
}

TEST_CASE("预演：没标定过就只算配音和首帧那部分") {
    // 档位表里没有实测耗时时 estimate_episode 回空。
    // 那时候不能把渲染当成 0 秒——但也没有别的数可报，
    // 所以报出来的是一个偏小的数，这一条钉的是"至少不为零"。
    const auto store = make_store("没标定", {{"ep01", 1}});
    const auto r = http::get_run_preview(project_arg(store), "ep01", false,
                                         false, false, preview_profile(false));
    CHECK(r.body["estimate_s"] == 20);   // 8 + 12
    CHECK(r.body["estimate_text"] == "20 秒");
}

TEST_CASE("预演：all_episodes 只看有分镜的集") {
    const auto store =
        make_store("预演整季", {{"ep01", 1}, {"ep02", 0}, {"ep03", 2}});
    const auto r = http::get_run_preview(project_arg(store), "", true, false,
                                         false, preview_profile(false));
    CHECK(r.body["episodes"] == json::array({"ep01", "ep03"}));
    CHECK(r.body["shots"] == 3);
}

TEST_CASE("预演：没有可跑的剧集时 400") {
    const auto store = make_store("预演空", {{"ep01", 1}});
    try {
        http::get_run_preview(project_arg(store), "ep99", false, false, false,
                              preview_profile(false));
        FAIL("该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 400);
        CHECK(std::string(e.what()).find("先出分镜") != std::string::npos);
    }
}

// ---- GET /api/outputs ----

TEST_CASE("成片列表：没有 output 目录时回空数组") {
    // 回 404 或者报错的话，一个刚建好还没跑过的项目一打开就是红的。
    const auto store = make_store("没成片", {{"ep01", 1}});
    std::error_code ec;
    fs::remove_all(store.paths().output(), ec);

    const auto r = http::get_outputs(project_arg(store));
    CHECK(r.status == 200);
    CHECK(r.body["files"] == json::array());
}

TEST_CASE("成片列表：新的在前，只列 mp4") {
    const auto store = make_store("成片", {{"ep01", 1}});
    const fs::path out = store.paths().output();
    std::error_code ec;
    fs::create_directories(out, ec);

    const auto write = [&](const std::string& name, std::size_t bytes) {
        std::ofstream f(out / paths::from_utf8(name), std::ios::binary);
        f << std::string(bytes, 'x');
    };
    write("第一集.mp4", 3 * 1024 * 1024);
    write("说明.txt", 10);
    // 时间要拉开，不然两个文件同一秒，顺序就成了目录遍历顺序
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write("第二集.mp4", 1536 * 1024);

    const auto r = http::get_outputs(project_arg(store));
    REQUIRE(r.body["files"].size() == 2);   // txt 不算
    CHECK(r.body["files"][0]["name"] == "第二集.mp4");
    CHECK(r.body["files"][1]["name"] == "第一集.mp4");

    SUBCASE("大小保留一位小数") {
        CHECK(r.body["files"][0]["size_mb"] == doctest::Approx(1.5));
        CHECK(r.body["files"][1]["size_mb"] == doctest::Approx(3.0));
    }

    SUBCASE("rel 是相对项目根的，用正斜杠") {
        // 存绝对路径的话项目拷到别的机器就断链；用反斜杠的话
        // 在 Windows 上存的项目拿到 Linux 上读不了。
        const std::string rel = r.body["files"][0]["rel"];
        CAPTURE(rel);
        CHECK(rel.find(kBackslash) == std::string::npos);
        CHECK(rel.rfind("output/", 0) == 0);
    }

    SUBCASE("mtime 是真正的 Unix 秒") {
        // 直接用 file_time_type::time_since_epoch() 的话，MSVC 给的是
        // 1601 纪元的秒数——前端按 Unix 秒算会得出 2381 年，
        // 然后一律显示"刚刚"。每个文件都显示"刚刚"，看起来像是没坏。
        const long long mt = r.body["files"][0]["mtime"];
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        CAPTURE(mt);
        CAPTURE(now);
        CHECK(std::llabs(mt - now) < 300);
    }
}

TEST_CASE("成片列表：项目路径为空是 400") {
    try {
        http::get_outputs("");
        FAIL("该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 400);
    }
}
