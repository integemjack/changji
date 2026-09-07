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

}  // namespace changji::util
