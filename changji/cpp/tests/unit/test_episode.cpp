// 整集流水线的测试。
//
// 两个后端都是注入的，所以整条编排能在几毫秒里跑完——而真跑一集要几十分钟。
// 这里测的正是那几十分钟里看不出来、跑完才发现的东西：
//
//   * 阶段之间是**分批**的，不是按镜头串行的（预算只装得下一个模型时，
//     串行是加载 80 次，分批是 2 次）
//   * 每个阶段跑完**立刻存盘**（不存的话中途停了，前面几十分钟白跑）
//   * 哪些镜头该跑、哪些该跳过（跳错了表现为"有些镜头一直没动"）

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "models/character.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"
#include "pipeline/episode.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_整集_" + tag);
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
    s.motion_prompt = "镜头缓慢推近";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    s.duration_s = 4.0;
    models::CharacterInShot in_shot;
    in_shot.char_id = "c_lin_wan";
    s.characters.push_back(in_shot);
    return s;
}

/// 建一个能跑的项目：一集，n 个镜头，全都是 PLANNED。
models::ProjectStore make_store(const std::string& tag, int n) {
    const fs::path root = temp_root(tag);
    auto store = models::ProjectStore::create(root, "yu_ye", "雨夜天台");

    models::Project project = store.load_project();
    models::Episode ep;
    ep.episode_id = "ep01";
    ep.title = "第一集";
    for (int i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "ep01_sh%03d", i + 1);
        ep.shots.push_back(make_shot(buf, i));
    }
    project.episodes.push_back(ep);
    store.save_project(project);

    models::AssetLibrary assets;
    models::Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.appearance.identity = "二十七岁女性";
    assets.characters["c_lin_wan"] = lin;
    assets.style.global_style = "电影感";
    assets.style.aspect_ratio = "9:16";
    store.save_assets(assets);
    return store;
}

models::HardwareProfile make_profile() {
    models::HardwareProfile p;
    p.vram_gb = 6.0;
    p.tiers = models::tiers_for_vram(6.0);
    p.detected = true;
    return p;
}

/// 一次后端调用的记录。测"谁先谁后"靠它。
struct Call {
    std::string kind;   ///< "frame" 或 "draft"/"final"
    std::string shot_id;
    bool had_start_image = false;
};

/// 两个后端都往同一个 log 里记，这样调用顺序是可比的。
/// 分成两个 log 的话就只知道各自内部的顺序，而要测的恰恰是**交错与否**。
struct Recorder {
    std::vector<Call> calls;
    /// 这些镜头的首帧要失败。
    std::vector<std::string> frame_fails;
    /// 这些镜头的视频要失败。
    std::vector<std::string> video_fails;
    /// 出到第几个视频时请求取消。0 表示不取消。
    int cancel_at_video = 0;
    pipeline::CancelToken* tok = nullptr;

    static void write_stub(const fs::path& dest, const char* what) {
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary);
        out << what;
    }

    stages::FrameRenderer frame() {
        return [this](const models::Shot& shot, const stages::PromptBundle&,
                      const models::TierSpec&, const fs::path& dest,
                      pipeline::CancelToken&, const infer::StepCallback&) {
            calls.push_back({"frame", shot.shot_id, false});
            if (std::find(frame_fails.begin(), frame_fails.end(),
                          shot.shot_id) != frame_fails.end()) {
                throw std::runtime_error("假装出图失败");
            }
            write_stub(dest, "假的 PNG");
        };
    }

    stages::VideoRenderer video() {
        return [this](const models::Shot& shot, const stages::RenderPlan& plan,
                      const std::optional<fs::path>& start,
                      const fs::path& dest, pipeline::CancelToken&,
                      const infer::StepCallback&) {
            calls.push_back({plan.tier == models::Tier::FINAL ? "final" : "draft",
                             shot.shot_id, start.has_value()});
            int videos = 0;
            for (const auto& c : calls) {
                if (c.kind != "frame") ++videos;
            }
            if (cancel_at_video > 0 && videos >= cancel_at_video && tok) {
                tok->request();
            }
            if (std::find(video_fails.begin(), video_fails.end(),
                          shot.shot_id) != video_fails.end()) {
                throw std::runtime_error("假装出片失败");
            }
            write_stub(dest, "假的 MP4");
        };
    }

    std::vector<std::string> ids_of(const std::string& kind) const {
        std::vector<std::string> out;
        for (const auto& c : calls) {
            if (c.kind == kind) out.push_back(c.shot_id);
        }
        return out;
    }
};

/// 在 job 表上跑一集，等它结束。
pipeline::RunReport run_it(const models::ProjectStore& store,
                           const pipeline::RunOptions& opts, Recorder& rec,
                           pipeline::CancelToken& tok,
                           std::vector<nlohmann::json>* msgs = nullptr) {
    pipeline::JobTable table;
    if (msgs) {
        table.set_sink([msgs](const std::string&, const nlohmann::json& m) {
            msgs->push_back(m);
        });
    }
    rec.tok = &tok;

    pipeline::Backends backends;
    backends.frame = rec.frame();
    backends.video = rec.video();

    pipeline::RunReport report;
    std::string thrown;
    table.start(pipeline::JobKind::Run, opts.episode_id,
                [&](pipeline::JobProgress& p) {
                    try {
                        report = pipeline::run_episode(
                            store, make_profile(), opts, backends, p, tok);
                    } catch (const std::exception& e) {
                        thrown = e.what();
                    }
                });
    table.wait_idle();
    if (!thrown.empty()) throw std::runtime_error(thrown);
    return report;
}

models::Episode reload(const models::ProjectStore& store) {
    models::Project p = store.load_project();
    const models::Episode* ep = p.episode_by_id("ep01");
    REQUIRE(ep != nullptr);
    return *ep;
}

}  // namespace

TEST_CASE("阶段之间分批，不是按镜头串行") {
    // **这一条钉的是方案里的一个结论，不只是当前实现。**
    // 预算只装得下一个模型时，按镜头串行要在图像模型和视频模型之间
    // 来回切 2N 次；按阶段分批只切 2 次。四十个镜头就是 80 次对 2 次。
    // 改成"出一张图就出一段视频"看起来更自然，但那会让这台 6GB 的机器
    // 把绝大部分时间花在加载模型上。
    const auto store = make_store("分批", 4);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;

    run_it(store, opts, rec, tok);

    // 所有 frame 调用都排在所有 draft 调用之前
    std::size_t last_frame = 0;
    std::size_t first_draft = rec.calls.size();
    for (std::size_t i = 0; i < rec.calls.size(); ++i) {
        if (rec.calls[i].kind == "frame") last_frame = i;
        if (rec.calls[i].kind == "draft" && i < first_draft) first_draft = i;
    }
    CHECK(rec.ids_of("frame").size() == 4);
    CHECK(rec.ids_of("draft").size() == 4);
    CHECK(last_frame < first_draft);
}

TEST_CASE("镜头按 order 跑，不按在数组里的位置") {
    // 分镜表被手工插过一镜之后，数组顺序和 order 就对不上了。
    // 画面的连贯性是按 order 来的：出图时前后镜的关系错了，
    // 人物的朝向和光线会接不上，而那要到成片拼起来才看得出。
    auto store = make_store("排序", 3);
    {
        models::Project p = store.load_project();
        models::Episode* ep = p.episode_by_id("ep01");
        ep->shots[0].order = 20;   // 第一个排到最后
        ep->shots[2].order = 1;
        store.save_project(p);
    }

    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;
    run_it(store, opts, rec, tok);

    const auto frames = rec.ids_of("frame");
    REQUIRE(frames.size() == 3);
    CHECK(frames[0] == "ep01_sh002");   // order 1
    CHECK(frames[1] == "ep01_sh003");   // order 2
    CHECK(frames[2] == "ep01_sh001");   // order 20
}

TEST_CASE("每个阶段跑完立刻存盘") {
    // 不存的话，跑到一半点停止或者断电，前面几十分钟的产出全部作废——
    // 图还在磁盘上，但项目文件里没记，下次跑会当成没跑过，重新算一遍。
    const auto store = make_store("存盘", 3);
    Recorder rec;
    rec.cancel_at_video = 1;   // 出完第一段视频就停
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;

    run_it(store, opts, rec, tok);

    // 从磁盘重新读一遍。内存里的对象对不对不重要，
    // 下次续跑读的是文件。
    const models::Episode ep = reload(store);
    for (const auto& s : ep.shots) {
        CAPTURE(s.shot_id);
        REQUIRE(s.frame_path.has_value());
        CHECK_FALSE(s.frame_path->empty());
    }
    // 第一镜的视频也存下来了
    const models::Shot* first = ep.shot_by_id("ep01_sh001");
    REQUIRE(first != nullptr);
    CHECK(first->status == models::ShotStatus::DRAFT_DONE);
    CHECK(first->video_path.has_value());
}

TEST_CASE("状态一路推到成片") {
    const auto store = make_store("状态", 2);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";

    const auto report = run_it(store, opts, rec, tok);

    CHECK(report.frames.size() == 2);
    CHECK(report.draft.size() == 2);
    CHECK(report.final_.size() == 2);
    CHECK(report.ok());

    const models::Episode ep = reload(store);
    for (const auto& s : ep.shots) {
        CAPTURE(s.shot_id);
        CHECK(s.status == models::ShotStatus::FINAL_DONE);
    }

    SUBCASE("草稿和成片分目录存") {
        // 混在一起的话重跑成片时分不清哪个 mp4 是哪一档的，
        // 而它们文件名一模一样。
        CHECK(fs::exists(store.paths().shots("draft") /
                         paths::from_utf8("ep01_sh001.mp4")));
        CHECK(fs::exists(store.paths().shots("final") /
                         paths::from_utf8("ep01_sh001.mp4")));
    }
}

TEST_CASE("skip_final 到草稿就停") {
    const auto store = make_store("跳成片", 2);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;

    const auto report = run_it(store, opts, rec, tok);

    CHECK(report.final_.empty());
    CHECK(rec.ids_of("final").empty());
    const models::Episode ep = reload(store);
    CHECK(ep.shots[0].status == models::ShotStatus::DRAFT_DONE);
}

TEST_CASE("only 指定阶段时只跑那一段") {
    // 用途是单独重做某一段：改完分镜只重渲染，不必把首帧再出一遍。
    const auto store = make_store("单阶段", 2);

    // 先把首帧跑出来
    {
        Recorder rec;
        pipeline::CancelToken tok;
        pipeline::RunOptions opts;
        opts.episode_id = "ep01";
        opts.only = {pipeline::Stage::Frames};
        run_it(store, opts, rec, tok);
        CHECK(rec.ids_of("draft").empty());
        CHECK(reload(store).shots[0].status == models::ShotStatus::FRAME_DONE);
    }

    // 再单独跑草稿
    {
        Recorder rec;
        pipeline::CancelToken tok;
        pipeline::RunOptions opts;
        opts.episode_id = "ep01";
        opts.only = {pipeline::Stage::Draft};
        run_it(store, opts, rec, tok);
        CHECK(rec.ids_of("frame").empty());
        CHECK(rec.ids_of("draft").size() == 2);
    }
}

TEST_CASE("首帧失败的镜头退回纯文生视频，不被跳过") {
    // 跳过的话整集会缺一镜，而缺的那一镜要到装配时才发现。
    // 退回纯文生视频画面一致性会差一些，但整集能出来。
    const auto store = make_store("退回", 3);
    Recorder rec;
    rec.frame_fails = {"ep01_sh002"};
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;

    const auto report = run_it(store, opts, rec, tok);

    // 三镜都渲了视频
    CHECK(rec.ids_of("draft").size() == 3);
    // 但失败那一镜没有首帧当起点
    for (const auto& c : rec.calls) {
        if (c.kind != "draft") continue;
        CAPTURE(c.shot_id);
        CHECK(c.had_start_image == (c.shot_id != "ep01_sh002"));
    }
    // 失败的那次记了次数，下次重试会换种子
    const models::Episode ep = reload(store);
    const models::Shot* failed = ep.shot_by_id("ep01_sh002");
    REQUIRE(failed != nullptr);
    CHECK(failed->attempts == 1);
    CHECK(failed->status == models::ShotStatus::DRAFT_DONE);

    int failed_frames = 0;
    for (const auto& o : report.frames) {
        if (!o.ok) ++failed_frames;
    }
    CHECK(failed_frames == 1);
}

TEST_CASE("跑全流程时成片档不吃 force") {
    // 照抄 Python：run() 给成片档的是 force=False。
    // 草稿失败的镜头状态没推进，成片档该跳过它——拿一个没渲出来的草稿
    // 去出成片，出来的是另一段片子，混在成片目录里最难发现。
    const auto store = make_store("成片force", 3);
    Recorder rec;
    rec.video_fails = {"ep01_sh002"};
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.force = true;

    run_it(store, opts, rec, tok);

    const auto finals = rec.ids_of("final");
    CHECK(finals.size() == 2);
    CHECK(std::find(finals.begin(), finals.end(), "ep01_sh002") == finals.end());

    SUBCASE("单跑成片阶段时才吃 force") {
        // 那时候是用户明确说"这一段重来"，包括状态没推进的那几镜。
        Recorder rec2;
        pipeline::CancelToken tok2;
        pipeline::RunOptions o2;
        o2.episode_id = "ep01";
        o2.only = {pipeline::Stage::Final};
        o2.force = true;
        run_it(store, o2, rec2, tok2);
        CHECK(rec2.ids_of("final").size() == 3);
    }
}

TEST_CASE("跑过一遍之后再跑就跳过") {
    // 断点续跑靠的就是这个。跳不掉的话点第二次会把整集重算一遍，
    // 而用户点第二次通常是因为第一次中途停了。
    const auto store = make_store("续跑", 2);
    {
        Recorder rec;
        pipeline::CancelToken tok;
        pipeline::RunOptions opts;
        opts.episode_id = "ep01";
        run_it(store, opts, rec, tok);
    }

    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    std::vector<nlohmann::json> msgs;
    const auto report = run_it(store, opts, rec, tok, &msgs);

    CHECK(rec.calls.empty());
    CHECK(report.frames.empty());
    CHECK(report.draft.empty());
    CHECK(report.final_.empty());

    SUBCASE("而且要说一声跳过了，不能一声不吭") {
        // 一声不吭的话用户看到的是"点了开始，立刻就完成了"，
        // 分不清是续跑跳过了还是根本没跑起来。
        bool said = false;
        for (const auto& m : msgs) {
            const std::string s = m.dump();
            if (s.find("跳过") != std::string::npos) said = true;
        }
        CHECK(said);
    }
}

TEST_CASE("force 无视状态全部重跑") {
    const auto store = make_store("重跑", 2);
    {
        Recorder rec;
        pipeline::CancelToken tok;
        pipeline::RunOptions opts;
        opts.episode_id = "ep01";
        run_it(store, opts, rec, tok);
    }

    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.force = true;
    run_it(store, opts, rec, tok);

    CHECK(rec.ids_of("frame").size() == 2);
    CHECK(rec.ids_of("draft").size() == 2);
}

TEST_CASE("取消之后不再进下一个阶段") {
    // 点了停止还接着跑下一个阶段的话，"停止"这个按钮就是不管用的——
    // 而下一个阶段可能要几十分钟。
    const auto store = make_store("取消", 3);
    Recorder rec;
    pipeline::CancelToken tok;
    tok.request();   // 一开始就取消
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";

    const auto report = run_it(store, opts, rec, tok);

    CHECK(rec.calls.empty());
    CHECK(report.frames.empty());
    CHECK(report.draft.empty());
    CHECK(report.final_.empty());
}

TEST_CASE("剧集不存在或者没有分镜表时说清楚") {
    const auto store = make_store("缺集", 1);
    Recorder rec;
    pipeline::CancelToken tok;

    SUBCASE("没这一集") {
        pipeline::RunOptions opts;
        opts.episode_id = "ep99";
        try {
            run_it(store, opts, rec, tok);
            FAIL("该抛异常");
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("ep99") != std::string::npos);
        }
    }

    SUBCASE("有这一集但没分镜") {
        models::Project p = store.load_project();
        models::Episode empty;
        empty.episode_id = "ep02";
        p.episodes.push_back(empty);
        store.save_project(p);

        pipeline::RunOptions opts;
        opts.episode_id = "ep02";
        try {
            run_it(store, opts, rec, tok);
            FAIL("该抛异常");
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("分镜表") != std::string::npos);
        }
    }
}

TEST_CASE("首帧的完成消息带上失败了几个") {
    // 只说"完成 N 个"的话，用户不知道另外几个去哪了。
    // 而失败的那几镜画面会明显差一截，事后翻日志才知道原因。
    const auto store = make_store("失败计数", 3);
    Recorder rec;
    rec.frame_fails = {"ep01_sh001", "ep01_sh003"};
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;
    std::vector<nlohmann::json> msgs;

    run_it(store, opts, rec, tok, &msgs);

    bool found = false;
    for (const auto& m : msgs) {
        const std::string s = m.dump();
        if (s.find("首帧完成 1 个") != std::string::npos &&
            s.find("失败 2 个") != std::string::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("开始渲染时报的是档位表里的分辨率") {
    // 报缩放后的数字会让用户以为设置没生效：他在设置页填的是 640x352，
    // 日志里写着 448x768，看起来像是被改掉了。
    const auto store = make_store("分辨率消息", 1);
    Recorder rec;
    pipeline::CancelToken tok;
    pipeline::RunOptions opts;
    opts.episode_id = "ep01";
    opts.skip_final = true;
    std::vector<nlohmann::json> msgs;

    run_it(store, opts, rec, tok, &msgs);

    const auto spec = make_profile().tiers.at(models::Tier::DRAFT);
    const std::string want = std::to_string(spec.width) + "x" +
                             std::to_string(spec.height) + " " +
                             std::to_string(spec.steps) + " 步";
    bool found = false;
    for (const auto& m : msgs) {
        if (m.dump().find(want) != std::string::npos) found = true;
    }
    CAPTURE(want);
    CHECK(found);
}

TEST_CASE("阶段名的字符串两边要能互转") {
    // 这几个串是 /api/run 的请求字段，前端直接传过来的。
    for (const auto* name :
         {"audio", "frames", "draft", "final", "assemble"}) {
        pipeline::Stage s{};
        CAPTURE(name);
        REQUIRE(pipeline::stage_from_string(name, s));
        CHECK(std::string(pipeline::to_string(s)) == name);
    }

    SUBCASE("不认识的返回 false，不是抛异常也不是默认成第一个") {
        // 默认成第一个的话，前端传错一个词，跑的是另一个阶段。
        pipeline::Stage s = pipeline::Stage::Final;
        CHECK_FALSE(pipeline::stage_from_string("render", s));
        CHECK(s == pipeline::Stage::Final);   // 没被改掉
    }
}
