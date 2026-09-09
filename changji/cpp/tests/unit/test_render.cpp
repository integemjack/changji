// 图生视频那一层的测试。
//
// 出片后端注入，测的是计划和状态：时长换多少帧、种子怎么来、
// 出完之后镜头是什么状态、首帧文件不在时怎么办。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "gates/checks.hpp"
#include "models/character.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

models::AssetLibrary make_assets(
    models::StyleLine line = models::StyleLine::REALISTIC) {
    models::AssetLibrary a;
    models::Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.appearance.identity = "二十七岁女性";
    lin.appearance.face = "黑色长直发";
    lin.appearance.attire = "白色衬衫";
    a.characters["c_lin_wan"] = lin;
    a.style.style_line = line;
    a.style.global_style = "电影感";
    a.style.negative_prompt = "低质量";
    a.style.aspect_ratio = "9:16";
    return a;
}

models::Shot make_shot(const std::string& id, double dur = 5.0) {
    models::Shot s;
    s.shot_id = id;
    s.scene_id = "sc01";
    s.first_frame_prompt = "雨夜天台";
    s.motion_prompt = "雨丝斜掠";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    s.camera_move = models::CameraMove::PUSH_IN;
    s.duration_s = dur;
    models::CharacterInShot c;
    c.char_id = "c_lin_wan";
    c.action = "转身";
    s.characters.push_back(c);
    return s;
}

models::TierSpec make_spec(models::Tier tier = models::Tier::DRAFT) {
    models::TierSpec spec;
    spec.tier = tier;
    spec.width = 448;
    spec.height = 256;
    spec.steps = 8;
    return spec;
}

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_出片_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

stages::VideoRenderer fake_ok(
    std::vector<std::optional<fs::path>>* seen_starts = nullptr,
    std::vector<int>* seen_frames = nullptr) {
    return [seen_starts, seen_frames](
               const models::Shot&, const stages::RenderPlan& plan,
               const std::optional<fs::path>& start, const fs::path& dest,
               pipeline::CancelToken&, const infer::StepCallback& on_step) {
        if (seen_starts) seen_starts->push_back(start);
        if (seen_frames) seen_frames->push_back(plan.frames);
        on_step(1, 2, 0.1, false);
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream(dest, std::ios::binary) << "假的 mp4";
    };
}

}  // namespace

TEST_CASE("时长换帧数：必须是 4n+1，而且不超过上限") {
    // 4n+1 是 Wan 的硬要求。给别的数它会自己截，而截的位置不告诉你。
    for (const double d : {0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 5.04, 8.0, 100.0}) {
        CAPTURE(d);
        const int f = stages::frames_for(d);
        CHECK((f - 1) % 4 == 0);
        CHECK(f >= 5);                    // 至少 4*1+1
        CHECK(f <= stages::kMaxFrames);
    }

    SUBCASE("常见档位的具体值") {
        CHECK(stages::frames_for(2.0) == 49);
        CHECK(stages::frames_for(3.0) == 73);
        CHECK(stages::frames_for(4.0) == 97);
        // 5 秒是 121，正好是上限
        CHECK(stages::frames_for(5.0) == 121);
    }

    SUBCASE("超出上限的被截住，不是让模型自己截") {
        // 超过约 100 帧会在末帧往回跑，出现乒乓现象。
        // 这是模型本身的限制，不是可调参数。
        CHECK(stages::frames_for(10.0) == stages::kMaxFrames);
        CHECK(stages::frames_for(600.0) == stages::kMaxFrames);
    }
}

TEST_CASE("视频种子和首帧种子不一样") {
    // 用同一个偏移的话，首帧撞上一个坏种子时视频也会撞上同一个，
    // 重试也躲不开。
    CHECK(stages::render_seed("ep01_sh001", 0) !=
          stages::frame_seed("ep01_sh001", 0));

    SUBCASE("同样跨进程稳定") {
        CHECK(stages::render_seed("ep01_sh001", 0) ==
              stages::render_seed("ep01_sh001", 0));
        CHECK(stages::render_seed("ep01_sh001", 0) >= 0);
        CHECK(stages::render_seed("ep01_sh001", 0) < 2147483648LL);
    }
    SUBCASE("重试换种子") {
        CHECK(stages::render_seed("x", 0) != stages::render_seed("x", 1));
    }
    SUBCASE("不同镜头不同种子") {
        CHECK(stages::render_seed("a", 0) != stages::render_seed("b", 0));
    }
}

TEST_CASE("视频提示词是画面加运动") {
    // 只给运动描述的话，模型不知道推的是谁。
    const auto a = make_assets();
    const stages::PromptComposer composer(a);
    const auto shot = make_shot("ep01_sh001");
    const auto plan = stages::make_plan(shot, make_spec(), composer, "9:16");

    const std::string v = stages::video_positive(plan);
    CHECK(v.find("二十七岁女性") != std::string::npos);      // 画面
    CHECK(v.find("镜头缓慢推近") != std::string::npos);      // 运动
    CHECK(v.find("雨丝斜掠") != std::string::npos);
    // 画面在前，运动在后
    CHECK(v.find("二十七岁女性") < v.find("镜头缓慢推近"));

    SUBCASE("动画线用半角逗号加空格，写实线用全角逗号") {
        // **原来这里是错的。** 两个视频后端（comfy/renderers.cpp 和
        // infer/sd_video.cpp）都写死了 REALISTIC，所以动画线的项目
        // 拼出来是"画面，运动"，而 Python 那边是"画面, 运动"。
        //
        // Python 走的是 `self.composer._sep`——composer 是从资产库
        // 建的，天然知道风格线。C++ 把后端抽成了函数对象，函数对象
        // 拿不到资产库，于是"传什么风格线"变成了调用方的责任，
        // 而两个调用方都传错了。
        //
        // 提示词是**要逐字节对得上**的，一个分隔符也算。所以修法不是
        // 把两处常量改对，是把 style_line 放进 RenderPlan——
        // 让它没法传错。
        const auto anime = make_assets(models::StyleLine::ANIME);
        const stages::PromptComposer ac(anime);
        const auto ap = stages::make_plan(shot, make_spec(), ac, "9:16");
        const std::string av = stages::video_positive(ap);

        REQUIRE_FALSE(ap.motion.empty());
        CHECK(av.find(", ") != std::string::npos);
        CHECK(av.find(ap.prompts.positive + ", " + ap.motion) == 0);

        // 写实线仍是全角逗号，没被顺手改掉
        CHECK(v.find(plan.prompts.positive + "，" + plan.motion) == 0);
    }

    SUBCASE("style_line 跟着资产库走，不是默认值") {
        // make_plan 忘了填这一行的话，动画线的计划会拿到默认的 REALISTIC，
        // 上面那条就白测了——它测的是 video_positive，不是 make_plan。
        const auto anime = make_assets(models::StyleLine::ANIME);
        const stages::PromptComposer ac(anime);
        CHECK(ac.style_line() == models::StyleLine::ANIME);
        CHECK(stages::make_plan(shot, make_spec(), ac, "9:16").style_line ==
              models::StyleLine::ANIME);
        CHECK(plan.style_line == models::StyleLine::REALISTIC);
    }

    SUBCASE("没有运动描述时不留尾巴") {
        stages::RenderPlan p = plan;
        p.motion.clear();
        const std::string s =
            stages::video_positive(p);
        CHECK(s == p.prompts.positive);
        CHECK(s.rfind("，") != s.size() - 3);
    }
}

TEST_CASE("成功时按档位置状态") {
    // 草稿档的片子当成片发出去，用户会以为模型质量就这样。
    for (const auto tier : {models::Tier::DRAFT, models::Tier::FINAL}) {
        CAPTURE(static_cast<int>(tier));
        const fs::path root = temp_root("状态");
        const models::ProjectPaths paths(root);
        auto owned = std::vector<models::Shot>{make_shot("ep01_sh001")};
        std::vector<models::Shot*> shots = {&owned[0]};

        pipeline::JobTable table;
        pipeline::CancelToken tok;
        std::vector<stages::RenderOutcome> outs;
        table.start(pipeline::JobKind::Run, "ep01",
                    [&](pipeline::JobProgress& p) {
                        outs = stages::render_batch(shots, make_assets(),
                                                    make_spec(tier), paths,
                                                    fake_ok(), p, tok);
                    });
        table.wait_idle();

        REQUIRE(outs.size() == 1);
        CHECK(outs[0].ok);
        CHECK(owned[0].status == (tier == models::Tier::FINAL
                                      ? models::ShotStatus::FINAL_DONE
                                      : models::ShotStatus::DRAFT_DONE));
        REQUIRE(owned[0].video_path.has_value());
        // 相对路径，而且按档位分了目录
        CHECK(owned[0].video_path->find(':') == std::string::npos);
        CHECK(owned[0].video_path->find(
                  tier == models::Tier::FINAL ? "final" : "draft") !=
              std::string::npos);

        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("首帧当起点传给后端") {
    const fs::path root = temp_root("首帧");
    const models::ProjectPaths paths(root);
    paths.ensure();
    auto owned = std::vector<models::Shot>{make_shot("ep01_sh001")};
    // 造一张真的首帧
    const fs::path frame = paths.frames() / "ep01_sh001.png";
    std::error_code ec;
    fs::create_directories(frame.parent_path(), ec);
    std::ofstream(frame, std::ios::binary) << "假的 PNG";
    owned[0].frame_path = paths.rel(frame);

    std::vector<models::Shot*> shots = {&owned[0]};
    std::vector<std::optional<fs::path>> starts;

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        stages::render_batch(shots, make_assets(), make_spec(), paths,
                             fake_ok(&starts), p, tok);
    });
    table.wait_idle();

    REQUIRE(starts.size() == 1);
    REQUIRE(starts[0].has_value());
    CHECK(fs::equivalent(*starts[0], frame));

    fs::remove_all(root, ec);
}

TEST_CASE("记着首帧但文件不在时退回纯文生，并且说出来") {
    // 直接把不存在的路径喂给后端的话，报的错是"读不了参考图"——
    // 而真正的问题是首帧那一步没跑或者文件被删了。
    const fs::path root = temp_root("缺首帧");
    const models::ProjectPaths paths(root);
    paths.ensure();
    auto owned = std::vector<models::Shot>{make_shot("ep01_sh001")};
    owned[0].frame_path = "frames/根本不存在.png";
    std::vector<models::Shot*> shots = {&owned[0]};

    std::vector<std::optional<fs::path>> starts;
    std::vector<nlohmann::json> msgs;
    pipeline::JobTable table;
    table.set_sink([&](const std::string&, const nlohmann::json& m) {
        msgs.push_back(m);
    });
    pipeline::CancelToken tok;
    std::vector<stages::RenderOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::render_batch(shots, make_assets(), make_spec(), paths,
                                    fake_ok(&starts), p, tok);
    });
    table.wait_idle();

    // 照样跑完，只是没有起点
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].ok);
    REQUIRE(starts.size() == 1);
    CHECK_FALSE(starts[0].has_value());

    // 而且要说出来。默默退回的话，用户拿到一批一致性很差的片子，
    // 完全不知道是因为首帧没接上。
    bool warned = false;
    for (const auto& m : msgs) {
        if (m.value("message", "").find("退回纯文生视频") != std::string::npos) {
            warned = true;
        }
    }
    CHECK(warned);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("一镜失败不拖垮后面几镜") {
    const fs::path root = temp_root("失败");
    const models::ProjectPaths paths(root);
    auto owned = std::vector<models::Shot>{
        make_shot("ep01_sh001"), make_shot("ep01_sh002"), make_shot("ep01_sh003")};
    std::vector<models::Shot*> shots = {&owned[0], &owned[1], &owned[2]};

    auto renderer = [ok = fake_ok()](
                        const models::Shot& shot, const stages::RenderPlan& plan,
                        const std::optional<fs::path>& start,
                        const fs::path& dest, pipeline::CancelToken& tok,
                        const infer::StepCallback& on_step) {
        if (shot.shot_id == "ep01_sh002") throw std::runtime_error("显存不够");
        ok(shot, plan, start, dest, tok, on_step);
    };

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::RenderOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::render_batch(shots, make_assets(), make_spec(), paths,
                                    renderer, p, tok);
    });
    table.wait_idle();

    REQUIRE(outs.size() == 3);
    CHECK(outs[0].ok);
    CHECK_FALSE(outs[1].ok);
    CHECK(outs[2].ok);
    // 失败那镜按 Python 的 _render_one 重试到 max_attempts_per_shot（默认 3）
    // 再降级——不是失败一次就放下。这几个数由 golden/render_loop.json
    // 那条"渲染连续失败到上限"从真 Python 上导出来的。
    CHECK(owned[1].attempts == 3);
    CHECK(owned[1].status == models::ShotStatus::FALLBACK);
    CHECK_FALSE(owned[1].video_path.has_value());
    // 另外两镜不受影响
    CHECK(owned[0].status == models::ShotStatus::DRAFT_DONE);
    CHECK(owned[2].status == models::ShotStatus::DRAFT_DONE);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("取消之后不再往下跑") {
    const fs::path root = temp_root("取消");
    const models::ProjectPaths paths(root);
    std::vector<models::Shot> owned;
    for (int i = 1; i <= 8; ++i) {
        owned.push_back(make_shot("ep01_sh00" + std::to_string(i)));
    }
    std::vector<models::Shot*> shots;
    for (auto& s : owned) shots.push_back(&s);

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    int rendered = 0;
    std::vector<stages::RenderOutcome> outs;
    auto renderer = [&, ok = fake_ok()](
                        const models::Shot& shot, const stages::RenderPlan& plan,
                        const std::optional<fs::path>& start,
                        const fs::path& dest, pipeline::CancelToken& t,
                        const infer::StepCallback& on_step) {
        ok(shot, plan, start, dest, t, on_step);
        if (++rendered == 2) tok.request();
    };

    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::render_batch(shots, make_assets(), make_spec(), paths,
                                    renderer, p, tok);
    });
    table.wait_idle();

    CHECK(outs.size() == 2);
    for (std::size_t i = 2; i < owned.size(); ++i) {
        CAPTURE(i);
        CHECK(owned[i].status == models::ShotStatus::PLANNED);
        CHECK(owned[i].attempts == 0);   // 取消不算失败
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("每一镜的帧数按它自己的时长算") {
    const fs::path root = temp_root("帧数");
    const models::ProjectPaths paths(root);
    auto owned = std::vector<models::Shot>{
        make_shot("ep01_sh001", 2.0), make_shot("ep01_sh002", 5.0)};
    std::vector<models::Shot*> shots = {&owned[0], &owned[1]};

    std::vector<int> frames;
    pipeline::JobTable table;
    pipeline::CancelToken tok;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        stages::render_batch(shots, make_assets(), make_spec(), paths,
                             fake_ok(nullptr, &frames), p, tok);
    });
    table.wait_idle();

    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == stages::frames_for(2.0));
    CHECK(frames[1] == stages::frames_for(5.0));
    CHECK(frames[0] < frames[1]);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// frames_for 的换算，和 Python 逐个比。
//
// **帧数错了 Wan 不会报错**——4n+1 是它的硬要求，给别的数它自己截，
// 而截在哪儿不告诉你。表现是"出来的片比预期短一点"，和"模型没跟上运动
// 描述"混在一起，几乎不可能联想到是这个换算错了。
//
// 这段换算里藏着一个很容易抄歪的地方：Python 的 `round()` 是
// **四舍六入五取偶**，不是学校教的四舍五入。`round(2.5)` 是 2 不是 3。
// 而 `(raw-1)/4` 落在 .5 上一点都不少见——raw=11 就是 2.5，
// 取偶给 9 帧，普通四舍五入给 13 帧。
//
// 上面那三条手写的期望值（2.0→49、3.0→73）是"我们以为应该是多少"，
// 不是"Python 给多少"。这一条才是后者。
// ---------------------------------------------------------------------------

TEST_CASE("frames_for 和 Python 逐个对得上（含取偶边界）") {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/render_math.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    nlohmann::json g;
    in >> g;

    const auto cases = g.at("cases");
    // 语料读空了循环一次都不转，而用例照样绿。
    REQUIRE(cases.size() > 60);

    for (const auto& c : cases) {
        const double d = c.at("duration_s").get<double>();
        const int fps = c.at("fps").get<int>();
        CAPTURE(d);
        CAPTURE(fps);
        CHECK(stages::frames_for(d, fps) == c.at("frames").get<int>());
    }
}

TEST_CASE("4n+1 这条硬要求，每一条都得满足") {
    // 上一条比的是"和 Python 一样"。这一条比的是**那个值本身合不合法**——
    // 两边一起抄错了的话，上一条照样绿。
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/render_math.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    nlohmann::json g;
    in >> g;

    const int cap = g.at("max_frames").get<int>();
    for (const auto& c : g.at("cases")) {
        const int f = c.at("frames").get<int>();
        CAPTURE(c.at("duration_s").get<double>());
        CHECK(f % 4 == 1);      // 4n+1
        CHECK(f >= 5);          // n 至少是 1
        CHECK(f <= cap);        // 不超上限
    }
}

// ---------------------------------------------------------------------------
// 并发出片时，状态改动还对不对。
//
// 出片是整条流水线最花时间的一环，多卡加速就加在这儿。但并行的只该是
// **渲染**——`video_path` / `status` / `attempts` 的写回必须仍然单线程、
// 按镜头原顺序，不然存盘时最后一个写的赢，而且一声不吭。
// ---------------------------------------------------------------------------

TEST_CASE("并发出片和串行的结果一模一样") {
    const models::ProjectPaths paths(temp_root("并发一致"));

    auto run_with = [&](int lanes) {
        std::vector<models::Shot> owned;
        for (int i = 0; i < 6; ++i) {
            owned.push_back(make_shot("sh" + std::to_string(i + 1),
                                      2.0 + 0.5 * i));
        }
        std::vector<models::Shot*> shots;
        for (auto& s : owned) shots.push_back(&s);

        pipeline::JobTable table;
        pipeline::CancelToken tok;
        table.start(pipeline::JobKind::Run, "ep01",
                    [&](pipeline::JobProgress& p) {
                        stages::render_batch(shots, make_assets(), make_spec(),
                                             paths, fake_ok(), p, tok, 24,
                                             lanes);
                    });
        table.wait_idle();

        std::vector<std::string> summary;
        for (const auto& s : owned) {
            summary.push_back(s.shot_id + "|" +
                              std::string(models::to_string(s.status)) + "|" +
                              std::to_string(s.attempts) + "|" +
                              s.video_path.value_or("(无)"));
        }
        return summary;
    };

    const auto serial = run_with(1);
    const auto parallel = run_with(4);

    REQUIRE(serial.size() == 6);
    // 并发只该改变"多快"，不该改变"是什么"。
    CHECK(serial == parallel);
}

TEST_CASE("并发出片失败时每镜的 attempts 都恰好停在上限") {
    // attempts 是闸门的重试计数。并发下要是写回漏了同步，同一镜可能被
    // 两个线程各加一遍，停在 6 而不是 3；或者被另一镜的写回盖掉停在 0。
    // 两种都不报错，只是断点续跑时重试次数不对。
    const models::ProjectPaths paths(temp_root("并发失败"));
    std::vector<models::Shot> owned;
    for (int i = 0; i < 5; ++i) {
        owned.push_back(make_shot("sh" + std::to_string(i + 1)));
    }
    std::vector<models::Shot*> shots;
    for (auto& s : owned) shots.push_back(&s);

    const stages::VideoRenderer always_fail =
        [](const models::Shot& shot, const stages::RenderPlan&,
           const std::optional<fs::path>&, const fs::path&,
           pipeline::CancelToken&, const infer::StepCallback&) {
            throw std::runtime_error(shot.shot_id + " 出片失败：造出来的错");
        };

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::RenderOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::render_batch(shots, make_assets(), make_spec(), paths,
                                    always_fail, p, tok, 24, 4);
    });
    table.wait_idle();

    CHECK(outs.size() == 5);
    for (const auto& s : owned) {
        CAPTURE(s.shot_id);
        CHECK(s.attempts == 3);                 // 不是 0，也不是 6
        CHECK(s.status == models::ShotStatus::FALLBACK);
        CHECK_FALSE(s.video_path.has_value());  // 失败不该留下路径
    }
}

TEST_CASE("并发出片每一镜都真的出了自己的那个文件") {
    // 派活要是算错了下标，可能两个线程领到同一镜、另一镜没人做——
    // 而 outcomes 的条数照样对得上，状态也照样是 draft_done。
    // 所以这里数的是**盘上的文件**。
    const models::ProjectPaths paths(temp_root("并发覆盖"));
    std::vector<models::Shot> owned;
    for (int i = 0; i < 7; ++i) {
        owned.push_back(make_shot("sh" + std::to_string(i + 1)));
    }
    std::vector<models::Shot*> shots;
    for (auto& s : owned) shots.push_back(&s);

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::RenderOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::render_batch(shots, make_assets(), make_spec(), paths,
                                    fake_ok(), p, tok, 24, 3);
    });
    table.wait_idle();

    REQUIRE(outs.size() == 7);
    for (const auto& s : owned) {
        CAPTURE(s.shot_id);
        REQUIRE(s.video_path.has_value());
        CHECK(fs::is_regular_file(paths.abs(*s.video_path)));
        CHECK(s.status == models::ShotStatus::DRAFT_DONE);
    }
}

// ---------------------------------------------------------------------------
// 渲染→过闸门→重试/退回/降级，和 Python 的 _render_one 逐条比。
//
// 语料 golden/render_loop.json 是 export_render_loop_golden.py 跑**真的**
// Python 循环导出来的：渲染器和 gate_video 按剧本出结果，decide_next 是真的。
// 这边用同一份剧本喂 render_batch，比镜头最后长什么样、渲染被叫了几次、
// 以及吐出来的 warn / gate / shot_done 序列。
//
// 这个循环以前在 C++ 里根本不存在——出完片直接置 DRAFT_DONE，
// gate_video 和 decide_next 只有测试在调。
// ---------------------------------------------------------------------------

namespace {

nlohmann::json render_loop_golden() {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/render_loop.json";
    std::ifstream in(paths::from_utf8(path), std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    return nlohmann::json::parse(in);
}

gates::Verdict verdict_from(const std::string& s) {
    if (s == "pass") return gates::Verdict::Pass;
    if (s == "retry") return gates::Verdict::Retry;
    if (s == "regress") return gates::Verdict::Regress;
    FAIL("剧本里不认识的闸门结果：" << s);
    return gates::Verdict::Pass;
}

}  // namespace

TEST_CASE("出片循环的每一条路和 Python 一样") {
    const nlohmann::json g = render_loop_golden();
    REQUIRE(g["cases"].size() >= 11);

    for (const auto& c : g["cases"]) {
        const std::string name = c["name"];
        CAPTURE(name);
        INFO(c["why"].get<std::string>());

        const bool is_final = c["tier"] == "final";
        const models::Tier tier = is_final ? models::Tier::FINAL
                                           : models::Tier::DRAFT;
        const fs::path root = temp_root("循环_" + std::to_string(&c - &g["cases"][0]));
        const models::ProjectPaths paths(root);

        models::Shot shot = make_shot("sh001", 3.0);
        shot.status = is_final ? models::ShotStatus::DRAFT_DONE
                               : models::ShotStatus::FRAME_DONE;
        shot.attempts = c["preset_attempts"];
        shot.gate_notes = c["preset_gate_notes"].get<std::vector<std::string>>();
        std::vector<models::Shot*> shots{&shot};

        // ---- 剧本 ----
        const auto render_script = c["render"].get<std::vector<std::string>>();
        const auto gate_script = c["gate"].get<std::vector<std::string>>();
        int render_calls = 0, gate_calls = 0;
        const auto pick = [](const std::vector<std::string>& v, int i) {
            REQUIRE_MESSAGE(!v.empty(), "剧本里没给这一步");
            return v[static_cast<std::size_t>(std::min<int>(i, static_cast<int>(v.size()) - 1))];
        };

        const stages::VideoRenderer renderer =
            [&](const models::Shot&, const stages::RenderPlan&,
                const std::optional<fs::path>&, const fs::path& dest,
                pipeline::CancelToken&, const infer::StepCallback&) {
                const int i = render_calls++;
                if (pick(render_script, i) == "fail") {
                    throw std::runtime_error("造出来的错 #" + std::to_string(i + 1));
                }
                std::error_code ec;
                fs::create_directories(dest.parent_path(), ec);
                std::ofstream(dest, std::ios::binary) << std::string(4096, 'x');
            };

        config::GateConfig gcfg;
        gcfg.max_attempts_per_shot = c["max_attempts_per_shot"];
        gcfg.fallback_on_exhausted = c["fallback_on_exhausted"];

        stages::GateHooks hooks;
        hooks.max_attempts = gcfg.max_attempts_per_shot;
        if (c["gates_enabled"].get<bool>()) {
            const std::string gate_name =
                std::string(models::to_string(tier)) + " 档闸门";
            hooks.check = [&, gate_name](const models::Shot& s, const fs::path&,
                                         const stages::RenderPlan&) {
                const int i = gate_calls++;
                gates::GateResult r;
                r.shot_id = s.shot_id;
                r.gate = gate_name;
                r.verdict = verdict_from(pick(gate_script, i));
                if (r.verdict == gates::Verdict::Retry) {
                    r.reasons = {"造出来的理由 #" + std::to_string(i + 1), "第二条理由"};
                } else if (r.verdict == gates::Verdict::Regress) {
                    r.reasons = {"重跑也没用的那种"};
                }
                return r;
            };
            hooks.decide = [&](const gates::GateResult& r, const models::Shot& s) {
                return gates::decide_next(r, s, gcfg);
            };
        }

        // ---- 跑 ----
        pipeline::JobTable table;
        std::vector<nlohmann::json> msgs;
        table.set_sink([&](const std::string&, const nlohmann::json& m) {
            msgs.push_back(m);
        });
        pipeline::CancelToken tok;
        std::vector<stages::RenderOutcome> outs;
        table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
            outs = stages::render_batch(shots, make_assets(), make_spec(tier),
                                        paths, renderer, p, tok, 24, 1, hooks);
        });
        table.wait_idle();

        // ---- 镜头最后长什么样 ----
        CHECK(std::string(models::to_string(shot.status)) == c["status"].get<std::string>());
        CHECK(shot.attempts == c["attempts"].get<int>());
        CHECK(shot.gate_notes == c["gate_notes"].get<std::vector<std::string>>());
        CHECK(shot.video_path.has_value() == c["has_video_path"].get<bool>());
        CHECK(render_calls == c["render_calls"].get<int>());
        CHECK(gate_calls == c["gate_calls"].get<int>());

        // ---- 事件序列 ----
        // Python 的 _render_one 只吐 warn / gate / shot_done；C++ 这层还会吐
        // progress 和 eta，那两种不在这个循环里，滤掉再比。
        //
        // **从快照里拿，不从 sink 拿。** sink 收到的是 WebSocket 那个形状，
        // kind 被压成了 type=progress/error——warn、gate、shot_done 到那儿
        // 全是 "progress"，分不出来。快照里的 events 才是 Event::to_json。
        // **先接住快照再遍历。** `for (auto& m : snapshot()["events"])` 是
        // 对临时对象的子对象取引用——临时在 init 语句结束就没了，循环
        // 跑在悬空引用上。C++23 之前不延长这种生命周期；这里表现为 0 条，
        // 不崩，所以特别难看出来。
        const nlohmann::json snap = table.snapshot(pipeline::JobKind::Run);
        std::vector<nlohmann::json> got;
        for (const auto& m : snap["events"]) {
            const std::string k = m.value("kind", "");
            if (k == "warn" || k == "gate" || k == "shot_done") {
                got.push_back({{"kind", k},
                               {"message", m.value("message", "")},
                               {"shot_id", m.value("shot_id", nlohmann::json())},
                               {"current", m.value("current", 0)},
                               {"total", m.value("total", 0)}});
            }
        }
        REQUIRE(got.size() == c["events"].size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CAPTURE(i);
            // MSVC 在 doctest 的宏里对 json==json 报 C7692（重写候选被同名 != 排除），
            // 所以比 dump 出来的字符串。语义一样，而且失败时打印出来更好读。
            const nlohmann::json& want = c["events"][i];
            CHECK(got[i]["kind"].dump() == want["kind"].dump());
            CHECK(got[i]["message"].dump() == want["message"].dump());
            CHECK(got[i]["shot_id"].dump() == want["shot_id"].dump());
            CHECK(got[i]["current"].dump() == want["current"].dump());
            CHECK(got[i]["total"].dump() == want["total"].dump());
        }

        std::error_code ec;
        fs::remove_all(root, ec);
    }
}
