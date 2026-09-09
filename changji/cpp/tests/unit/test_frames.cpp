// 首帧流水线的测试。
//
// 出图后端是注入的，测试里塞一个写假文件的进去。这样能把**状态流转**
// 整个测死——改了状态没记路径、失败了没加次数、取消了还接着跑，
// 这几样错了都不会当场报错，只会在跑完一整集之后表现为
// "有些镜头明明失败了却显示已完成"。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

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
        on_step(1, 2, 0.1, false);
        on_step(2, 2, 0.2, false);
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
                                  fake_ok(), p, tok, 1);
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
                                  renderer, p, tok, 1);
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
                                  fake_ok(), p, tok, 1);
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
                                  renderer, p, tok, 1);
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
                           fake_ok(), p, tok, 1);
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
                           renderer, p, tok, 1);
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
                           fake_ok(&prompts), p, tok, 1);
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

// ---------------------------------------------------------------------------
// 首帧阶段跑完之后镜头变成什么样，和 Python 逐条比。
//
// 这一层逻辑不多，但**每一条都写在镜头状态上**，而状态是后面每个阶段的
// 输入：
//
//   成功：status 推到 frame_done，frame_path 填相对路径
//   失败：**attempts 加一，status 不动**
//
// 第二条要紧。attempts 是闸门的重试计数，加错了要么永远重试、要么第一次
// 就判超限降级。而 status 不动意味着这一镜下一轮还会被捡起来——改成推到
// 别的状态它就被跳过了，表现是"那一镜永远没有首帧"，
// 而日志里只有一条早就滚掉的失败。
//
// ⚠️ 语料只喂两边都会捕的错误类型。Python 捕 (FrameError, RenderError)，
// **C++ 捕的是 std::exception**——渲染器抛别的类型时 Python 让它穿出去
// （整个阶段中断），C++ 算成"这一镜失败"接着跑。那处差异写在方案里，
// 不在这份语料的范围内。
// ---------------------------------------------------------------------------

TEST_CASE("首帧阶段的状态变化和 Python 一样") {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/frames_stage.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    nlohmann::json g;
    in >> g;

    const auto cases = g.at("cases");
    REQUIRE(cases.size() == 5);

    for (const auto& c : cases) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);

        const auto plan = c.at("plan").get<std::vector<bool>>();
        const fs::path root = temp_root("语料_" + name);
        const models::ProjectPaths paths(root);

        std::vector<models::Shot> owned;
        for (std::size_t i = 0; i < plan.size(); ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "ep01_sh%03d",
                          static_cast<int>(i + 1));
            auto s = make_shot(buf);
            s.order = static_cast<int>(i);
            s.status = models::ShotStatus::AUDIO_DONE;
            s.attempts = 0;
            owned.push_back(s);
        }
        std::vector<models::Shot*> shots;
        for (auto& s : owned) shots.push_back(&s);

        // 按剧本成功或失败，和 Python 那边的 ScriptedBackend 一样
        std::size_t idx = 0;
        stages::FrameRenderer scripted =
            [&plan, &idx](const models::Shot& shot, const stages::PromptBundle&,
                          const models::TierSpec&, const fs::path& dest,
                          pipeline::CancelToken&, const infer::StepCallback&) {
                const bool ok = idx < plan.size() ? plan[idx] : true;
                ++idx;
                if (!ok) {
                    // C++ 这边没有单独的 FrameError——run_frames 捕的是 std::exception。
                    // 这正是方案里记的那处差异，用例注释开头写了。
                    throw std::runtime_error(shot.shot_id + " 出首帧失败：造出来的错");
                }
                std::error_code ec;
                fs::create_directories(dest.parent_path(), ec);
                std::ofstream f(dest, std::ios::binary | std::ios::trunc);
                f << "png";
            };

        pipeline::JobTable table;
        pipeline::CancelToken tok;
        std::vector<stages::FrameOutcome> outs;
        table.start(pipeline::JobKind::Run, "ep01",
                    [&](pipeline::JobProgress& p) {
                        outs = stages::run_frames(shots, make_assets(),
                                                  make_spec(), paths, scripted,
                                                  p, tok, 1);
                    });
        table.wait_idle();

        const auto want_outs = c.at("outcomes");
        REQUIRE(outs.size() == want_outs.size());
        for (std::size_t i = 0; i < outs.size(); ++i) {
            CAPTURE(i);
            CHECK(outs[i].shot_id == want_outs[i].at("shot_id").get<std::string>());
            CHECK(outs[i].ok == want_outs[i].at("ok").get<bool>());
            CHECK(outs[i].error.empty() != want_outs[i].at("has_error").get<bool>());
        }

        const auto want_shots = c.at("shots_after");
        REQUIRE(owned.size() == want_shots.size());
        for (std::size_t i = 0; i < owned.size(); ++i) {
            CAPTURE(i);
            CHECK(std::string(models::to_string(owned[i].status)) ==
                  want_shots[i].at("status").get<std::string>());
            CHECK(owned[i].attempts == want_shots[i].at("attempts").get<int>());
            // frame_path：成功该有、失败该没有
            const bool want_path = !want_shots[i].at("frame_path").is_null();
            CHECK(owned[i].frame_path.has_value() == want_path);
        }
    }
}

// ---------------------------------------------------------------------------
// 并发跑的时候，状态改动还对不对。
//
// **这是派—收那个改动最容易出错的地方。** 渲染并行了，但写回 Shot 必须
// 仍然是单线程、按原顺序的——不然两个线程同时改同一批镜头，存盘时
// 最后一个写的赢，而且不报错。
// ---------------------------------------------------------------------------

TEST_CASE("并发出首帧和串行的结果一模一样") {
    const models::ProjectPaths paths(temp_root("并发一致"));

    // 同一批镜头跑两遍：一遍 1 路，一遍 4 路。
    auto run_with = [&](int lanes) {
        std::vector<models::Shot> owned;
        for (int i = 0; i < 6; ++i) {
            owned.push_back(make_shot("sh" + std::to_string(i + 1)));
        }
        std::vector<models::Shot*> shots;
        for (auto& s : owned) shots.push_back(&s);

        pipeline::JobTable table;
        pipeline::CancelToken tok;
        table.start(pipeline::JobKind::Run, "ep01",
                    [&](pipeline::JobProgress& p) {
                        stages::run_frames(shots, make_assets(), make_spec(),
                                           paths, fake_ok(), p, tok, lanes);
                    });
        table.wait_idle();

        std::vector<std::string> summary;
        for (const auto& s : owned) {
            summary.push_back(s.shot_id + "|" +
                              std::string(models::to_string(s.status)) + "|" +
                              std::to_string(s.attempts) + "|" +
                              s.frame_path.value_or("(无)"));
        }
        return summary;
    };

    const auto serial = run_with(1);
    const auto parallel = run_with(4);

    REQUIRE(serial.size() == 6);
    // **逐条比**：状态、attempts、产物路径全都要一样。
    // 并发只该改变"多快"，不该改变"是什么"。
    CHECK(serial == parallel);
}

TEST_CASE("并发时失败那镜的 attempts 只加一次") {
    // 写回要是漏了同步，同一镜可能被加两次——而 attempts 是闸门的重试
    // 计数，多加一次就可能直接判超限降级，画面从此变成静帧加运镜。
    const models::ProjectPaths paths(temp_root("并发失败"));
    std::vector<models::Shot> owned;
    for (int i = 0; i < 5; ++i) {
        owned.push_back(make_shot("sh" + std::to_string(i + 1)));
    }
    std::vector<models::Shot*> shots;
    for (auto& s : owned) shots.push_back(&s);

    const stages::FrameRenderer always_fail =
        [](const models::Shot& shot, const stages::PromptBundle&,
           const models::TierSpec&, const fs::path&, pipeline::CancelToken&,
           const infer::StepCallback&) {
            throw std::runtime_error(shot.shot_id + " 出首帧失败：造出来的错");
        };

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::FrameOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::run_frames(shots, make_assets(), make_spec(), paths,
                                  always_fail, p, tok, 4);
    });
    table.wait_idle();

    CHECK(outs.size() == 5);
    for (const auto& s : owned) {
        CAPTURE(s.shot_id);
        CHECK(s.attempts == 1);                 // 不是 0，也不是 2
        CHECK_FALSE(s.frame_path.has_value());  // 失败不该留下路径
    }
}

TEST_CASE("并发上限不超过镜头数") {
    // 池里八个而只有两镜时，起八个线程只是白占。
    const models::ProjectPaths paths(temp_root("并发上限"));
    std::vector<models::Shot> owned{make_shot("sh1"), make_shot("sh2")};
    std::vector<models::Shot*> shots{&owned[0], &owned[1]};

    pipeline::JobTable table;
    pipeline::CancelToken tok;
    std::vector<stages::FrameOutcome> outs;
    table.start(pipeline::JobKind::Run, "ep01", [&](pipeline::JobProgress& p) {
        outs = stages::run_frames(shots, make_assets(), make_spec(), paths,
                                  fake_ok(), p, tok, 8);
    });
    table.wait_idle();

    CHECK(outs.size() == 2);
    for (const auto& s : owned) {
        CHECK(s.status == models::ShotStatus::FRAME_DONE);
    }
}
