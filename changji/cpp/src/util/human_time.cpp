#include "util/human_time.hpp"

#include <algorithm>
#include <cstdio>

namespace changji::util {

namespace {

/// printf 的 %.Nf 用的是 IEEE 754 的默认舍入（就近取偶），
/// 和 Python 的 format spec 一致。所以这里不能自己写
/// `int(x + 0.5)` 那种四舍五入——恰好在 .5 上两边会给出不同的数。
std::string fmt(const char* spec, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

}  // namespace

std::string human_time(double seconds) {
    seconds = std::max(0.0, seconds);
    if (seconds < 60.0) return fmt("%.0f", seconds) + " 秒";
    const double minutes = seconds / 60.0;
    if (minutes < 60.0) return fmt("%.0f", minutes) + " 分钟";
    return fmt("%.1f", minutes / 60.0) + " 小时";
}

std::string human_time_precise_as(double seconds, double scale_ref) {
    seconds = std::max(0.0, seconds);
    scale_ref = std::max(0.0, scale_ref);
    if (scale_ref < 60.0) return fmt("%.1f", seconds) + " 秒";
    if (scale_ref < 3600.0) {
        const int mins = static_cast<int>(seconds / 60.0);
        const double rest = seconds - mins * 60.0;
        // 整分钟就别拖个 " 0.0 秒" 的尾巴。
        if (rest < 0.05) return std::to_string(mins) + " 分";
        return std::to_string(mins) + " 分 " + fmt("%.1f", rest) + " 秒";
    }
    const int hours = static_cast<int>(seconds / 3600.0);
    const int mins = static_cast<int>((seconds - hours * 3600.0) / 60.0);
    if (mins == 0) return std::to_string(hours) + " 小时";
    return std::to_string(hours) + " 小时 " + std::to_string(mins) + " 分";
}

std::string human_time_precise(double seconds) {
    return human_time_precise_as(seconds, seconds);
}

}  // namespace changji::util
