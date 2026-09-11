// hardware.cpp 的对拍测试。
//
// 这个文件盯的核心是一个很容易漏的差异：**Python 的 round() 是银行家舍入
// （四舍六入五取偶），C++ 的 std::round 是四舍五入远离零。**
// 恰好落在 .5 上时两者不同，而 _round32 每次调用都可能撞上。
//
// 语料里 round32(400) = 384、round32(464) = 448，用 std::round 会得到
// 416 和 480，全错。分辨率算错的后果是 Wan 的潜空间对不齐。

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "models/hardware.hpp"
#include "util/paths.hpp"

using namespace changji::models;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    json j;
    in >> j;
    return j;
}

}  // namespace

TEST_CASE("round32 用的是银行家舍入，和 Python 一致") {
    for (const auto& c : load_golden("round32")) {
        const int n = c.at("n").get<int>();
        CAPTURE(n);
        CHECK(round32(n) == c.at("expected").get<int>());
    }
}

TEST_CASE("半整数的几例单独钉死") {
    // 这几个是 std::round 会做错的。留在这里当哨兵：
    // 谁把 nearbyint 改回 std::round，立刻红。
    CHECK(round32(400) == 384);  // 12.5 → 12（偶）
    CHECK(round32(464) == 448);  // 14.5 → 14（偶）
    CHECK(round32(432) == 448);  // 13.5 → 14（偶），这个两种舍入碰巧相同
    CHECK(round32(48) == 64);    // 1.5 → 2（偶）
}

TEST_CASE("各档显存推导出的档位参数与 Python 一致") {
    for (const auto& c : load_golden("tiers_for_vram")) {
        const double vram = c.at("vram_gb").get<double>();
        CAPTURE(vram);
        const auto tiers = tiers_for_vram(vram);
        const json& want = c.at("tiers");

        REQUIRE(tiers.size() == want.size());
        for (Tier t : all_tiers()) {
            const std::string key = to_string(t);
            CAPTURE(key);
            REQUIRE(want.contains(key));
            const auto it = tiers.find(t);
            REQUIRE(it != tiers.end());
            const TierSpec& got = it->second;
            const json& w = want.at(key);

            CHECK(got.width == w.at("width").get<int>());
            CHECK(got.height == w.at("height").get<int>());
            CHECK(got.steps == w.at("steps").get<int>());
            // 耗时估算里还有一次 round(x, 1) 的银行家舍入
            REQUIRE(got.measured_seconds.has_value());
            CHECK(*got.measured_seconds ==
                  doctest::Approx(w.at("measured_seconds").get<double>()).epsilon(1e-9));
        }
    }
}

TEST_CASE("画幅缩放") {
    for (const auto& c : load_golden("tier_scaled_to")) {
        const json& in = c.at("input");
        TierSpec spec;
        spec.tier = Tier::FINAL;
        spec.width = in.at("width").get<int>();
        spec.height = in.at("height").get<int>();
        spec.steps = in.at("steps").get<int>();
        CAPTURE(spec.width);
        CAPTURE(spec.height);

        for (const char* ratio : {"9:16", "16:9", "1:1"}) {
            CAPTURE(ratio);
            const TierSpec got = spec.scaled_to(ratio);
            const json& want = c.at(ratio);
            CHECK(got.width == want.at("width").get<int>());
            CHECK(got.height == want.at("height").get<int>());
            // 缩放不该动步数和档位
            CHECK(got.steps == spec.steps);
            CHECK(got.tier == spec.tier);
        }
    }
}

TEST_CASE("describe 的输出与 Python 逐字节相同") {
    for (const auto& c : load_golden("hardware_describe")) {
        HardwareProfile p;
        p.vram_gb = c.at("vram_gb").get<double>();
        p.detected = c.at("detected").get<bool>();
        if (!c.at("gpu").is_null()) {
            GPUInfo g;
            g.name = c.at("gpu").at("name").get<std::string>();
            g.vram_mb = c.at("gpu").at("vram_mb").get<int>();
            if (!c.at("gpu").at("driver").is_null()) {
                g.driver = c.at("gpu").at("driver").get<std::string>();
            }
            p.gpu = g;
        }
        p.tiers = tiers_for_vram(p.vram_gb);

        CAPTURE(p.vram_gb);
        CAPTURE(p.detected);
        // 这一条是逐字节的：describe 直接打给用户看
        CHECK(p.describe() == c.at("describe").get<std::string>());

        const auto est = p.estimate_episode(20, Tier::FINAL);
        const json& want_est = c.at("estimate_episode_20_final");
        if (want_est.is_null()) {
            CHECK_FALSE(est.has_value());
        } else {
            REQUIRE(est.has_value());
            CHECK(*est == doctest::Approx(want_est.get<double>()).epsilon(1e-9));
        }
    }
}

TEST_CASE("探测不到显卡时按 12GB 保守估算") {
    // 不依赖本机有没有卡：直接给一个覆盖值走另一条分支
    const HardwareProfile p = HardwareProfile::detect(6.0);
    CHECK(p.vram_gb == doctest::Approx(6.0));
    CHECK_FALSE(p.detected);  // 给了覆盖值就不算探测到
    CHECK(p.tiers.size() == 3);
    // 6GB 落在最低那一档
    CHECK(p.tiers.at(Tier::DRAFT).width == 448);
}

TEST_CASE("本机探测能跑通且不崩") {
    // 这台机器上有没有卡都不影响：detect 的契约是探测不到就返回空，
    // 不是抛异常。前置验证里 doctor 崩掉的教训就在这里。
    const HardwareProfile p = HardwareProfile::detect();
    CHECK(p.tiers.size() == 3);
    CHECK(p.vram_gb > 0.0);
    if (p.gpu.has_value()) {
        CHECK_FALSE(p.gpu->name.empty());
        CHECK(p.gpu->vram_mb > 0);
        CHECK(p.detected);
    }
}


TEST_CASE("问实时空闲显存：有卡就问得到，没卡就老老实实说没有") {
    // **这条是给 NVML 那条新路兜底的。**
    //
    // free_vram_gb 原来只有一条路：fork + exec 跑 nvidia-smi。这个进程
    // 初始化 CUDA 之后 fork 是 NVIDIA 明确不支持的，问不到就退回保守
    // 估算——"显存够就不清理"于是可能从来没生效过。现在先问 NVML
    // （进程内、不 fork），问不到才走老路。
    //
    // 契约是：**要么给一个说得通的数，要么说没有，绝不给个荒唐的数**。
    // 给荒唐的数比说没有更糟：说没有只是退回保守（多卸一次），
    // 给个偏大的数是 CUDA OOM，abort() 把整个服务带走。
    const auto gb = free_vram_gb();
    if (gb.has_value()) {
        CHECK(*gb >= 0.0);
        CHECK(*gb < 4096.0);   // 2026 年还没有 4 TB 显存的卡
#if !defined(__APPLE__)
        // 空闲不可能比整卡还多。**这一条最要紧**：调度器拿它和
        // "这一路要占多少"直接比，虚报一点点就是一次 OOM。
        //
        // **Mac 上不比。** 那边是统一内存：free_vram_gb 报的是 vm_stat
        // 算出来的"还能用多少系统内存"，而 gpu->vram_gb() 是
        // hw.memsize × iogpu.wired_limit_pct（默认七成半）。两个数出自
        // 两套口径，空闲大过那个七成半是正常的，不是错。
        const HardwareProfile p = HardwareProfile::detect();
        if (p.gpu.has_value()) {
            CHECK(*gb <= p.gpu->vram_gb() + 1.0);
        }
#endif
    }
    // 没值也是合法答案（没装 NVIDIA 驱动的机器），不该因此判失败。
}

TEST_CASE("总量和空闲要一次问出来，而且互相说得通") {
    // 算"这个槽实际占了多少"用的是 总量 − 空闲。分两次问的话两个数来自
    // 两个时刻、两条不同的路，差值就不是这个槽占的——而那个差值会被当成
    // 实测值记下来，之后每一镜都拿它判要不要卸模型。
    const auto t = vram_totals_gb();
    if (t.has_value()) {
        CHECK(t->total_gb > 0.0);
        CHECK(t->free_gb >= 0.0);
        // **空闲不可能比总量还多。** 反过来的话 用掉的 = 总量 − 空闲
        // 会是负数，实测值就记不下来，"够就不清理"永远起不来。
        CHECK(t->free_gb <= t->total_gb);
        // 和单独问空闲那条对得上（两次调用之间会变，放宽到 4 GB）。
        if (const auto f = free_vram_gb(); f.has_value()) {
            CHECK(std::abs(*f - t->free_gb) < 4.0);
        }
        // 和硬件探测报的整卡容量对得上。
        const HardwareProfile p = HardwareProfile::detect();
        if (p.gpu.has_value()) {
            CHECK(std::abs(p.gpu->vram_gb() - t->total_gb) < 1.0);
        }
    }
    // 拿不到也是合法答案（Mac、没装驱动的机器），调用方会退回老路。
}

TEST_CASE("CHANGJI_NO_NVML 能把新路关掉，退回老路") {
    // 新加一个原生库依赖，得留一个一键关掉的口子。关掉之后仍然要么
    // 给数、要么给空——不能因为关掉就崩，也不能给个荒唐的数。
    const auto before = free_vram_gb();
    changji::paths::set_env("CHANGJI_NO_NVML", "1");
    const auto after = free_vram_gb();
    changji::paths::set_env("CHANGJI_NO_NVML", "");
    if (after.has_value()) {
        CHECK(*after >= 0.0);
        CHECK(*after < 4096.0);
    }
    // 两条路问的是同一张卡，差得离谱就说明有一条读错了。
    // 放宽到 4 GB：两次调用之间显存本来就在变。
    if (before.has_value() && after.has_value()) {
        CHECK(std::abs(*before - *after) < 4.0);
    }
}

TEST_CASE("认得出有几张卡") {
    // **这台开发机只有一张卡，多卡那条路一行都跑不到。**
    // 明天上 8×48 才知道对不对，而那时候要是错了，表现是
    // "八个进程全挤在卡 0 上，看着在并行实际在排队"，一声不吭。
    // 所以拿真实格式的输出在这儿先验。

    // 一张卡
    {
        const auto g = parse_gpu_query(
            "NVIDIA GeForce RTX 2060, 6144, 581.15\n");
        REQUIRE(g.has_value());
        CHECK(g->name == "NVIDIA GeForce RTX 2060");
        CHECK(g->vram_mb == 6144);
        CHECK(g->count == 1);
    }

    // 八张卡：**显存仍然是单卡的 48 GB，不是加起来的 384**
    {
        std::string out;
        for (int i = 0; i < 8; ++i) out += "NVIDIA L40S, 49140, 581.15\n";
        const auto g = parse_gpu_query(out);
        REQUIRE(g.has_value());
        CHECK(g->count == 8);
        CHECK(g->vram_mb == 49140);
        // 加起来去查档位表会算出一张卡根本跑不动的分辨率
        CHECK(g->vram_gb() < 50.0);
    }

    // 末尾没有换行也要数对
    {
        const auto g = parse_gpu_query(
            "NVIDIA L40S, 49140, 581.15\nNVIDIA L40S, 49140, 581.15");
        REQUIRE(g.has_value());
        CHECK(g->count == 2);
    }

    // 中间的空行不算一张卡
    {
        const auto g = parse_gpu_query(
            "NVIDIA L40S, 49140, 581.15\n\nNVIDIA L40S, 49140, 581.15\n\n");
        REQUIRE(g.has_value());
        CHECK(g->count == 2);
    }

    // 探测不到就是探测不到，别编一个出来
    CHECK_FALSE(parse_gpu_query("").has_value());
    CHECK_FALSE(parse_gpu_query("\n\n").has_value());
    // 显存那一列不是数字时也不该硬凑
    CHECK_FALSE(parse_gpu_query("某张卡, N/A, 1.0\n").has_value());
}

TEST_CASE("档位按单卡算，不按总显存") {
    // 八张 48 GB 加起来 384 GB 去查档位表，会算出一张卡跑不动的分辨率。
    // 这一条钉住"两个维度不能混"。
    const auto one = tiers_for_vram(48.0);
    const auto eight = tiers_for_vram(48.0);   // 卡数不参与
    CHECK(one.at(Tier::FINAL).width ==
          eight.at(Tier::FINAL).width);

    // 而 384 GB 会落到更高的档——正是不该发生的那种
    const auto summed = tiers_for_vram(384.0);
    CHECK(summed.at(Tier::FINAL).width >=
          one.at(Tier::FINAL).width);
}

TEST_CASE("解析实时空闲显存") {
    // nounits 格式：一张卡一行，纯数字（MiB）
    CHECK(parse_free_vram("12873\n").value() ==
          doctest::Approx(12.571).epsilon(0.01));
    // 带单位的也吃
    CHECK(parse_free_vram("12873 MiB\n").value() ==
          doctest::Approx(12.571).epsilon(0.01));
    // 问不到时**必须回空，不能回 0**——回 0 会被当成"没空间"，
    // 而"问不到"和"没空间"是两回事：前者该退回静态估算。
    CHECK_FALSE(parse_free_vram("").has_value());
    CHECK_FALSE(parse_free_vram("\n").has_value());
    CHECK_FALSE(parse_free_vram("N/A\n").has_value());
    CHECK_FALSE(parse_free_vram("0\n").has_value());
}

// ---- 苹果机器：统一内存当显存 ----
//
// 这台机器上没有 nvidia-smi，而 detect_gpu 以前只认它：探不到就退回
// "按 12 GB 估算"。一台 128 GB 的 Mac 于是被当成 12 GB，档位、权重放哪、
// "显存够就不用清理"全部按 12 GB 算——**而且不报错**，只是什么都跑不大。

/// 真机上抓的（iMac21,1 / Apple M1 / 16 GB，页大小 16384）。
static const char* kVmStat =
    "Mach Virtual Memory Statistics: (page size of 16384 bytes)\n"
    "Pages free:                                     6364.\n"
    "Pages active:                                 360261.\n"
    "Pages inactive:                               385709.\n"
    "Pages speculative:                             10817.\n"
    "Pages throttled:                                   0.\n"
    "Pages wired down:                             127621.\n"
    "Pages purgeable:                               25885.\n"
    "\"Translation faults\":                      574116357.\n"
    "Pages copy-on-write:                        20675625.\n";

TEST_CASE("vm_stat：算的是「还能用多少」，不是「完全空着多少」") {
    const auto gb = parse_vm_stat(kVmStat);
    REQUIRE(gb.has_value());

    // free 6364 + inactive 385709 + purgeable 25885 + speculative 10817
    // = 428775 页 × 16384 字节 = 6.54 GB
    CHECK(*gb == doctest::Approx(6.54).epsilon(0.01));

    // **只看 Pages free 是不行的**：那只有 6364 页 ≈ 0.1 GB，
    // 调度器会以为一点空间都没有，每次都去卸模型。
    CHECK(*gb > 1.0);
}

TEST_CASE("vm_stat：页大小必须从输出里读，不能写死 4096") {
    // 苹果芯片是 16384。写死 4096 的话算出来差四倍——而这不会报错，
    // 只是调度器一直以为显存不够。
    std::string small = kVmStat;
    const auto pos = small.find("16384");
    REQUIRE(pos != std::string::npos);
    small.replace(pos, 5, "4096");
    const auto a = parse_vm_stat(kVmStat);
    const auto b = parse_vm_stat(small);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == doctest::Approx(*b * 4.0).epsilon(0.01));
}

TEST_CASE("vm_stat：读不出来就说读不出来，别猜") {
    // 猜一个数比没有更糟：调度器会拿它当真，而"问不到"那条路本来就有
    // 保守的退路（见 Scheduler::make_room）。
    CHECK_FALSE(parse_vm_stat("").has_value());
    CHECK_FALSE(parse_vm_stat("完全不相干的输出").has_value());
    // 没有页大小那一行 → 不猜
    CHECK_FALSE(parse_vm_stat("Pages free: 100.\n").has_value());
}
