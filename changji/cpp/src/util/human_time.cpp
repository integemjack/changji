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

}  // namespace changji::util
