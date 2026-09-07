#include "diff.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace changji::compat {

namespace {

std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < p.size()) {
        if (p[i] == '/') ++i;
        const std::size_t j = p.find('/', i);
        if (j == std::string::npos) {
            if (i < p.size()) out.push_back(p.substr(i));
            break;
        }
        out.push_back(p.substr(i, j - i));
        i = j;
    }
    return out;
}

/// 键里有 / 的话要转义，否则路径会被切错。JSON Pointer 的规矩。
std::string escape_segment(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

std::string type_name(const json& j) {
    if (j.is_number_integer() || j.is_number_unsigned() || j.is_number_float()) {
        return "number";   // 整浮点不算类型差异，见文件头
    }
    return j.type_name();
}

std::string brief(const json& j, std::size_t max = 120) {
    const std::string s = j.dump();
    if (s.size() <= max) return s;
    return s.substr(0, max) + "…";
}

bool numbers_equal(const json& a, const json& b, double tol) {
    const double x = a.get<double>();
    const double y = b.get<double>();
    if (x == y) return true;
    const double scale = std::max({1.0, std::abs(x), std::abs(y)});
    return std::abs(x - y) <= tol * scale;
}

void walk(const json& expected, const json& actual, const std::string& path,
          const CompareOptions& opts, std::vector<Difference>& out);

void walk_object(const json& expected, const json& actual,
                 const std::string& path, const CompareOptions& opts,
                 std::vector<Difference>& out) {
    // 键的并集，两边各查一遍。只遍历一边的话，多出来的键查不出来——
    // 而多一个键同样是破契约：前端可能按 in 判断字段存不存在。
    std::set<std::string> keys;
    for (const auto& kv : expected.items()) keys.insert(kv.key());
    for (const auto& kv : actual.items()) keys.insert(kv.key());

    for (const auto& key : keys) {
        const std::string sub = path + "/" + escape_segment(key);
        const bool in_e = expected.contains(key);
        const bool in_a = actual.contains(key);
        if (in_e && in_a) {
            walk(expected[key], actual[key], sub, opts, out);
        } else if (in_e) {
            out.push_back({sub, "少了这个键，期望 " + brief(expected[key])});
        } else {
            out.push_back({sub, "多了这个键：" + brief(actual[key])});
        }
    }
}

void walk(const json& expected, const json& actual, const std::string& path,
          const CompareOptions& opts, std::vector<Difference>& out) {
    for (const auto& rule : opts.ignore) {
        if (path_matches(path, rule.pattern)) return;
    }

    if (type_name(expected) != type_name(actual)) {
        out.push_back({path, "类型不同：期望 " + type_name(expected) + "（" +
                                 brief(expected) + "），实际 " +
                                 type_name(actual) + "（" + brief(actual) + "）"});
        return;
    }

    if (expected.is_object()) {
        walk_object(expected, actual, path, opts, out);
        return;
    }
    if (expected.is_array()) {
        // **数组按下标比，顺序有关。** 镜头的先后就是成片的先后，
        // 把它当集合比的话，顺序反了也算通过。
        if (expected.size() != actual.size()) {
            out.push_back({path, "长度不同：期望 " +
                                     std::to_string(expected.size()) + "，实际 " +
                                     std::to_string(actual.size())});
            // 长度不同也要接着比公共部分——差异往往集中在头几个元素上，
            // 只报一句"长度不同"等于让人自己去找。
        }
        const std::size_t n = std::min(expected.size(), actual.size());
        for (std::size_t i = 0; i < n; ++i) {
            walk(expected[i], actual[i], path + "/" + std::to_string(i), opts,
                 out);
        }
        return;
    }
    if (expected.is_number()) {
        if (!numbers_equal(expected, actual, opts.float_tolerance)) {
            out.push_back({path, "值不同：期望 " + brief(expected) + "，实际 " +
                                     brief(actual)});
        }
        return;
    }
    if (expected != actual) {
        out.push_back(
            {path, "值不同：期望 " + brief(expected) + "，实际 " + brief(actual)});
    }
}

}  // namespace

bool path_matches(const std::string& path, const std::string& pattern) {
    const auto p = split_path(path);
    const auto q = split_path(pattern);

    // ** 能吃掉任意多段，所以要回溯。段数都不多，直接递归。
    const std::function<bool(std::size_t, std::size_t)> go =
        [&](std::size_t i, std::size_t j) -> bool {
        if (j == q.size()) return i == p.size();
        if (q[j] == "**") {
            for (std::size_t k = i; k <= p.size(); ++k) {
                if (go(k, j + 1)) return true;
            }
            return false;
        }
        if (i == p.size()) return false;
        if (q[j] != "*" && q[j] != p[i]) return false;
        return go(i + 1, j + 1);
    };
    return go(0, 0);
}

std::vector<Difference> compare(const json& expected, const json& actual,
                                const CompareOptions& opts) {
    std::vector<Difference> out;
    walk(expected, actual, "", opts, out);
    return out;
}

std::string format(const std::vector<Difference>& diffs, int max_lines) {
    if (diffs.empty()) return "一致";
    std::ostringstream os;
    os << diffs.size() << " 处差异：";
    int shown = 0;
    for (const auto& d : diffs) {
        if (shown++ >= max_lines) {
            os << "\n  …还有 " << (diffs.size() - static_cast<std::size_t>(shown) + 1)
               << " 处";
            break;
        }
        os << "\n  " << (d.path.empty() ? "（根）" : d.path) << "  " << d.detail;
    }
    return os.str();
}

}  // namespace changji::compat
