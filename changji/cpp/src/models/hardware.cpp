#include "models/hardware.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "util/paths.hpp"
#include "util/proc.hpp"

// NVML（显卡驱动自带的那个库）**运行时加载**，不在链接期依赖它。
// 见下面 Nvml 上面那段。Mac 上根本没有这条路，整块编译掉。
#if !defined(__APPLE__)
#if defined(_WIN32)
// **这两个必须在 windows.h 之前。** 不定 NOMINMAX 的话它会把 min/max
// 定义成宏，这个文件下面那些 std::min / std::max 当场编不过
// （报的是"':' 右边的非法标记"，看不出跟 windows.h 有关系）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace changji::models {

namespace {

/// Python 的 round(x, 1)：先按 1 位小数做银行家舍入。
///
/// 不能写成 std::round(x*10)/10——那是"远离零"的舍入，
/// 在 .05 结尾的数上和 Python 不一致。std::nearbyint 用的是当前舍入模式，
/// IEEE 754 默认就是取偶，与 Python 相同。
double round1(double x) {
    return std::nearbyint(x * 10.0) / 10.0;
}

// 按显存分的档位表。第一个元素是该档位要求的最低显存 GB。
// 显存越小，分辨率和步数越保守，否则会 OOM 或者慢到不可用。
//
// 阈值全部比标称容量低 0.5，因为驱动和固件会占掉一部分，
// nvidia-smi 报出来的永远小于标称值。一张 16GB 的卡通常报 15.9，
// 按 16.0 卡阈值会把它错判成 12GB 档。
struct TierRow {
    double threshold;
    int draft[3];    // w, h, steps
    int preview[3];
    int final_[3];
};

const TierRow kTierTable[] = {
    {23.5, {768, 432, 10},  {1280, 704, 20},  {1920, 1088, 30}},
    {15.5, {640, 352, 10},  {960, 544, 20},   {1280, 704, 30}},
    {11.5, {512, 288, 8},   {768, 432, 18},   {960, 544, 28}},
    {7.5,  {448, 256, 8},   {640, 352, 16},   {768, 432, 25}},
};
constexpr int kTierRows = static_cast<int>(sizeof(kTierTable) / sizeof(kTierTable[0]));

/// 基准档在表里的下标。耗时估算以它为原点，从表里取而不是另写一个数字，
/// 避免改表后两处对不上。
constexpr int kReferenceIndex = 1;

const int* row_for(const TierRow& row, Tier t) {
    switch (t) {
        case Tier::DRAFT:   return row.draft;
        case Tier::PREVIEW: return row.preview;
        case Tier::FINAL:   return row.final_;
    }
    return row.draft;
}

/// 该档位在 RTX 5080 16GB 上的实测单镜耗时。
double reference_seconds(Tier t) {
    switch (t) {
        case Tier::DRAFT:   return 27.0;
        case Tier::PREVIEW: return 120.0;
        case Tier::FINAL:   return 392.0;
    }
    return 0.0;
}

/// 粗估单镜耗时。
///
/// 仅用于给用户一个数量级预期和排产估算，不是承诺。真实数字要靠标定
/// 在目标机器上实测。扩散模型耗时大致正比于像素数乘步数。
double estimate_seconds(Tier tier, int w, int h, int steps, double vram_gb) {
    const int* ref = row_for(kTierTable[kReferenceIndex], tier);
    const double ref_work =
        static_cast<double>(round32(ref[0])) * round32(ref[1]) * ref[2];
    const double work = static_cast<double>(w) * h * steps;
    double base = reference_seconds(tier) * (work / ref_work);

    // 比基准档更小的机器通常算力也弱，且要频繁换入换出，给一个惩罚系数。
    // 基准档自身不吃惩罚，否则实测值会被凭空放大。
    const double ref_vram = kTierTable[kReferenceIndex].threshold;
    if (vram_gb < ref_vram) {
        base *= 1.0 + (ref_vram - vram_gb) * 0.08;
    }
    return round1(base);
}

std::string fmt1(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

std::string fmt0(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
}

/// 左对齐补空格到指定宽度，对应 Python 的 f"{s:8s}"。
std::string pad_right(const std::string& s, std::size_t width) {
    std::string out = s;
    while (out.size() < width) out.push_back(' ');
    return out;
}

}  // namespace

const char* to_string(Tier v) {
    switch (v) {
        case Tier::DRAFT:   return "draft";
        case Tier::PREVIEW: return "preview";
        case Tier::FINAL:   return "final";
    }
    return "?";
}

const std::vector<Tier>& all_tiers() {
    static const std::vector<Tier> kAll = {Tier::DRAFT, Tier::PREVIEW, Tier::FINAL};
    return kAll;
}

int round32(int n) {
    // nearbyint 走当前舍入模式，IEEE 754 默认取偶，与 Python 的 round() 一致。
    const double r = std::nearbyint(static_cast<double>(n) / 32.0) * 32.0;
    return std::max(32, static_cast<int>(r));
}

TierSpec TierSpec::scaled_to(const std::string& aspect_ratio) const {
    TierSpec out = *this;
    if (aspect_ratio == "9:16") {
        const int long_side = std::max(width, height);
        const int short_side = std::min(width, height);
        out.width = round32(short_side);
        out.height = round32(long_side);
    } else if (aspect_ratio == "1:1") {
        // 注意 Python 用的是整除 //，先截断再规整
        const int side = round32((width + height) / 2);
        out.width = side;
        out.height = side;
    } else {  // 16:9
        const int long_side = std::max(width, height);
        const int short_side = std::min(width, height);
        out.width = round32(long_side);
        out.height = round32(short_side);
    }
    return out;
}

#if defined(__APPLE__)
// **必须写绝对路径。** sysctl 在 /usr/sbin，而子进程拿到的最小 PATH 里
// 没有 /usr/sbin（实测是 /usr/gnu/bin:/usr/local/bin:/bin:/usr/bin:.）。
// 只写名字的话 proc::run 找不到它，探测直接失败——**而失败是静默的**，
// 表现就是"未探测到显卡，按 12 GB 估算"，一台 128 GB 的 Mac 被当成 12 GB。
// 2026-09-11 就是这么查出来的：Metal 明明起来了，显存却一直是 12 GB。
constexpr const char* kSysctl = "/usr/sbin/sysctl";
constexpr const char* kVmStat = "/usr/bin/vm_stat";

/// Apple Silicon 的「显存」。
///
/// **这台机器上没有 nvidia-smi，而以前的代码只认它**：探不到就退回
/// "按 12 GB 估算"。于是一台 128 GB 的 Mac 被当成 12 GB，档位、权重放哪、
/// "显存够就不用清理"全部按 12 GB 算——而这**不报错**，只是什么都跑不大。
///
/// 苹果芯片是统一内存：CPU 和 GPU 共用同一块，没有独立显存这回事。
/// GPU 能用多少由 `iogpu.wired_limit_pct` 决定，默认不是全部——
/// 系统自己要留一份。读得到就用它，读不到按 75% 算（苹果文档里
/// recommendedMaxWorkingSetSize 在这一档附近）。
///
/// **Intel Mac 不走这条**：那些机器要么是独显（另说），要么核显性能
/// 根本跑不动这套东西，按统一内存算会得出一个大得离谱的数。
std::optional<GPUInfo> detect_apple_gpu() {
    const bool dbg = !paths::env("CHANGJI_DEBUG_HW").empty();
    const auto sysctl_num = [dbg](const char* key) -> std::optional<std::uint64_t> {
        auto r = proc::run(kSysctl, {"-n", key}, 5000);
        if (dbg) {
            std::fprintf(stderr, "[hw] sysctl %s: launched=%d exit=%d out=[%s]\n",
                         key, static_cast<int>(r.launched), r.exit_code,
                         r.out.c_str());
        }
        if (!r.launched || r.exit_code != 0) return std::nullopt;
        std::string t;
        for (char c : r.out) {
            if (std::isdigit(static_cast<unsigned char>(c))) t += c;
        }
        if (t.empty()) return std::nullopt;
        try {
            return std::stoull(t);
        } catch (...) {
            return std::nullopt;
        }
    };

    // 芯片名。拿不到就给个能看的兜底，不影响算数。
    std::string name = "Apple Silicon";
    auto br = proc::run(kSysctl, {"-n", "machdep.cpu.brand_string"}, 5000);
    if (dbg) {
        std::fprintf(stderr, "[hw] brand: launched=%d exit=%d out=[%s]\n",
                     static_cast<int>(br.launched), br.exit_code, br.out.c_str());
    }
    if (auto& r = br; r.launched && r.exit_code == 0) {
        std::string t = r.out;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                              t.back() == ' ')) {
            t.pop_back();
        }
        // 只认苹果自家的芯片。Intel Mac 的 brand_string 是 "Intel(R) Core..."，
        // 那种机器不该按统一内存算。
        if (t.rfind("Apple", 0) != 0) return std::nullopt;
        if (!t.empty()) name = t;
    } else {
        return std::nullopt;
    }

    const auto total = sysctl_num("hw.memsize");
    if (!total.has_value() || *total == 0) return std::nullopt;

    // GPU 能用的那一份。iogpu.wired_limit_pct 是百分数，0 表示"系统自己定"。
    double pct = 75.0;
    if (const auto p = sysctl_num("iogpu.wired_limit_pct");
        p.has_value() && *p > 0 && *p <= 100) {
        pct = static_cast<double>(*p);
    }

    GPUInfo g;
    g.name = name + "（统一内存）";
    const double usable_bytes = static_cast<double>(*total) * pct / 100.0;
    g.vram_mb = static_cast<int>(usable_bytes / (1024.0 * 1024.0));
    g.count = 1;
    return g;
}
#endif

std::optional<GPUInfo> detect_gpu() {
#if defined(__APPLE__)
    // 苹果机器优先按统一内存算。**放在 nvidia-smi 之前**：有人在 Mac 上
    // 装过 nvidia 的工具链，那时候 nvidia-smi 在但没有 N 卡，
    // 探出来的是空的，反而把统一内存那条盖掉。
    if (auto apple = detect_apple_gpu(); apple.has_value()) return apple;
#endif
    if (!proc::which("nvidia-smi")) return std::nullopt;

    auto r = proc::run("nvidia-smi",
                       {"--query-gpu=name,memory.total,driver_version",
                        "--format=csv,noheader,nounits"},
                       15000);
    if (!r.launched) return std::nullopt;
    return parse_gpu_query(r.out);
}

std::optional<double> parse_free_vram(const std::string& out) {
    // 一张卡一行，只认第一行（卡 0）。**多卡时这个数没有意义**——
    // 我们的工作进程绑一张卡，而 nvidia-smi 不知道绑的是哪张，
    // 所以多卡直接回 nullopt，让调用方退回静态估算。
    // 一行都没有就没什么可解析的。
    const std::string first = out.substr(0, out.find('\n'));
    std::string trimmed;
    for (char c : first) {
        if (!std::isspace(static_cast<unsigned char>(c))) trimmed += c;
    }
    if (trimmed.empty()) return std::nullopt;
    // 末尾可能带 " MiB"，nounits 时没有；两种都吃。
    std::size_t digits = 0;
    while (digits < trimmed.size() &&
           std::isdigit(static_cast<unsigned char>(trimmed[digits]))) {
        ++digits;
    }
    if (digits == 0) return std::nullopt;
    try {
        const double mib = std::stod(trimmed.substr(0, digits));
        if (mib <= 0) return std::nullopt;
        return mib / 1024.0;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> parse_vm_stat(const std::string& out) {
    if (out.empty()) return std::nullopt;

    // 头一行： "Mach Virtual Memory Statistics: (page size of 16384 bytes)"
    std::uint64_t page = 0;
    if (const auto pos = out.find("page size of"); pos != std::string::npos) {
        std::string digits;
        for (std::size_t i = pos; i < out.size() && out[i] != ')'; ++i) {
            if (std::isdigit(static_cast<unsigned char>(out[i]))) digits += out[i];
        }
        if (!digits.empty()) {
            try {
                page = std::stoull(digits);
            } catch (...) {
                page = 0;
            }
        }
    }
    if (page == 0) return std::nullopt;   // 读不到页大小就别猜

    const auto pages_of = [&out](const char* label) -> std::uint64_t {
        const auto pos = out.find(label);
        if (pos == std::string::npos) return 0;
        std::string digits;
        for (std::size_t i = pos + std::strlen(label); i < out.size(); ++i) {
            const char ch = out[i];
            if (std::isdigit(static_cast<unsigned char>(ch))) {
                digits += ch;
            } else if (!digits.empty()) {
                break;      // 数字读完了（后面是那个句点）
            } else if (ch == '\n') {
                break;      // 这一行压根没有数
            }
        }
        if (digits.empty()) return 0;
        try {
            return std::stoull(digits);
        } catch (...) {
            return 0;
        }
    };

    // 见头文件：只数 free 是不够的。
    const std::uint64_t usable = pages_of("Pages free:") +
                                 pages_of("Pages inactive:") +
                                 pages_of("Pages purgeable:") +
                                 pages_of("Pages speculative:");
    if (usable == 0) return std::nullopt;
    return static_cast<double>(usable) * static_cast<double>(page) /
           (1024.0 * 1024.0 * 1024.0);
}

namespace {
#if !defined(__APPLE__)

/// NVML 那个内存结构。**自己声明，不引 nvml.h**：那个头跟着 CUDA toolkit
/// 走，而我们要的是"装了驱动就能用"，不是"装了 toolkit 才编得过"。
/// 字段顺序和类型是 NVML 的 ABI，动不得。
struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

/// 直接问驱动要显存，**不 fork**。
///
/// 原来只有一条路：fork + exec 去跑 nvidia-smi。两个毛病：
///
///   1. **这个进程初始化 CUDA 之后映射着几十 GB**，在这种进程里 fork 是
///      NVIDIA 明确不支持的做法。问不到的时候调度器就退回保守估算，
///      于是"显存够就不清理"在真机上可能等于从来没生效过——2026-09-11
///      在 96 GB 卡上看到的正是"每次出片都把大模型踢掉"。
///   2. **一次一百毫秒上下。** 它在每次借槽、每次采样第一步的路径上。
///
/// NVML 是驱动自带的库（装了 NVIDIA 驱动就有，不用装 CUDA toolkit），
/// 进程内调，微秒级，不 fork。
///
/// **运行时加载、链接期不依赖**：Mac、纯 CPU 的 Linux、没显卡的 Windows
/// 都得照样起得来。加载不上就回 nullopt，调用方退回 nvidia-smi 那条老路
/// ——这里只是**加了一条更快更稳的路，没有拆掉任何东西**。
class Nvml {
public:
    /// 总量和空闲，**一次问出来的**。加载不上、初始化失败、拿不到卡，
    /// 一律 nullopt。
    static std::optional<VramTotals> totals() {
        // **每次都看一眼那个开关，不是只在构造时看。** 只在构造时看的话，
        // 第一次问过之后再设就没用了——而"出了岔子一键关掉"要的正是
        // 随时能关。getenv 比 dlopen 便宜得多，放在这条路上不心疼。
        if (!paths::env("CHANGJI_NO_NVML").empty()) return std::nullopt;
        static Nvml one;   // C++11 起，局部静态的初始化是线程安全的
        if (!one.ok_) return std::nullopt;
        void* dev = nullptr;
        if (one.handle_(0, &dev) != 0 || dev == nullptr) return std::nullopt;
        NvmlMemory mem{};
        if (one.mem_(dev, &mem) != 0) return std::nullopt;
        // total == 0 说明这结构没被填上，别拿它当"空闲 0 字节"用——
        // 那会让调度器以为卡满了，每次都去卸模型。
        if (mem.total == 0) return std::nullopt;
        constexpr double kGb = 1024.0 * 1024 * 1024;
        return VramTotals{static_cast<double>(mem.total) / kGb,
                          static_cast<double>(mem.free) / kGb};
    }

private:
    using InitFn = int (*)();
    using HandleFn = int (*)(unsigned int, void**);
    using MemFn = int (*)(void*, NvmlMemory*);

#if defined(_WIN32)
    using LibHandle = HMODULE;
    static LibHandle open_lib() {
        if (LibHandle h = ::LoadLibraryA("nvml.dll")) return h;
        // 老驱动把它装在这儿，不在 System32。
        return ::LoadLibraryA(
            "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }
    template <class T>
    T sym(const char* n) const {
        return reinterpret_cast<T>(::GetProcAddress(lib_, n));
    }
#else
    using LibHandle = void*;
    static LibHandle open_lib() {
        // 带版本号那个才是驱动装的实文件；不带的是 -dev 包里的软链，
        // 没装开发包的机器上不存在。两个都试。
        if (LibHandle h = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL)) {
            return h;
        }
        return ::dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
    }
    template <class T>
    T sym(const char* n) const {
        return reinterpret_cast<T>(::dlsym(lib_, n));
    }
#endif

    Nvml() {
        lib_ = open_lib();
        if (!lib_) return;
        // _v2 是现在的名字；老驱动上只有不带后缀那个。
        InitFn init = sym<InitFn>("nvmlInit_v2");
        if (!init) init = sym<InitFn>("nvmlInit");
        handle_ = sym<HandleFn>("nvmlDeviceGetHandleByIndex_v2");
        if (!handle_) handle_ = sym<HandleFn>("nvmlDeviceGetHandleByIndex");
        mem_ = sym<MemFn>("nvmlDeviceGetMemoryInfo");
        if (!init || !handle_ || !mem_) return;
        // **不配 nvmlShutdown。** 这个对象活到进程结束；中途关掉的话
        // 下次问又要重新初始化（几十毫秒），而它在借槽的关键路径上。
        ok_ = init() == 0;
    }

    LibHandle lib_ = nullptr;
    HandleFn handle_ = nullptr;
    MemFn mem_ = nullptr;
    bool ok_ = false;
};

#endif  // !__APPLE__
}  // namespace

std::optional<double> free_vram_gb() {
#if defined(__APPLE__)
    // 统一内存：能用的系统内存就是能用的"显存"。
    // nvidia-smi 在这台机器上不存在，不走这条的话调度器永远拿不到实时
    // 空闲量，只能按保守估算办事——也就是每次切阶段都卸一个模型。
    if (auto r = proc::run(kVmStat, {}, 5000);
        r.launched && r.exit_code == 0) {
        if (auto gb = parse_vm_stat(r.out); gb.has_value()) return gb;
    }
    return std::nullopt;
#endif
    // **先问 NVML**（进程内、不 fork、微秒级），问不到再走老路。
    // 理由写在 Nvml 上面。
    if (auto t = Nvml::totals(); t.has_value()) return t->free_gb;
    if (!proc::which("nvidia-smi")) return std::nullopt;
    // **超时要短。** 这个函数在每次借槽的路径上，卡住比问不到更糟；
    // 问不到只是退回静态估算。
    auto r = proc::run(
        "nvidia-smi",
        {"--query-gpu=memory.free", "--format=csv,noheader,nounits"}, 5000);
    if (!r.launched || r.exit_code != 0) return std::nullopt;
    return parse_free_vram(r.out);
}

std::optional<VramTotals> vram_totals_gb() {
#if defined(__APPLE__)
    // 统一内存这边没有"整卡多大"这个说法对得上 NVML 的语义，
    // 就不硬凑一个。调用方拿不到就走原来那条两次问的老路。
    return std::nullopt;
#else
    if (auto t = Nvml::totals(); t.has_value()) return t;
    // 退路：一次 nvidia-smi 同时要两个数。**一次，不是两次**——
    // 分两次问的话两个数来自两个时刻，差值就不是这个槽占的。
    if (!proc::which("nvidia-smi")) return std::nullopt;
    auto r = proc::run("nvidia-smi",
                       {"--query-gpu=memory.total,memory.free",
                        "--format=csv,noheader,nounits"},
                       5000);
    if (!r.launched || r.exit_code != 0) return std::nullopt;
    const std::string first = r.out.substr(0, r.out.find('\n'));
    const std::size_t comma = first.find(',');
    if (comma == std::string::npos) return std::nullopt;
    const auto total = parse_free_vram(first.substr(0, comma));
    const auto free = parse_free_vram(first.substr(comma + 1));
    if (!total.has_value() || !free.has_value() || *total <= 0.0) {
        return std::nullopt;
    }
    return VramTotals{*total, *free};
#endif
}

std::optional<GPUInfo> parse_gpu_query(const std::string& out) {
    if (out.empty()) return std::nullopt;
    // **卡数 = 非空行数。** nvidia-smi 一张卡一行。
    // 只读第一行取显存是对的（那是卡 0 的），但"有几张"以前根本没人问。
    int gpu_count = 0;
    for (std::size_t i = 0; i < out.size();) {
        const std::size_t nl = out.find('\n', i);
        const std::size_t end = nl == std::string::npos ? out.size() : nl;
        const std::string line = out.substr(i, end - i);
        if (line.find_first_not_of(" \t\r") != std::string::npos) ++gpu_count;
        if (nl == std::string::npos) break;
        i = nl + 1;
    }

    const std::string first = out.substr(0, out.find('\n'));
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= first.size()) {
        const std::size_t comma = first.find(',', start);
        const std::size_t end = comma == std::string::npos ? first.size() : comma;
        std::string p = first.substr(start, end - start);
        // strip
        const auto b = p.find_first_not_of(" \t\r\n");
        const auto e = p.find_last_not_of(" \t\r\n");
        parts.push_back(b == std::string::npos ? "" : p.substr(b, e - b + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parts.size() < 2) return std::nullopt;

    GPUInfo info;
    info.name = parts[0];
    try {
        info.vram_mb = static_cast<int>(std::stod(parts[1]));
    } catch (...) {
        return std::nullopt;
    }
    if (parts.size() > 2 && !parts[2].empty()) info.driver = parts[2];
    info.count = gpu_count > 0 ? gpu_count : 1;
    return info;
}

std::map<Tier, TierSpec> tiers_for_vram(double vram_gb) {
    int idx = kTierRows - 1;
    for (int i = 0; i < kTierRows; ++i) {
        if (vram_gb >= kTierTable[i].threshold) { idx = i; break; }
    }
    const TierRow& row = kTierTable[idx];

    std::map<Tier, TierSpec> specs;
    for (Tier t : all_tiers()) {
        const int* v = row_for(row, t);
        // 统一规整到 32 的倍数。表里手写的数字可能不合规，
        // 而不是 32 的倍数会导致 Wan 的潜空间对不齐。
        const int w = round32(v[0]);
        const int h = round32(v[1]);
        TierSpec spec;
        spec.tier = t;
        spec.width = w;
        spec.height = h;
        spec.steps = v[2];
        spec.measured_seconds = estimate_seconds(t, w, h, v[2], row.threshold);
        specs[t] = spec;
    }
    return specs;
}

HardwareProfile HardwareProfile::detect(std::optional<double> override_vram_gb) {
    HardwareProfile p;
    p.gpu = detect_gpu();
    if (override_vram_gb.has_value()) {
        p.vram_gb = *override_vram_gb;
        p.detected = false;
    } else if (p.gpu.has_value()) {
        p.vram_gb = p.gpu->vram_gb();
        p.detected = true;
    } else {
        // 探测不到就按 12GB 这个偏保守的假设走，并明确标记未探测到
        p.vram_gb = 12.0;
        p.detected = false;
    }
    p.tiers = tiers_for_vram(p.vram_gb);
    return p;
}

std::string HardwareProfile::describe() const {
    std::string head;
    if (gpu.has_value()) {
        head = gpu->name + "，显存 " + fmt1(gpu->vram_gb()) + " GB";
        // 多卡的时候说一声。**说清楚显存是单卡的**——
        // 不然看到"8 张卡"很容易以为那 48 GB 是总数。
        if (gpu->count > 1) {
            head += "（共 " + std::to_string(gpu->count) + " 张，显存是单卡的）";
        }
    } else if (detected) {
        head = "显存 " + fmt1(vram_gb) + " GB";
    } else {
        head = "未探测到显卡，按 " + fmt1(vram_gb) + " GB 估算";
    }

    std::ostringstream os;
    os << head;
    for (Tier t : all_tiers()) {
        const auto it = tiers.find(t);
        if (it == tiers.end()) continue;
        const TierSpec& spec = it->second;
        const std::string secs = spec.measured_seconds.has_value()
                                     ? "约 " + fmt0(*spec.measured_seconds) + " 秒"
                                     : "未标定";
        os << "\n  " << pad_right(to_string(t), 8) << " " << spec.width << "x"
           << spec.height << "  " << spec.steps << " 步  单镜 " << secs;
    }
    return os.str();
}

std::optional<double> HardwareProfile::estimate_episode(int shot_count,
                                                        Tier tier) const {
    const auto it = tiers.find(tier);
    if (it == tiers.end() || !it->second.measured_seconds.has_value()) {
        return std::nullopt;
    }
    return *it->second.measured_seconds * shot_count;
}

}  // namespace changji::models
