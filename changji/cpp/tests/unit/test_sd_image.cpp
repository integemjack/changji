// sd.cpp 出图那一层的测试。
//
// **测试目标里没链 sd.cpp**（CHANGJI_HAVE_SD 没定义），所以这里跑的是桩：
// SdContext::create 抛"没编进出图后端"。
//
// 这不是绕过测试，恰恰是要测的东西之一：那条路必须给出一句人话，
// 而不是段错误或者一个空指针。真正出图的验证要有模型文件才能做，
// 属于端到端那一档，不在单元测试里。
//
// 另外测的是**调度器接线**：槽注册了没有、加载失败时账退没退干净。
// 那部分不依赖 sd.cpp，是纯策略，而且恰恰是最容易写错的地方。

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <string>

#include "config/settings.hpp"
#include "infer/scheduler.hpp"
#include "infer/sd_backend.hpp"
#include "infer/sd_image.hpp"
#include "models/hardware.hpp"
#include "pipeline/jobs.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

config::Settings with_models() {
    config::Settings s;
    s.models.dir = "Z:/不存在的模型目录";
    s.models.image = "假的.gguf";
    return s;
}

models::HardwareProfile fake_profile(double vram_gb) {
    models::HardwareProfile p;
    p.vram_gb = vram_gb;
    p.detected = true;
    return p;
}

}  // namespace

TEST_CASE("没链 sd.cpp 时给的是人话不是崩溃") {
    // 这条在链了 sd.cpp 的构建里不成立，所以先问一句。
    // 测试目标现在不链，但将来可能会链（比如加端到端测试时）。
    if (infer::sd_available()) {
        MESSAGE("这个构建链了 sd.cpp，跳过桩的用例");
        return;
    }

    try {
        infer::SdContext::create(with_models(), 6.0, infer::ModelRole::Image);
        FAIL("该抛异常");
    } catch (const infer::SdError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        // 要说清两件事：为什么不行，以及能怎么办
        CHECK(msg.find("没有编进") != std::string::npos);
        CHECK(msg.find("推理服务") != std::string::npos);
    }
}

TEST_CASE("图像槽注册到调度器上") {
    // 注册这件事和链没链 sd.cpp 无关：没链的话 acquire 时 create 抛异常，
    // 调度器把账退干净。不注册的话调用方拿到的是"槽还没注册"，
    // 那句话对用户没有任何意义。
    infer::scheduler().evict_all();
    infer::register_sd_slots(with_models(), fake_profile(6.0));

    // 注册不等于加载
    CHECK_FALSE(infer::scheduler().loaded(infer::Slot::Image));
    CHECK(infer::current_image_context() == nullptr);
}

TEST_CASE("加载失败时调度器的账要退干净") {
    // 退不干净的后果是：调度器以为那个槽加载着，后面的预算计算全错，
    // 而且永远不会再去加载它——用户配好模型之后重启才恢复。
    infer::scheduler().evict_all();
    infer::register_sd_slots(with_models(), fake_profile(6.0));

    // 模型文件不存在（或者根本没链 sd.cpp），加载一定失败
    CHECK_THROWS(infer::scheduler().acquire(infer::Slot::Image));

    CHECK_FALSE(infer::scheduler().loaded(infer::Slot::Image));
    CHECK(infer::scheduler().lease_count(infer::Slot::Image) == 0);
    CHECK(infer::scheduler().resident_bytes() == 0);
    CHECK(infer::current_image_context() == nullptr);

    SUBCASE("退干净之后还能再试") {
        // 用户去补上模型文件再点一次，不该因为上一次失败而永久不可用
        CHECK_THROWS(infer::scheduler().acquire(infer::Slot::Image));
        CHECK_FALSE(infer::scheduler().loaded(infer::Slot::Image));
    }
}

TEST_CASE("显存预算留了余量") {
    // 估高了是 OOM 直接崩，估低了只是多分段（慢）。所以往低了取。
    infer::scheduler().evict_all();
    infer::scheduler().set_budget(0);   // 不限制，只看注册进去的估值

    infer::register_sd_slots(with_models(), fake_profile(6.0));
    // 6 GB 的卡不该按 6 GB 报预算——驱动上下文和别的程序也要占。
    // 这里只能间接验：注册进去的估值小于卡的容量。
    // （estimate 本身没有 getter，所以用"装得下"来验。）
    infer::scheduler().set_budget(static_cast<std::size_t>(6.0 * 1024) * 1024 * 1024);
    CHECK_THROWS(infer::scheduler().acquire(infer::Slot::Image));
    // 装不下的话上面抛的是"显存不够"，装得下才会走到 create 然后抛别的
    CHECK_FALSE(infer::scheduler().loaded(infer::Slot::Image));

    infer::scheduler().set_budget(0);
}

TEST_CASE("没配模型时的报错说清了去哪儿改") {
    if (!infer::sd_available()) {
        // 桩那条路先撞上"没编进出图后端"，测不到这一条
        return;
    }
    config::Settings s;   // image 和 video 都是空的
    try {
        infer::SdContext::create(s, 6.0, infer::ModelRole::Image);
        FAIL("该抛异常");
    } catch (const infer::SdError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        CHECK(msg.find("[models]") != std::string::npos);
    }
}

TEST_CASE("出图请求的种子必须能显式给") {
    // 让 sd.cpp 随机取种子的话，重跑同一个镜头得到的是另一张图，
    // "重试"和"换一张"就分不清了——而闸门重试依赖前者。
    infer::ImageRequest r;
    CHECK(r.seed == 0);        // 有默认值，不是未初始化
    r.seed = 12345;
    CHECK(r.seed == 12345);

    // 别的默认值也要是能直接用的，不能是 0 宽 0 高
    CHECK(r.width > 0);
    CHECK(r.height > 0);
    CHECK(r.steps > 0);
}

TEST_CASE("出视频默认开 VAE 分块，参数是实测出来的那一组") {
    // **这条不测代码，它钉的是 verify/RESULTS.md 三点八的实测结论。**
    //
    // 这几个数看着像可以随手清理掉的魔数，实际上每一个都对应一次失败：
    //
    //   不分块           解码要 11747 MB —— 6GB 卡上失败
    //   默认 32x32       9610 MB        —— 还是失败（40x22 的潜变量上
    //                                      切出来两块重叠 0.75，白切）
    //   再开时间维分块   10321 MB       —— 反而更高
    //   16x11 重叠 0.25  成功，44.7 秒
    //
    // 改这几个数之前先在 6GB 卡上跑一遍 Wan，别照着"看起来更整齐"改。
    infer::VideoRequest r;
    CHECK(r.vae_tiling);
    CHECK(r.vae_tile_x == 16);
    CHECK(r.vae_tile_y == 11);
    CHECK(r.vae_tile_overlap == doctest::Approx(0.25));
    // 时间维分块要关：低帧数下切不动，还引入有状态分块自身的开销
    CHECK_FALSE(r.vae_temporal_tiling);
}

// ---------------------------------------------------------------------------
// **不许拿视频模型顶替图像模型。**
//
// 2026-09-08 跑阶段 5 判据时撞上的：只配了 [models].video 没配 image，
// 出首帧那一步**整个进程崩掉**——异常码 0xc0000094（整数除零），
// HTTP 服务连同正在跑的整集一起没，日志里什么都没有。
//
// 原因看 API 就清楚：`sd_img_gen_params_t` 里**没有 video_frames 字段**
// （只有 sd_vid_gen_params_t 有），压根没法告诉 generate_image 出几帧。
//
// 代码原来是**故意**这么退的，注释写着"没配就退回视频模型出单帧——
// 这个选择和 Python 的 build_backend 是同一个判断"。那个判断在 Python
// 那边成立（走 ComfyUI 工作流，把 length 设成 1 就行），在这边不成立。
//
// 判断拆进了 sd_model_problem()，放在 #ifdef 外面——测试目标编的是
// 没链 sd.cpp 那一支，写在 #ifdef 里面就测不到。
// ---------------------------------------------------------------------------

TEST_CASE("没配 image 但配了 video：拦下来，不许顶替") {
    config::Settings s;
    s.models.video = "Wan2.2-TI2V-5B-Q4_K_M.gguf";
    s.models.image = "";

    const std::string why = infer::sd_model_problem(s, infer::ModelRole::Image);
    REQUIRE_FALSE(why.empty());
    // 话要说清两件事：缺什么，以及为什么不能拿现有的顶
    CHECK(why.find("[models].image") != std::string::npos);
    CHECK(why.find("不能用视频模型顶替") != std::string::npos);
    // 还要给出路，不然用户只知道不行、不知道怎么办
    CHECK(why.find("comfy") != std::string::npos);
}

TEST_CASE("配了 image 就放行") {
    config::Settings s;
    s.models.image = "Qwen-Image-Edit-Q4_K_M.gguf";
    s.models.video = "Wan2.2-TI2V-5B-Q4_K_M.gguf";
    CHECK(infer::sd_model_problem(s, infer::ModelRole::Image).empty());
    CHECK(infer::sd_model_problem(s, infer::ModelRole::Video).empty());
}

TEST_CASE("两个都没配：还是要说清缺的是 image") {
    config::Settings s;
    const std::string why = infer::sd_model_problem(s, infer::ModelRole::Image);
    REQUIRE_FALSE(why.empty());
    CHECK(why.find("image") != std::string::npos);
    // 这一支不该提"顶替"——没有视频模型可顶，说了只会让人困惑
    CHECK(why.find("不能用视频模型顶替") == std::string::npos);
}

TEST_CASE("出片没配 video：单独一句话") {
    config::Settings s;
    s.models.image = "Qwen-Image-Edit-Q4_K_M.gguf";
    const std::string why = infer::sd_model_problem(s, infer::ModelRole::Video);
    REQUIRE_FALSE(why.empty());
    CHECK(why.find("video") != std::string::npos);
}

// ---- 首帧和画幅比例对不上时的中心裁剪 ----
//
// 这一组测的是 center_crop_box 的算术。它存在的理由见头文件：
// sd.cpp 拿到比例不同的 init_image 会直接拉伸，不报错。

TEST_CASE("比例本来就一样：一刀不裁") {
    // 尺寸不同但比例相同——缩放交给下游，这里不该动。
    const auto box = infer::center_crop_box(1408, 2560, 704, 1280);
    CHECK(box.whole(1408, 2560));

    const auto same = infer::center_crop_box(544, 928, 544, 928);
    CHECK(same.whole(544, 928));
}

TEST_CASE("旧首帧 704x1280 出 544x928 的片：裁上下，宽不动") {
    // 就是 2026-09-10 改画幅留下的那批图。
    // 源 0.550 比目标 0.586 窄，所以是高的那一维多了。
    const auto box = infer::center_crop_box(704, 1280, 544, 928);
    CHECK(box.w == 704);
    CHECK(box.h == 704 * 928 / 544);  // 1200
    CHECK(box.x == 0);
    CHECK(box.y == (1280 - box.h) / 2);
    CHECK_FALSE(box.whole(704, 1280));
    // 裁完的比例要和目标一致（整除的余数之内）
    CHECK(std::abs(static_cast<double>(box.w) / box.h -
                   544.0 / 928.0) < 0.002);
}

TEST_CASE("源比目标宽：裁两侧，高不动") {
    const auto box = infer::center_crop_box(1920, 1080, 544, 928);
    CHECK(box.h == 1080);
    CHECK(box.w == 1080 * 544 / 928);  // 633
    CHECK(box.y == 0);
    CHECK(box.x == (1920 - box.w) / 2);
}

TEST_CASE("裁出来的框不许跑到图外面") {
    const int cases[][4] = {
        {704, 1280, 544, 928}, {1920, 1080, 544, 928},
        {100, 3000, 544, 928}, {3000, 100, 544, 928},
        {1, 1, 544, 928},      {544, 928, 2560, 1440},
    };
    for (const auto& c : cases) {
        const auto box = infer::center_crop_box(c[0], c[1], c[2], c[3]);
        CAPTURE(c[0]);
        CAPTURE(c[1]);
        CHECK(box.w >= 1);
        CHECK(box.h >= 1);
        CHECK(box.x >= 0);
        CHECK(box.y >= 0);
        CHECK(box.x + box.w <= c[0]);
        CHECK(box.y + box.h <= c[1]);
    }
}

TEST_CASE("尺寸不成样子：整张递下去，不算出负数来") {
    CHECK(infer::center_crop_box(0, 0, 544, 928).whole(0, 0));
    CHECK(infer::center_crop_box(704, 1280, 0, 928).whole(704, 1280));
    CHECK(infer::center_crop_box(-4, 8, 544, 928).whole(-4, 8));
}

// ---- 实测显存的落盘格式 ----
//
// 这个文件在两次运行之间保存的是**决定要不要卸模型的依据**。解析出错的
// 代价不对称：多算了只是白卸一次（慢），少算了是 CUDA OOM——而 OOM 在
// sd.cpp 里走的是 GGML_ASSERT，abort() 把整个服务带走。所以坏数据一律
// 当"没量过"，回到保守那条。

using M = infer::Scheduler::Measured;

TEST_CASE("实测显存：写出去再读回来，要一模一样") {
    std::map<infer::Slot, M> m;
    m[infer::Slot::Video] = M{80ull << 30, 2560ull * 1440 * 81};
    m[infer::Slot::Image] = M{26ull << 30, 2560ull * 1440};

    const auto back = infer::parse_measured_vram(infer::serialize_measured_vram(m));
    CHECK(back.size() == 2);
    CHECK(back.at(infer::Slot::Video).bytes == (80ull << 30));
    // **量的是多大的活也要一起存。** 光存字节数的话，在 720p 量到的数
    // 重启之后会被拿去给 2K 那一镜背书，而那正是 CUDA OOM 的前一步。
    CHECK(back.at(infer::Slot::Video).work == 2560ull * 1440 * 81);
    CHECK(back.at(infer::Slot::Image).bytes == (26ull << 30));
    CHECK(back.at(infer::Slot::Image).work == 2560ull * 1440);
    // 没量过的槽不该凭空冒出来
    CHECK(back.find(infer::Slot::LLM) == back.end());
}

TEST_CASE("实测显存：老文件里那个光秃秃的数还认，但当成不知道多大的活") {
    // 升级之前存下来的是 {"视频": 字节数}，没有 work。丢掉太浪费，
    // 但也不能当成"什么活都罩得住"——读回来 work = 0，调度器见到 0
    // 就不会拿它给指定了大小的那一镜背书，跑一镜自己就补上了。
    const auto m = infer::parse_measured_vram(R"({"视频":85899345920})");
    REQUIRE(m.size() == 1);
    CHECK(m.at(infer::Slot::Video).bytes == 85899345920ull);
    CHECK(m.at(infer::Slot::Video).work == 0);
}

TEST_CASE("实测显存：新格式里缺 work 也不丢这一条") {
    const auto m = infer::parse_measured_vram(R"({"视频":{"bytes":123}})");
    REQUIRE(m.size() == 1);
    CHECK(m.at(infer::Slot::Video).bytes == 123);
    CHECK(m.at(infer::Slot::Video).work == 0);
}

TEST_CASE("实测显存：0 不写出去") {
    // 0 的语义是"没量过"，写进文件再读回来会被当成量过了 0 字节。
    std::map<infer::Slot, M> m;
    m[infer::Slot::TTS] = M{0, 123};
    CHECK(infer::parse_measured_vram(infer::serialize_measured_vram(m)).empty());
}

TEST_CASE("实测显存：坏数据一律当没量过，绝不瞎猜") {
    // 少算了是 OOM，所以宁可回到保守那条。
    CHECK(infer::parse_measured_vram("").empty());
    CHECK(infer::parse_measured_vram("{ 这不是 json").empty());
    CHECK(infer::parse_measured_vram("[1,2,3]").empty());          // 不是对象
    CHECK(infer::parse_measured_vram(R"({"视频":"很多"})").empty());  // 不是数
    CHECK(infer::parse_measured_vram(R"({"视频":{"bytes":"多"}})").empty());
    CHECK(infer::parse_measured_vram(R"({"视频":{"work":9}})").empty());  // 缺 bytes
    CHECK(infer::parse_measured_vram(R"({"视频":{"bytes":0,"work":9}})").empty());
    CHECK(infer::parse_measured_vram(R"({"视频":-5})").empty());      // 负数
    CHECK(infer::parse_measured_vram(R"({"视频":0})").empty());       // 0
    CHECK(infer::parse_measured_vram(R"({"没这个槽":123})").empty());
}

TEST_CASE("实测显存：一条坏的不该带垮整份") {
    // 换了版本、多了个字段的时候，别把还认得的那几条一起丢掉。
    const auto m = infer::parse_measured_vram(
        R"({"视频":85899345920,"没这个槽":1,"图像":"坏的"})");
    CHECK(m.size() == 1);
    CHECK(m.at(infer::Slot::Video).bytes == 85899345920ull);
}
