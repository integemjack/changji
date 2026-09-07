// 环境体检。
//
// 跑之前先把所有外部依赖查一遍，缺什么直接说清楚怎么补。
// 目标是让用户永远不会在跑到一半的时候才发现缺东西。
//
// 对齐 Python 侧 src/changji/doctor.py。响应结构体必须完全一致：
// 前端只认 can_run 和 checks 数组里的 name/level/detail，检查项本身可以增删。

#pragma once

#include <string>
#include <vector>

#include "config/settings.hpp"

namespace changji::doctor {

enum class Level { OK, WARN, FAIL };

/// 序列化用的字符串，必须是 "ok" / "warn" / "fail"——前端按这三个值上色。
const char* to_string(Level level);

struct Check {
    std::string name;
    Level level = Level::OK;
    std::string detail;
    std::string fix;  ///< 怎么补。空表示不需要动作
};

struct Report {
    std::vector<Check> checks;

    /// 能不能跑完整流程。有任何一项 FAIL 就是不能。
    bool can_run() const;
};

/// 跑一遍全部检查。
///
/// 注意这些检查是串行的，其中三项要发网络请求，各自超时 8 秒，
/// 最坏情况这个函数会阻塞二十多秒。Python 侧也是同样的行为。
/// TODO(阶段 2): 三个网络检查改成并发，把最坏情况压到单次超时。
Report run_checks(const config::Settings& settings);

}  // namespace changji::doctor
