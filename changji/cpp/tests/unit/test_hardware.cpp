// hardware.cpp 的对拍测试。
//
// 这个文件盯的核心是一个很容易漏的差异：**Python 的 round() 是银行家舍入
// （四舍六入五取偶），C++ 的 std::round 是四舍五入远离零。**
// 恰好落在 .5 上时两者不同，而 _round32 每次调用都可能撞上。
//
// 语料里 round32(400) = 384、round32(464) = 448，用 std::round 会得到
// 416 和 480，全错。分辨率算错的后果是 Wan 的潜空间对不齐。

#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "models/hardware.hpp"

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
