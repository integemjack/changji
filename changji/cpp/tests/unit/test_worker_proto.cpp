// 工作进程的协议。
//
// 这一层没有 Python 对应物，是纯粹的新逻辑，不走对拍。
//
// **单独抽出来就是为了能在这儿测。** 接上真模型之后一个用例要跑几分钟，
// 协议层的 bug 没法反复撞——和 `scheduler` 把加载卸载做成注入回调
// 是同一个理由。

#include <doctest/doctest.h>

#include <set>

#include <string>

#include "infer/worker_farm.hpp"
#include "infer/worker_proto.hpp"

using namespace changji;

namespace {

infer::Task sample_frame() {
    infer::Task t;
    t.kind = infer::TaskKind::Frame;
    t.shot_id = "ep01_sh001";
    t.prompts.positive = "雨夜天台，霓虹";
    t.prompts.negative = "低质量";
    t.prompts.reference_images = {"assets/lin.png", "assets/rooftop.png"};
    t.spec.tier = models::Tier::DRAFT;
    t.spec.width = 448;
    t.spec.height = 256;
    t.spec.steps = 8;
    t.dest = R"(C:\项目\frames\ep01_sh001.png)";
    t.seed = 1234567890123LL;
    return t;
}

}  // namespace

TEST_CASE("任务往返一圈不丢东西") {
    const auto t = sample_frame();
    const auto back = infer::task_from_json(infer::to_json(t));

    CHECK(back.kind == t.kind);
    CHECK(back.shot_id == t.shot_id);
    CHECK(back.prompts.positive == t.prompts.positive);
    CHECK(back.prompts.negative == t.prompts.negative);
    CHECK(back.prompts.reference_images == t.prompts.reference_images);
    CHECK(back.spec.width == t.spec.width);
    CHECK(back.spec.height == t.spec.height);
    CHECK(back.spec.steps == t.spec.steps);
    // **中文路径和反斜杠**。这个项目在这上面栽过好几次，
    // 而任务是要跨进程传的，路径错了产物就写到别处去了。
    CHECK(back.dest == t.dest);
    CHECK(back.seed == t.seed);
}

TEST_CASE("出片任务的那几项也要活着回来") {
    infer::Task t;
    t.kind = infer::TaskKind::Video;
    t.shot_id = "ep01_sh002";
    t.dest = "/tmp/x.mp4";
    t.frames = 25;
    t.motion = "推近";
    t.style_line = models::StyleLine::ANIME;
    t.tier = models::Tier::FINAL;
    t.start_image = "/tmp/first.png";
    t.seed = 42;

    const auto back = infer::task_from_json(infer::to_json(t));
    CHECK(back.kind == infer::TaskKind::Video);
    CHECK(back.frames == 25);
    CHECK(back.motion == "推近");
    // **style_line 必须传对。** 它决定提示词用哪个分隔符，
    // 而提示词是要和 Python 逐字节对上的——方案里记过这个坑：
    // 两个视频后端都写死过 REALISTIC，动画线的项目就差一个分隔符。
    CHECK(back.style_line == models::StyleLine::ANIME);
    CHECK(back.tier == models::Tier::FINAL);
    REQUIRE(back.start_image.has_value());
    CHECK(*back.start_image == "/tmp/first.png");
}

TEST_CASE("没有首帧图的任务不该凭空多出一个") {
    auto t = sample_frame();
    t.start_image.reset();
    const auto j = infer::to_json(t);
    CHECK_FALSE(j.contains("start_image"));
    CHECK_FALSE(infer::task_from_json(j).start_image.has_value());
}

TEST_CASE("缺必填字段要当场报，不能用默认值糊过去") {
    // 一个没带 dest 的任务默默写到当前目录去，比当场报错难查得多。
    auto j = infer::to_json(sample_frame());

    for (const char* key : {"kind", "shot_id", "dest", "seed", "spec",
                            "prompts"}) {
        CAPTURE(key);
        auto broken = j;
        broken.erase(key);
        CHECK_THROWS_AS(infer::task_from_json(broken), std::runtime_error);
    }
}

TEST_CASE("kind 只认 frame 和 video") {
    auto j = infer::to_json(sample_frame());
    j["kind"] = "随便写的";
    CHECK_THROWS_AS(infer::task_from_json(j), std::runtime_error);

    CHECK(infer::task_kind_from("frame").has_value());
    CHECK(infer::task_kind_from("video").has_value());
    CHECK_FALSE(infer::task_kind_from("").has_value());
}

TEST_CASE("结果和进度也往返得回来") {
    infer::TaskResult r;
    r.ok = false;
    r.error = "出首帧失败：显存不够";
    const auto rb = infer::task_result_from_json(infer::to_json(r));
    CHECK_FALSE(rb.ok);
    // 错误原因要能直接给用户看——它会变成事件流里那条 warn
    CHECK(rb.error == r.error);

    infer::TaskProgress p;
    p.state = "running";
    p.step = 3;
    p.steps = 8;
    p.loading = true;
    const auto pb = infer::task_progress_from_json(infer::to_json(p));
    CHECK(pb.state == "running");
    CHECK(pb.step == 3);
    CHECK(pb.steps == 8);
    // 加载和采样要分得开，不然进度会从 1927/1927 跳回 1/8
    CHECK(pb.loading);
    CHECK_FALSE(pb.result.has_value());
}

TEST_CASE("种子必须由协调者给，不能让工作进程自己算") {
    // 工作进程不知道 attempts，自己算出来的种子和串行跑的不一样。
    // 那样并行就不是"更快"，是"结果变了"——而这一点没有任何报错会提醒你。
    auto j = infer::to_json(sample_frame());
    j.erase("seed");
    CHECK_THROWS_AS(infer::task_from_json(j), std::runtime_error);
}

TEST_CASE("多卡自动拉起：端口按卡号排，不会撞") {
    // 端口撞了的表现是"某张卡的工作进程起不来"，而日志里只有一句连不上，
    // 看不出是端口规则算错了。所以这条规则单独拿出来测。
    CHECK(infer::worker_port_for(9001, 0) == 9001);
    CHECK(infer::worker_port_for(9001, 7) == 9008);
    // 八张卡两两不同
    std::set<int> seen;
    for (int g = 0; g < 8; ++g) seen.insert(infer::worker_port_for(9001, g));
    CHECK(seen.size() == 8);
}

TEST_CASE("多卡自动拉起：这四种情况都不该动手") {
    // **不动手时要退回单卡进程内跑**，不是报错——多卡拉不起来该继续干活。
    models::HardwareProfile two;
    models::GPUInfo g;
    g.count = 2;
    two.gpu = g;

    SUBCASE("用户自己填了 endpoints（包括跨机）") {
        config::Settings s;
        s.workers.endpoints = {"http://别的机器:9001"};
        CHECK(infer::WorkerFarm::start(s, two) == nullptr);
    }
    SUBCASE("显式关掉了") {
        config::Settings s;
        s.workers.auto_spawn = false;
        CHECK(infer::WorkerFarm::start(s, two) == nullptr);
    }
    SUBCASE("只有一张卡：进程内跑更省事") {
        config::Settings s;
        models::HardwareProfile one;
        models::GPUInfo g1;
        g1.count = 1;
        one.gpu = g1;
        CHECK(infer::WorkerFarm::start(s, one) == nullptr);
    }
    SUBCASE("压根没探到显卡") {
        config::Settings s;
        models::HardwareProfile none;
        CHECK(infer::WorkerFarm::start(s, none) == nullptr);
    }
}
