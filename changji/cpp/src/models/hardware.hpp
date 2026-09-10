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
    /// 这台机器上有几张卡。**探测不到时是 1，不是 0**——
    /// 有一张卡在跑这个程序，0 会让"起几个工作进程"算出 0 个。
    ///
    /// **和 vram_mb 是两个维度，别混。** vram_mb 是**卡 0** 的显存，
    /// 决定单个镜头能跑多大（一个模型跑在一张卡上）；
    /// count 决定能同时跑几个镜头。八张 48 GB 加起来去查档位表，
    /// 会算出一张卡根本跑不动的分辨率。
    int count = 1;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GPUInfo, name, vram_mb, driver,
                                                count)

    double vram_gb() const { return static_cast<double>(vram_mb) / 1024.0; }
};

/// 解析 `nvidia-smi --query-gpu=name,memory.total,driver_version
/// --format=csv,noheader,nounits` 的输出。
///
/// **抽出来是为了能测。** 开发机只有一张卡，多卡那条路一行都跑不到；
/// 而这段要是错了，表现是"八个进程全挤在卡 0 上，看着在并行实际在排队"，
/// 一声不吭。
///
/// 显存取**第一行**（卡 0 的），卡数是非空行数——两个维度，别混。
std::optional<GPUInfo> parse_gpu_query(const std::string& out);

/// 探测本机显卡。探测不到返回空，由调用方决定怎么办。
///
/// 只用 nvidia-smi，不引入 torch 依赖。这个包是编排引擎，
/// 真正的推理可能根本不在同一台机器上。
std::optional<GPUInfo> detect_gpu();

/// 现在这张卡还空着多少显存（GB）。**每次调用都真去问一遍。**
///
/// 和 `detect_gpu()` 报的总量不是一回事：总量是静态的，这个是实时的，
/// 会随着别的进程、别的槽的占用变化。调度器拿它决定"要不要卸掉别的模型"
/// ——静态估算说装不下、而实际空着一大块的时候，卸载是纯浪费：
/// 一次重新加载是几十秒到几分钟。
///
/// 问不到（没有 nvidia-smi、多卡、解析失败）就回 nullopt，
/// 调用方退回原来的静态估算。**不要把"问不到"当成"没有空间"**。
std::optional<double> free_vram_gb();

/// 从 `nvidia-smi --query-gpu=memory.free` 的输出里解析空闲显存。
/// 单独拆出来是为了能测——测试里不该真去跑 nvidia-smi。
std::optional<double> parse_free_vram(const std::string& out);

/// 从 `vm_stat` 的输出里算出「还能用多少内存」（GB）。
///
/// **苹果机器上这就是"还剩多少显存"**：统一内存，CPU 和 GPU 共用一块。
///
/// 不能只看 "Pages free"——这台 16 GB 的 iMac 上它只有 4124 页（67 MB），
/// 拿它当依据的话调度器会以为一点空间都没有，每次都卸模型。macOS 真正
/// 能拿来用的是 free + inactive + purgeable + speculative：inactive 是
/// 有主但随时可以回收的，purgeable 是明说可以丢的。
///
/// 页大小从输出头一行 "(page size of N bytes)" 里读——**别写死 4096**，
/// 苹果芯片是 16384。写死的话算出来差四倍。
std::optional<double> parse_vm_stat(const std::string& out);

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
