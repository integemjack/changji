#include "models/hardware.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "util/proc.hpp"

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

std::optional<GPUInfo> detect_gpu() {
    if (!proc::which("nvidia-smi")) return std::nullopt;

    auto r = proc::run("nvidia-smi",
                       {"--query-gpu=name,memory.total,driver_version",
                        "--format=csv,noheader,nounits"},
                       15000);
    if (!r.launched || r.out.empty()) return std::nullopt;

    const std::string first = r.out.substr(0, r.out.find('\n'));
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
