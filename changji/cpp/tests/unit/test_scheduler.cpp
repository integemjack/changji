// 跨模型显存调度的测试。
//
// 这里测的是**策略**，不是推理。加载和卸载都是假的回调，只记账。
//
// 之所以刻意做成这样：接上真模型之后，每个用例要读几个 GB、跑几分钟，
// 策略层的 bug 就没法反复撞了。而策略层恰恰是最容易出错的地方——
// 引用计数、驱逐顺序、加载失败后的回滚，这三样错了都不会当场报错，
// 只会在跑到一半时以 OOM 或者段错误的形式出现。

#include <doctest/doctest.h>

#include <optional>
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "infer/scheduler.hpp"

using namespace changji::infer;

namespace {

constexpr std::size_t GB = 1024ull * 1024 * 1024;

/// 记账用的假模型。
struct FakeModel {
    std::atomic<int> loads{0};
    std::atomic<int> unloads{0};
    std::atomic<bool> resident{false};
    bool fail_next_load = false;
};

SlotSpec spec_for(Slot slot, FakeModel& m, std::size_t bytes, int priority,
                  Residency r = Residency::Cached) {
    SlotSpec s;
    s.slot = slot;
    s.residency = r;
    s.vram_estimate = bytes;
    s.evict_priority = priority;
    s.load = [&m] {
        if (m.fail_next_load) {
            m.fail_next_load = false;
            throw std::runtime_error("假装加载失败");
        }
        ++m.loads;
        m.resident = true;
    };
    s.unload = [&m] {
        ++m.unloads;
        m.resident = false;
    };
    return s;
}

}  // namespace

TEST_CASE("借出时才加载，不借不加载") {
    Scheduler s;
    FakeModel llm;
    s.register_slot(spec_for(Slot::LLM, llm, 9 * GB, 0));

    // 注册不等于加载。九个 GB 的东西不该因为"登记了一下"就读进来。
    CHECK(llm.loads == 0);
    CHECK_FALSE(s.loaded(Slot::LLM));

    {
        auto lease = s.acquire(Slot::LLM);
        CHECK(lease.valid());
        CHECK(llm.loads == 1);
        CHECK(s.loaded(Slot::LLM));
        CHECK(s.lease_count(Slot::LLM) == 1);
    }
    // Cached 的槽还回来之后仍然留着
    CHECK(s.lease_count(Slot::LLM) == 0);
    CHECK(s.loaded(Slot::LLM));
    CHECK(llm.unloads == 0);
}

TEST_CASE("重复借用只加载一次") {
    Scheduler s;
    FakeModel video;
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    auto a = s.acquire(Slot::Video);
    auto b = s.acquire(Slot::Video);
    auto c = s.acquire(Slot::Video);
    CHECK(video.loads == 1);
    CHECK(s.lease_count(Slot::Video) == 3);

    a.release();
    CHECK(s.lease_count(Slot::Video) == 2);
    CHECK(s.loaded(Slot::Video));
}

TEST_CASE("用完就放的槽在最后一个借用还回来时卸载") {
    // LLM 一集只用一次。留着纯粹占地方——它按定义不会再被用了，
    // 而那是九个 GB。
    Scheduler s;
    FakeModel llm;
    s.register_slot(spec_for(Slot::LLM, llm, 9 * GB, 0, Residency::Ephemeral));

    {
        auto a = s.acquire(Slot::LLM);
        auto b = s.acquire(Slot::LLM);
        CHECK(llm.resident);
        a.release();
        // 还有一个借用者，不能卸
        CHECK(llm.resident);
        CHECK(llm.unloads == 0);
    }
    CHECK_FALSE(llm.resident);
    CHECK(llm.unloads == 1);
}

TEST_CASE("装不下时按优先级驱逐") {
    Scheduler s;
    s.set_budget(6 * GB);

    FakeModel llm, image, video;
    // 优先级：数字小的先被驱逐。重新加载越贵的数字越大。
    s.register_slot(spec_for(Slot::LLM, llm, 2 * GB, 0));
    s.register_slot(spec_for(Slot::Image, image, 2 * GB, 5));
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    s.acquire(Slot::LLM).release();
    s.acquire(Slot::Image).release();
    CHECK(s.resident_bytes() == 4 * GB);

    // 再来 3 GB 就超了 6 GB，要驱逐。LLM 优先级最低，先走。
    {
        auto v = s.acquire(Slot::Video);
        CHECK_FALSE(s.loaded(Slot::LLM));
        CHECK(s.loaded(Slot::Image));   // 优先级更高，保住
        CHECK(s.loaded(Slot::Video));
        CHECK(llm.unloads == 1);
    }
    CHECK(s.resident_bytes() == 5 * GB);
}

TEST_CASE("正在被借用的槽绝不驱逐") {
    // 正在用的模型被抽走，表现是段错误，不是一个能读的报错。
    // 宁可这次加载失败。
    Scheduler s;
    s.set_budget(4 * GB);

    FakeModel llm, video;
    s.register_slot(spec_for(Slot::LLM, llm, 3 * GB, 0));    // 优先级最低
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    auto held = s.acquire(Slot::LLM);   // 一直拿着不放

    // LLM 虽然优先级最低，但正被借用，不能动。于是视频加载不进来。
    CHECK_THROWS_AS(s.acquire(Slot::Video), std::runtime_error);
    CHECK(s.loaded(Slot::LLM));
    CHECK_FALSE(s.loaded(Slot::Video));
    CHECK(llm.unloads == 0);

    // 放开之后就能加载了
    held.release();
    {
        auto v = s.acquire(Slot::Video);
        CHECK(s.loaded(Slot::Video));
        CHECK_FALSE(s.loaded(Slot::LLM));
    }
}

TEST_CASE("同优先级时最久没用的先走") {
    Scheduler s;
    s.set_budget(4 * GB);

    FakeModel a, b, c;
    s.register_slot(spec_for(Slot::LLM, a, 2 * GB, 5));
    s.register_slot(spec_for(Slot::Image, b, 2 * GB, 5));
    s.register_slot(spec_for(Slot::Video, c, 2 * GB, 5));

    s.acquire(Slot::LLM).release();     // 最早
    s.acquire(Slot::Image).release();
    // 再碰一下 LLM，让它变成"最近用过"
    s.acquire(Slot::LLM).release();

    {
        auto v = s.acquire(Slot::Video);
        // Image 是最久没用的，它先走，不是 LLM
        CHECK_FALSE(s.loaded(Slot::Image));
        CHECK(s.loaded(Slot::LLM));
    }
}

TEST_CASE("预算为零表示不限制") {
    Scheduler s;   // 没设预算
    FakeModel a, b, c, d;
    s.register_slot(spec_for(Slot::LLM, a, 100 * GB, 0));
    s.register_slot(spec_for(Slot::Image, b, 100 * GB, 1));
    s.register_slot(spec_for(Slot::Video, c, 100 * GB, 2));
    s.register_slot(spec_for(Slot::TTS, d, 100 * GB, 3));

    s.acquire(Slot::LLM).release();
    s.acquire(Slot::Image).release();
    s.acquire(Slot::Video).release();
    s.acquire(Slot::TTS).release();

    CHECK(s.loaded_slots().size() == 4);
    CHECK(s.resident_bytes() == 400 * GB);
}

TEST_CASE("加载失败要把账退干净") {
    // 退不干净的后果是：调度器以为那个槽加载着，
    // 后面的预算计算全错，而且永远不会再去加载它。
    Scheduler s;
    s.set_budget(6 * GB);
    FakeModel video;
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));
    video.fail_next_load = true;

    CHECK_THROWS_AS(s.acquire(Slot::Video), std::runtime_error);
    CHECK_FALSE(s.loaded(Slot::Video));
    CHECK(s.lease_count(Slot::Video) == 0);
    CHECK(s.resident_bytes() == 0);

    SUBCASE("退干净之后还能重试") {
        auto lease = s.acquire(Slot::Video);
        CHECK(s.loaded(Slot::Video));
        CHECK(video.loads == 1);
    }
}

TEST_CASE("加载失败不会把被驱逐的槽自动加载回来") {
    // 自动回滚会把一次失败变成一串连锁加载，而且日志里看不出源头。
    Scheduler s;
    s.set_budget(4 * GB);
    FakeModel llm, video;
    s.register_slot(spec_for(Slot::LLM, llm, 3 * GB, 0));
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    s.acquire(Slot::LLM).release();
    CHECK(s.loaded(Slot::LLM));

    video.fail_next_load = true;
    CHECK_THROWS(s.acquire(Slot::Video));

    // LLM 已经被驱逐了，不会自己回来
    CHECK_FALSE(s.loaded(Slot::LLM));
    CHECK(llm.loads == 1);
    CHECK(llm.unloads == 1);
}

TEST_CASE("evict 不强卸正在用的") {
    Scheduler s;
    FakeModel video;
    s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

    auto held = s.acquire(Slot::Video);
    CHECK(s.evict(Slot::Video) == false);
    CHECK(s.loaded(Slot::Video));

    held.release();
    CHECK(s.evict(Slot::Video) == true);
    CHECK_FALSE(s.loaded(Slot::Video));

    // 已经卸了再卸一次不报错
    CHECK(s.evict(Slot::Video) == true);
    CHECK(video.unloads == 1);
}

TEST_CASE("evict_all 放掉所有没被借用的") {
    // 跑完一集要把显存交回去，别的程序还等着用。
    Scheduler s;
    FakeModel a, b, c;
    s.register_slot(spec_for(Slot::LLM, a, 1 * GB, 0));
    s.register_slot(spec_for(Slot::Image, b, 1 * GB, 1));
    s.register_slot(spec_for(Slot::Video, c, 1 * GB, 2));

    s.acquire(Slot::LLM).release();
    s.acquire(Slot::Image).release();
    auto held = s.acquire(Slot::Video);

    s.evict_all();
    CHECK_FALSE(s.loaded(Slot::LLM));
    CHECK_FALSE(s.loaded(Slot::Image));
    CHECK(s.loaded(Slot::Video));      // 还借着，不动
}

TEST_CASE("没注册的槽借不出来") {
    Scheduler s;
    CHECK_THROWS_AS(s.acquire(Slot::Video), std::runtime_error);
}

TEST_CASE("已加载的槽不能重新注册") {
    // 覆盖之后新的 unload 会去卸一个不是它加载的东西。
    // 让它响亮地失败，比留一个对不上的账好。
    Scheduler s;
    FakeModel a, b;
    s.register_slot(spec_for(Slot::Video, a, 1 * GB, 0));
    auto held = s.acquire(Slot::Video);
    CHECK_THROWS_AS(s.register_slot(spec_for(Slot::Video, b, 2 * GB, 0)),
                    std::runtime_error);

    held.release();
    s.evict(Slot::Video);
    CHECK_NOTHROW(s.register_slot(spec_for(Slot::Video, b, 2 * GB, 0)));
}

TEST_CASE("析构时把还加载着的都卸掉") {
    FakeModel llm, video;
    {
        Scheduler s;
        s.register_slot(spec_for(Slot::LLM, llm, 1 * GB, 0));
        s.register_slot(spec_for(Slot::Video, video, 1 * GB, 1));
        s.acquire(Slot::LLM).release();
        s.acquire(Slot::Video).release();
        CHECK(llm.resident);
        CHECK(video.resident);
    }
    CHECK_FALSE(llm.resident);
    CHECK_FALSE(video.resident);
}

TEST_CASE("Lease 可以移动") {
    Scheduler s;
    FakeModel video;
    s.register_slot(spec_for(Slot::Video, video, 1 * GB, 0,
                             Residency::Ephemeral));

    {
        Lease outer;
        {
            Lease inner = s.acquire(Slot::Video);
            CHECK(inner.valid());
            outer = std::move(inner);
            // 移走之后原来那个不该再归还一次——归还两次会把引用计数弄成负的，
            // 然后模型在还有人用的时候被卸掉
            CHECK_FALSE(inner.valid());
        }
        // inner 析构了，但借用还在 outer 手里
        CHECK(s.lease_count(Slot::Video) == 1);
        CHECK(video.resident);
    }
    CHECK(s.lease_count(Slot::Video) == 0);
    CHECK_FALSE(video.resident);   // Ephemeral，还回来就卸
}

TEST_CASE("多线程同时借同一个槽，只加载一次") {
    // 两条线程同时发现"没加载"然后都去加载，会读两遍几个 GB，
    // 而且第二次覆盖第一次的指针，第一次那份就泄漏了。
    for (int round = 0; round < 20; ++round) {
        CAPTURE(round);
        Scheduler s;
        FakeModel video;
        auto spec = spec_for(Slot::Video, video, 1 * GB, 0);
        // 让加载慢一点，把竞态窗口撑开
        spec.load = [&video] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ++video.loads;
            video.resident = true;
        };
        s.register_slot(std::move(spec));

        std::vector<std::thread> ts;
        std::atomic<int> ok{0};
        for (int i = 0; i < 8; ++i) {
            ts.emplace_back([&] {
                try {
                    auto lease = s.acquire(Slot::Video);
                    ++ok;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } catch (...) {
                }
            });
        }
        for (auto& t : ts) t.join();

        CHECK(ok.load() == 8);
        CHECK(video.loads == 1);
        CHECK(s.lease_count(Slot::Video) == 0);
    }
}

TEST_CASE("按阶段分批比按镜头串行少换很多次模型") {
    // 这条不是测代码，是把方案里那条结论钉下来：
    // 跑一集的正确顺序是"四十个镜头的首帧全出完，再统一出视频"，
    // 不是"一个镜头出首帧再出视频，然后下一个"。
    //
    // 预算只装得下一个模型时，两种顺序的换入换出次数差四十倍。
    constexpr int kShots = 40;

    auto count_swaps = [](bool batched) {
        Scheduler s;
        s.set_budget(3 * GB);      // 只装得下一个
        FakeModel image, video;
        s.register_slot(spec_for(Slot::Image, image, 3 * GB, 5));
        s.register_slot(spec_for(Slot::Video, video, 3 * GB, 9));

        if (batched) {
            for (int i = 0; i < kShots; ++i) s.acquire(Slot::Image).release();
            for (int i = 0; i < kShots; ++i) s.acquire(Slot::Video).release();
        } else {
            for (int i = 0; i < kShots; ++i) {
                s.acquire(Slot::Image).release();
                s.acquire(Slot::Video).release();
            }
        }
        return image.loads + video.loads;
    };

    const int batched = count_swaps(true);
    const int serial = count_swaps(false);

    CHECK(batched == 2);              // 一共就加载两次
    CHECK(serial == kShots * 2);      // 每镜换两次
    CHECK(serial > batched * 30);
}

TEST_CASE("显存真空着的时候别瞎卸模型") {
    // vram_estimate 是按整份预算估的（"同时只装得下一个"），那是保守的：
    // 权重放内存时显存里只有计算缓冲，两个槽同时在也没事。只信估算的话，
    // 每次切阶段都要卸一个再装一个，而一次重装是几十秒到几分钟。
    Scheduler s;
    s.set_budget(10ull << 30);          // 预算 10 GB

    int image_unloads = 0;
    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = 8ull << 30;     // 估 8 GB
    img.load = [] {};
    img.unload = [&image_unloads] { ++image_unloads; };
    s.register_slot(img);

    SlotSpec vid;
    vid.slot = Slot::Video;
    vid.vram_estimate = 8ull << 30;
    vid.load = [] {};
    vid.unload = [] {};
    s.register_slot(vid);

    { auto lease = s.acquire(Slot::Image); }   // 装上图像槽
    CHECK(image_unloads == 0);

    SUBCASE("卡上真的空着 20 GB：不该卸") {
        s.set_free_vram_probe([] { return std::optional<double>(20.0); });
        { auto lease = s.acquire(Slot::Video); }
        CHECK(image_unloads == 0);
    }
    SUBCASE("卡上只剩 1 GB：照旧按估算卸") {
        s.set_free_vram_probe([] { return std::optional<double>(1.0); });
        { auto lease = s.acquire(Slot::Video); }
        CHECK(image_unloads == 1);
    }
    SUBCASE("问不到就退回估算——**问不到不等于有空间**") {
        s.set_free_vram_probe([] { return std::optional<double>{}; });
        { auto lease = s.acquire(Slot::Video); }
        CHECK(image_unloads == 1);
    }
    SUBCASE("没装探针时和以前一样") {
        { auto lease = s.acquire(Slot::Video); }
        CHECK(image_unloads == 1);
    }
}

TEST_CASE("显存不够时，每个槽要给出各自的出路") {
    // 只说一句"显存不够"用户无从下手。配音和大模型都能换成外部服务
    // （改配置，不改代码），出图出片躲不掉、只能在放内存和降分辨率之间挑。
    SUBCASE("配音要点名外接 API 这条路") {
        const std::string m = out_of_vram_message(Slot::TTS);
        CHECK(m.find("tts") != std::string::npos);
        CHECK(m.find("base_url") != std::string::npos);
        CHECK(m.find("http") != std::string::npos);
    }
    SUBCASE("大模型同样") {
        const std::string m = out_of_vram_message(Slot::LLM);
        CHECK(m.find("llm") != std::string::npos);
        CHECK(m.find("OpenAI") != std::string::npos);
    }
    SUBCASE("出图出片给的是另外两条，别乱指外接服务") {
        for (const Slot s : {Slot::Image, Slot::Video}) {
            const std::string m = out_of_vram_message(s);
            CHECK(m.find("weights") != std::string::npos);
            CHECK(m.find("清晰度") != std::string::npos);
            // 这两个槽没有外部服务可换，别给假出路
            CHECK(m.find("base_url") == std::string::npos);
            // **清晰度不在设置页了**（2026-09-10 搬到项目的「画面」卡）。
            // 指错地方的话用户会在设置页翻半天，而这句话是他此刻唯一的线索。
            CHECK(m.find("项目页") != std::string::npos);
            CHECK(m.find("设置页") == std::string::npos);
            // **档位名要和下拉框里写的一样。** 界面上是「标准（544×928）」；
            // 取值虽然还叫 "720p"（老项目的配置文件里存的就是它），但那个
            // 字符串不该出现在给人看的话里——2026-09-10 画幅改成 544×928
            // 之后，叫 720p 就是假的，用户会在那张卡上找一个不存在的选项。
            CHECK(m.find("标准") != std::string::npos);
            CHECK(m.find("544") != std::string::npos);
            CHECK(m.find("720p") == std::string::npos);
        }
    }
    SUBCASE("每一条都得先说清是哪个槽") {
        for (const Slot s : {Slot::LLM, Slot::Image, Slot::Video, Slot::TTS}) {
            CHECK(out_of_vram_message(s).find(to_string(s)) !=
                  std::string::npos);
        }
    }
    SUBCASE("一条都不许再提 comfy") {
        // ComfyUI 2026-09-10 拆了，comfy 现在连配置校验都过不去。
        // 把人指到一个不存在的取值上，比只说一句"显存不够"更糟。
        for (const Slot s : {Slot::LLM, Slot::Image, Slot::Video, Slot::TTS}) {
            CAPTURE(to_string(s));
            CHECK(out_of_vram_message(s).find("comfy") == std::string::npos);
            CHECK(out_of_vram_message(s).find("ComfyUI") == std::string::npos);
        }
    }
}

TEST_CASE("生产里 vram_estimate 就等于整份预算：老实数才救得回来") {
    // 上面那组用的是 8 GB 估值配 10 GB 预算，探针一问就够。
    // **生产不长这样**：register_sd_slots 把每个槽的 vram_estimate 都设成
    // 整份预算（整卡的九成），谁也凑不出第二份。那时候拿 vram_estimate
    // 去问"卡上够不够"，答案永远是不够——这条分支等于不存在，
    // 每次点出片照样把大模型卸掉。这一组盯的就是那个形状。
    Scheduler s;
    const std::size_t budget = 28ull << 30;   // 32 GB 卡的九成，约 28 GB
    s.set_budget(budget);

    int llm_unloads = 0;
    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;               // 和预算一样大，生产就是这样
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [&llm_unloads] { ++llm_unloads; };
    s.register_slot(llm);

    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = budget;
    img.evict_priority = 5;
    img.load = [] {};
    img.unload = [] {};

    // 卡上真空着 23 GB（大模型占了 8 GB 左右）。
    SUBCASE("图像这一路真占 10.6 GB：够，不该卸大模型") {
        // Q4 图像模型放内存：缓冲 6.6 + 4.0，权重不常驻。
        img.live_vram_estimate = 10ull << 30;
        s.register_slot(img);
        { auto lease = s.acquire(Slot::LLM); }
        s.set_free_vram_probe([] { return std::optional<double>(23.0); });
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 0);
    }
    SUBCASE("图像这一路真占 26.6 GB：不够，照卸") {
        img.live_vram_estimate = 26ull << 30;
        s.register_slot(img);
        { auto lease = s.acquire(Slot::LLM); }
        s.set_free_vram_probe([] { return std::optional<double>(23.0); });
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 1);
    }
    SUBCASE("没给老实数：退回 vram_estimate，也就是老行为") {
        s.register_slot(img);                 // live_vram_estimate 留 0
        { auto lease = s.acquire(Slot::LLM); }
        s.set_free_vram_probe([] { return std::optional<double>(23.0); });
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 1);
    }
}

TEST_CASE("老实数只走问到卡那条路，静态那条一点不放松") {
    // 探针问不到（没有 nvidia-smi）的时候，老实数不许拿来当依据——
    // "问不到"不等于"有空间"。
    Scheduler s;
    const std::size_t budget = 28ull << 30;
    s.set_budget(budget);

    int llm_unloads = 0;
    SlotSpec llm;
    llm.slot = Slot::LLM;
    llm.vram_estimate = budget;
    llm.evict_priority = 1;
    llm.load = [] {};
    llm.unload = [&llm_unloads] { ++llm_unloads; };
    s.register_slot(llm);

    SlotSpec img;
    img.slot = Slot::Image;
    img.vram_estimate = budget;
    img.live_vram_estimate = 10ull << 30;     // 老实数很小
    img.evict_priority = 5;
    img.load = [] {};
    img.unload = [] {};
    s.register_slot(img);

    { auto lease = s.acquire(Slot::LLM); }
    SUBCASE("没装探针") {
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 1);
    }
    SUBCASE("装了但问不到") {
        s.set_free_vram_probe([] { return std::optional<double>{}; });
        { auto lease = s.acquire(Slot::Image); }
        CHECK(llm_unloads == 1);
    }
}

TEST_CASE("配音显存不够时，抛出来的就是那条带出路的话") {
    // 消息内容上面测过了，这一条测的是**够不够得着**：
    // out_of_vram_message 只在 acquire 失败时抛，所以必须真有人借这个槽。
    // 2026-09-10 之前没人借——tts_backends 直接 LlamaTts::load 绕开了调度器，
    // 那段"改成 [tts].backend = http 接外部服务"的话是死代码，用户看不到。
    Scheduler s;
    s.set_budget(4ull << 30);

    SlotSpec big;
    big.slot = Slot::Video;
    big.vram_estimate = 4ull << 30;
    big.load = [] {};
    big.unload = [] {};
    s.register_slot(big);

    SlotSpec tts;
    tts.slot = Slot::TTS;
    tts.vram_estimate = 3ull << 30;
    tts.load = [] {};
    tts.unload = [] {};
    s.register_slot(tts);

    // 视频槽借着不放，腾不出地方。
    auto held = s.acquire(Slot::Video);
    std::string msg;
    try {
        auto lease = s.acquire(Slot::TTS);
        FAIL("显存不够却借到了");
    } catch (const std::exception& e) {
        msg = e.what();
    }
    CHECK(msg.find("base_url") != std::string::npos);
    CHECK(msg.find("http") != std::string::npos);
    // 别把人指到一个已经拆掉的取值上。
    CHECK(msg.find("comfy") == std::string::npos);
}
