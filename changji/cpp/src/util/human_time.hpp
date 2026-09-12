#pragma once

// 把秒说成人话。
//
// 「还剩 1847 秒」没人愿意在脑子里除一遍。这在 6GB 卡上尤其要紧——
// 一集跑几个小时，进度条上的数字是用户唯一能判断"该不该去睡"的依据。
//
// 移植自 src/changji/pipeline.py 的 human_time。**文案逐字对齐**：
// 这句话直接显示给用户，不是日志。

#include <string>

namespace changji::util {

/// 秒 -> "45 秒" / "12 分钟" / "3.5 小时"。负数按 0 算。
std::string human_time(double seconds);

/// 秒 -> "45.2 秒" / "1 分 4.0 秒" / "1 小时 3 分"。负数按 0 算。
///
/// **给"两个时长的对比"用的，不是给进度条用的。** human_time 在一分钟
/// 以上只留整分钟（`%.0f 分钟`），于是 57 秒、60 秒、64 秒全都显示成
/// 「1 分钟」——拿它写「A → B（目标 C）」这种句子，三个数长得一模一样，
/// 等于什么也没说。实测踩过：
///
///   [info] 按配音重排了镜头时长：1 分钟 → 1 分钟（目标 1 分钟）
///
/// 进度条那边照旧用 human_time：那里要的是"还得等多久"，一分钟以内的
/// 零头对"该不该去睡"没有意义，多一位反而更吵。
std::string human_time_precise(double seconds);

}  // namespace changji::util
