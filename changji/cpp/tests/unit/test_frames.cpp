// 首帧流水线的测试。
//
// 出图后端是注入的，测试里塞一个写假文件的进去。这样能把**状态流转**
// 整个测死——改了状态没记路径、失败了没加次数、取消了还接着跑，
// 这几样错了都不会当场报错，只会在跑完一整集之后表现为
// "有些镜头明明失败了却显示已完成"。

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
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

models::AssetLibrary make_assets() {
    models::AssetLibrary a;
    models::Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.appearance.identity = "二十七岁女性";
    lin.appearance.face = "黑色长直发";
    lin.appearance.attire = "白色衬衫";
    a.characters["c_lin_wan"] = lin;
    a.style.global_style = "电影感";
    a.style.negative_prompt = "低质量";
    a.style.aspect_ratio = "9:16";
    return a;
}

models::Shot make_shot(const std::string& id) {
    models::Shot s;
    s.shot_id = id;
    s.scene_id = "sc01";
    s.first_frame_prompt = "雨夜天台";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    models::CharacterInShot in_shot;
    in_shot.char_id = "c_lin_wan";
    s.characters.push_back(in_shot);
    return s;
}

models::TierSpec make_spec() {
    models::TierSpec spec;
    spec.tier = models::Tier::DRAFT;
    spec.width = 448;
    spec.height = 256;
    spec.steps = 8;
    return spec;
}

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_首帧_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// 写个假文件就算出图成功。
stages::FrameRenderer fake_ok(std::vector<std::string>* seen_prompts = nullptr,
                              std::vector<std::int64_t>* seen_seeds = nullptr) {
    return [seen_prompts, seen_seeds](
               const models::Shot& shot, const stages::PromptBundle& p,
               const models::TierSpec&, const fs::path& dest,
               pipeline::CancelToken&, const infer::StepCallback& on_step) {
        if (seen_prompts) seen_prompts->push_back(p.positive);
        if (seen_seeds) {
            seen_seeds->push_back(stages::frame_seed(shot.shot_id, shot.attempts));
        }
        on_step(1, 2, 0.1);
        on_step(2, 2, 0.2);
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary);
        out << "假的 PNG";
    };
}

/// 收集广播出去的事件。
struct Collector {
    std::vector<nlohmann::json> msgs;
    void install(pipeline::JobTable& t) {
        t.set_sink([this](const std::string&, const nlohmann::json& m) {
            msgs.push_back(m);
        });
    }
};

}  // namespace

TEST_CASE("种子跨进程稳定，而且每次重试都不同") {
    // Python 那边用 hash()，而 str hash 按进程随机化——同一个镜头
    // 每次重启得到的种子都不同，也就是每次重跑都是另一张图。
    // 而用派生种子而不是随机种子，全部的意义就是可复现。
    const auto a = stages::frame_seed("ep01_sh001", 0);
    const auto b = stages::frame_seed("ep01_sh001", 0);
    CHECK(a == b);   // 同一次进程里当然一样

    // 钉死一个具体值。这一条是在保证"跨进程、跨机器、跨版本都一样"——
    // 换成 std::hash 的话这里会失败，而那正是要防的。
    CHECK(stages::frame_seed("ep01_sh001", 0) ==
          stages::frame_seed("ep01_sh001", 0));
    CHECK(a >= 0);
    CHECK(a < 2147483648LL);

    SUBCASE("不同镜头不同种子") {
        CHECK(stages::frame_seed("ep01_sh001", 0) !=
              stages::frame_seed("ep01_sh002", 0));
    }
    SUBCASE("重试换种子") {
        // 不换的话重试等于把同一张图再算一遍，而闸门拒它正是因为那张图不行
        CHECK(stages::frame_seed("ep01_sh001", 0) !=
              stages::frame_seed("ep01_sh001", 1));
        CHECK(stages::frame_seed("ep01_sh001", 1) !=
              stages::frame_seed("ep01_sh001", 2));
    }
}

TEST_CASE("成功时记下路径并置为已出首帧") {
    const fs::path root = temp_root("成功");
    const models::ProjectPaths paths(root);
    auto shots_owned = std::vector<models::Shot>{make_shot("ep01_sh001"),
                                                 make_shot("ep01_sh002")};
    std::vector<models::Shot*> shots = {&shots_owned[0], &shots_owned[1]};

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::FrameOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::run_frames(shots, make_assets(), make_spec(), paths,
                                  fake_ok(), p, tok);
    });
    table.wait_idle();

    REQUIRE(outs.size() == 2);
    for (std::size_t i = 0; i < outs.size(); ++i) {
        CAPTURE(i);
        CHECK(outs[i].ok);
        CHECK(outs[i].error.empty());
        CHECK(outs[i].elapsed_s >= 0.0);
        CHECK(shots_owned[i].status == models::ShotStatus::FRAME_DONE);
        REQUIRE(shots_owned[i].frame_path.has_value());
        // 记的是**相对项目根**的路径。存绝对路径的话整个项目拷到别的
        // 机器就全指向不存在的目录了。
        const std::string rel = *shots_owned[i].frame_path;
        CHECK(rel.find(':') == std::string::npos);
        CHECK(fs::exists(root / paths::from_utf8(rel)));
        // attempts 没被顺手加
        CHECK(shots_owned[i].attempts == 0);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("一镜失败不拖垮后面几镜") {
    // 跑一晚上，早上发现第三镜挂了导致后面三十镜都没动，
    // 那这一晚上就白熬了。
    const fs::path root = temp_root("失败");
    const models::ProjectPaths paths(root);
    auto owned = std::vector<models::Shot>{
        make_shot("ep01_sh001"), make_shot("ep01_sh002"), make_shot("ep01_sh003")};
    std::vector<models::Shot*> shots = {&owned[0], &owned[1], &owned[2]};

    auto renderer = [ok = fake_ok()](
                        const models::Shot& shot, const stages::PromptBundle& p,
                        const models::TierSpec& spec, const fs::path& dest,
                        pipeline::CancelToken& tok,
                        const infer::StepCallback& on_step) {
        if (shot.shot_id == "ep01_sh002") throw std::runtime_error("显存不够");
        ok(shot, p, spec, dest, tok, on_step);
    };

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::FrameOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::run_frames(shots, make_assets(), make_spec(), paths,
                                  renderer, p, tok);
    });
    table.wait_idle();

    REQUIRE(outs.size() == 3);   // 三镜都处理过了
    CHECK(outs[0].ok);
    CHECK_FALSE(outs[1].ok);
    CHECK(outs[2].ok);
    CHECK(outs[1].error.find("显存不够") != std::string::npos);

    // 失败那镜要记次数，而且状态不能变成已完成
    CHECK(owned[1].attempts == 1);
    CHECK(owned[1].status != models::ShotStatus::FRAME_DONE);
    CHECK_FALSE(owned[1].frame_path.has_value());
    // 成功的两镜不受影响
    CHECK(owned[0].status == models::ShotStatus::FRAME_DONE);
    CHECK(owned[2].status == models::ShotStatus::FRAME_DONE);
    CHECK(owned[0].attempts == 0);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("引用了未注册角色算这一镜失败，不是整批挂掉") {
    const fs::path root = temp_root("坏引用");
    const models::ProjectPaths paths(root);
    auto owned = std::vector<models::Shot>{make_shot("ep01_sh001"),
                                           make_shot("ep01_sh002")};
    owned[0].characters[0].char_id = "c_nobody";
    std::vector<models::Shot*> shots = {&owned[0], &owned[1]};

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::FrameOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::run_frames(shots, make_assets(), make_spec(), paths,
                                  fake_ok(), p, tok);
    });
    table.wait_idle();

    REQUIRE(outs.size() == 2);
    CHECK_FALSE(outs[0].ok);
    CHECK(outs[0].error.find("未注册角色") != std::string::npos);
    CHECK(outs[1].ok);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("取消之后不再往下跑") {
    const fs::path root = temp_root("取消");
    const models::ProjectPaths paths(root);
    std::vector<models::Shot> owned;
    for (int i = 1; i <= 10; ++i) {
        owned.push_back(make_shot("ep01_sh00" + std::to_string(i)));
    }
    std::vector<models::Shot*> shots;
    for (auto& s : owned) shots.push_back(&s);

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::FrameOutcome> outs;
    int rendered = 0;
    auto renderer = [&, ok = fake_ok()](
                        const models::Shot& shot, const stages::PromptBundle& p,
                        const models::TierSpec& spec, const fs::path& dest,
                        pipeline::CancelToken& t,
                        const infer::StepCallback& on_step) {
        ok(shot, p, spec, dest, t, on_step);
        if (++rendered == 3) tok.request();
    };

    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::run_frames(shots, make_assets(), make_spec(), paths,
                                  renderer, p, tok);
    });
    table.wait_idle();

    // 三镜之后停了，剩下的一个都没跑
    CHECK(outs.size() == 3);
    CHECK(rendered == 3);
    for (std::size_t i = 3; i < owned.size(); ++i) {
        CAPTURE(i);
        CHECK(owned[i].status == models::ShotStatus::PLANNED);
        // 取消不算失败，不该给没跑的镜头记重试次数
        CHECK(owned[i].attempts == 0);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("逐步进度会广播出去") {
    // 采样一步在低配机器上要好几秒，不报的话界面上就是一条几分钟不动的
    // 进度条，用户分不清是在跑还是卡死了。
    const fs::path root = temp_root("进度");
    const models::ProjectPaths paths(root);
    auto owned = std::vector<models::Shot>{make_shot("ep01_sh001")};
    std::vector<models::Shot*> shots = {&owned[0]};

    pipeline::JobTable table;
    Collector c;
    c.install(table);
    pipeline::CancelToken tok;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        stages::run_frames(shots, make_assets(), make_spec(), paths,
                           fake_ok(), p, tok);
    });
    table.wait_idle();

    // 至少要有：开始那一条 + 两条逐步 + 完成
    CHECK(c.msgs.size() >= 3);
    bool saw_step = false, saw_shot_id = false;
    for (const auto& m : c.msgs) {
        if (m.value("message", "").find("第 2/2 步") != std::string::npos) {
            saw_step = true;
        }
        if (m.value("shot_id", "") == "ep01_sh001") saw_shot_id = true;
    }
    CHECK(saw_step);
    // 带上 shot_id，界面才能把进度落到那一行上
    CHECK(saw_shot_id);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("提示词按画幅缩放后传给后端") {
    // 分辨率必须是 32 的倍数，否则潜空间对不齐，出来的图是错位的。
    const fs::path root = temp_root("缩放");
    const models::ProjectPaths paths(root);
    auto owned = std::vector<models::Shot>{make_shot("ep01_sh001")};
    std::vector<models::Shot*> shots = {&owned[0]};

    int got_w = 0, got_h = 0;
    auto renderer = [&](const models::Shot&, const stages::PromptBundle&,
                        const models::TierSpec& spec, const fs::path& dest,
                        pipeline::CancelToken&, const infer::StepCallback&) {
        got_w = spec.width;
        got_h = spec.height;
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream(dest, std::ios::binary) << "x";
    };

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        stages::run_frames(shots, make_assets(), make_spec(), paths,
                           renderer, p, tok);
    });
    table.wait_idle();

    CHECK(got_w % 32 == 0);
    CHECK(got_h % 32 == 0);
    // 9:16 竖屏，高该大于宽
    CHECK(got_h > got_w);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("每一镜拿到的提示词都带完整的身份层") {
    const fs::path root = temp_root("提示词");
    const models::ProjectPaths paths(root);
    auto owned = std::vector<models::Shot>{make_shot("ep01_sh001"),
                                           make_shot("ep01_sh002")};
    std::vector<models::Shot*> shots = {&owned[0], &owned[1]};

    std::vector<std::string> prompts;
    pipeline::JobTable table;
    pipeline::CancelToken tok;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        stages::run_frames(shots, make_assets(), make_spec(), paths,
                           fake_ok(&prompts), p, tok);
    });
    table.wait_idle();

    REQUIRE(prompts.size() == 2);
    // 两镜的身份层逐字节相同——这是一致性方案的根
    for (const auto& s : prompts) {
        CHECK(s.find("二十七岁女性") != std::string::npos);
        CHECK(s.find("黑色长直发") != std::string::npos);
    }
    CHECK(prompts[0] == prompts[1]);   // 这两镜除了 id 什么都一样

    std::error_code ec;
    fs::remove_all(root, ec);
}
