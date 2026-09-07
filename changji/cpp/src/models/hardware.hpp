#pragma once

// 硬件探测与画质档位推导。
//
// 这个文件存在的理由是可移植性。画质档位绝不能写死成某一台机器的数字，
// 必须由运行时探测到的实际显存推导出来。
//
// 档位的基准点来自 RTX 5080 16GB 上对 Wan 2.2 TI2V 5B 的实测：
// 草稿 640x352 10 步 27 秒，预览 960x544 20 步 120 秒，成片 1280x704 30 步 392 秒。
// 其他显存档位按分辨率和步数缩放，首次运行时会用实测校准。
//
// 移植自 src/changji/hardware.py。

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/json_compat.hpp"

namespace changji::models {

/// 画质档位。分级生成靠它。
enum class Tier { DRAFT, PREVIEW, FINAL };

NLOHMANN_JSON_SERIALIZE_ENUM(Tier, {
    {Tier::DRAFT, "draft"},
    {Tier::PREVIEW, "preview"},
    {Tier::FINAL, "final"},
})

const char* to_string(Tier v);

/// 三个档位的固定顺序。遍历时用它，别依赖 map 的顺序。
const std::vector<Tier>& all_tiers();

/// 规整到 32 的倍数。分辨率不是 32 的倍数会导致 Wan 的潜空间对不齐。
///
/// ⚠️ 必须用**银行家舍入**（四舍六入五取偶），因为 Python 的 round() 就是那样。
/// C++ 的 std::round 是"四舍五入远离零"，两者在恰好 .5 时不同：
/// round(12.5) Python 给 12、std::round 给 13。当前档位表里只有 432/32=13.5
/// 命中半整数（两边碰巧都给 14），但 scaled_to 的 1:1 分支算的是 (w+h)/2，
/// 那是任意值，迟早会撞上。
int round32(int n);

/// 一个档位的生成参数。
struct TierSpec {
    Tier tier = Tier::DRAFT;
    int width = 0;
    int height = 0;
    int steps = 0;
    /// 本机实测的单镜耗时，未标定时为空
    std::optional<double> measured_seconds;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        TierSpec, tier, width, height, steps, measured_seconds)

    /// 按画幅调整。分辨率必须是 32 的倍数，否则 Wan 的潜空间对不齐。
    TierSpec scaled_to(const std::string& aspect_ratio) const;
};

struct GPUInfo {
    std::string name;
    int vram_mb = 0;
    std::optional<std::string> driver;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GPUInfo, name, vram_mb, driver)

    double vram_gb() const { return static_cast<double>(vram_mb) / 1024.0; }
};

/// 探测本机显卡。探测不到返回空，由调用方决定怎么办。
///
/// 只用 nvidia-smi，不引入 torch 依赖。这个包是编排引擎，
/// 真正的推理可能根本不在同一台机器上。
std::optional<GPUInfo> detect_gpu();

/// 按显存推导三个档位的参数。
std::map<Tier, TierSpec> tiers_for_vram(double vram_gb);

/// 本机硬件画像。配置里可以覆盖，方便推理服务在别的机器上时手动指定。
struct HardwareProfile {
    std::optional<GPUInfo> gpu;
    double vram_gb = 12.0;
    std::map<Tier, TierSpec> tiers;
    bool detected = false;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        HardwareProfile, gpu, vram_gb, tiers, detected)

    static HardwareProfile detect(std::optional<double> override_vram_gb = std::nullopt);

    std::string describe() const;

    /// 估算一集的纯生成时间，单位秒。未标定返回空。
    std::optional<double> estimate_episode(int shot_count, Tier tier) const;
};

}  // namespace changji::models
