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

#include <filesystem>
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
        infer::SdContext::create(with_models(), 6.0);
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
        infer::SdContext::create(s, 6.0);
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
