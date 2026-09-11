// 机器此刻的负载：CPU、内存、每张卡。顶栏那三个小表用（GET /api/system）。
//
// 用户 2026-09-11：「在顶部导航栏增加 GPU、CPU、内存的使用情况实时显示」。
//
// 两秒一问，所以每一条都得便宜：CPU 读 /proc/stat 差值（Windows 走
// GetSystemTimes），内存读 /proc/meminfo（容器里按 cgroup 上限算），
// 显卡走 NVML 进程内问。唯一会 fork 的是 nvidia-smi 那条退路，限了五秒一次。

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/hardware.hpp"

namespace changji::sysstat {

using GpuLoad = models::GpuLive;

struct Load {
    double cpu_percent = -1;   ///< -1 = 还没有差值（第一次采样）或者这平台问不到
    double mem_used_gb = 0;
    double mem_total_gb = 0;
    std::vector<GpuLoad> gpus;
};

/// 采一次样。CPU 是和上一次采样之间的差值，第一次调回 -1。
Load sample();

nlohmann::json to_json(const Load& load);

// ---- 纯函数，给测试用 ----

struct CpuTicks {
    unsigned long long idle = 0;
    unsigned long long total = 0;
};

/// /proc/stat 第一行。idle 含 iowait，total 是前八列之和。
std::optional<CpuTicks> parse_proc_stat(const std::string& text);

/// /proc/meminfo 的 MemTotal 和 MemAvailable，单位 GB。{total, available}。
std::optional<std::pair<double, double>> parse_meminfo(const std::string& text);

/// cgroup 的内存上限（memory.max / memory.limit_in_bytes 的内容）。
/// "max" 或者大得离谱（没设上限时 v1 写的是 2^63 附近的数）回 nullopt。
std::optional<unsigned long long> parse_cgroup_limit(const std::string& text);

/// `nvidia-smi --query-gpu=index,name,utilization.gpu,memory.used,memory.total
/// --format=csv,noheader,nounits` 的输出，一张卡一行。
std::vector<GpuLoad> parse_nvidia_smi(const std::string& out);

}  // namespace changji::sysstat
