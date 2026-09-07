#pragma once

// 对拍用的深比较。
//
// 方案第六节：Python 引擎保留的唯一目的是当回归基准，删除之前必须完成对拍。
//
// ---
//
// **契约标准是结构兼容**：字段名、嵌套结构、取值、状态码一致，key 顺序不管。
// 所以这里：
//
//   对象按键比，顺序无关；
//   **数组按下标比，顺序有关**——镜头的先后就是成片的先后；
//   数字按值比，1 和 1.0 算同一个（JSON 不区分整浮点，Python 那边
//     一个字段是 int 还是 float 取决于它是怎么算出来的）；
//   null 和"字段不存在"是**两回事**——Python 的 opt_str 会吐 null，
//     少一个键是真的差异。
//
// ---
//
// **忽略一条就等于放弃那个字段的对拍。** 所以忽略项必须带理由，
// 而且理由要写在语料里、跟着差异一起打出来——不然半年后没人知道
// 当初为什么忽略它，也就没人敢删。

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::compat {

/// 一处差异。
struct Difference {
    /// JSON 指针式的位置，比如 /episodes/0/shots/2/duration_s。
    std::string path;
    std::string detail;
};

/// 一条忽略规则。
struct IgnoreRule {
    /// 路径模式。`*` 匹配一段，`**` 匹配任意多段。
    std::string pattern;
    /// **为什么可以忽略。** 空字符串不接受——见文件头。
    std::string why;
};

struct CompareOptions {
    std::vector<IgnoreRule> ignore;
    /// 浮点比较的相对容差。两边的算法一样但求值顺序可能不同，
    /// 完全相等的要求会在第十几位小数上炸。
    double float_tolerance = 1e-9;
};

/// 路径匹配。暴露出来是因为模式写错了不会报错，只会静默地少比或者多比。
bool path_matches(const std::string& path, const std::string& pattern);

/// 深比较。返回全部差异，空表示一致。
///
/// **不在第一处差异就停。** 一次跑完看到所有问题，比修一个跑一次快得多——
/// 对拍要起两个后端，一轮下来是分钟级。
std::vector<Difference> compare(const nlohmann::json& expected,
                                const nlohmann::json& actual,
                                const CompareOptions& opts = {});

/// 把差异列表拼成人能读的报告。
std::string format(const std::vector<Difference>& diffs, int max_lines = 40);

}  // namespace changji::compat
