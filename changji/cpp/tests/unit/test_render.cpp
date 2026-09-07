// 图生视频那一层的测试。
//
// 出片后端注入，测的是计划和状态：时长换多少帧、种子怎么来、
// 出完之后镜头是什么状态、首帧文件不在时怎么办。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
        on_step(1, 2, 0.1);
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

    const std::string v = stages::video_positive(plan, models::StyleLine::REALISTIC);
    CHECK(v.find("二十七岁女性") != std::string::npos);      // 画面
    CHECK(v.find("镜头缓慢推近") != std::string::npos);      // 运动
    CHECK(v.find("雨丝斜掠") != std::string::npos);
    // 画面在前，运动在后
    CHECK(v.find("二十七岁女性") < v.find("镜头缓慢推近"));

    SUBCASE("没有运动描述时不留尾巴") {
        stages::RenderPlan p = plan;
        p.motion.clear();
        const std::string s =
            stages::video_positive(p, models::StyleLine::REALISTIC);
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
    CHECK(owned[1].attempts == 1);
    CHECK(owned[1].status != models::ShotStatus::DRAFT_DONE);
    CHECK_FALSE(owned[1].video_path.has_value());

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
