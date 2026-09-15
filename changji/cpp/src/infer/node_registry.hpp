#pragma once

// 那张「机器 × 能力」的表，后端这一半。
//
// 表里有谁：**本机自己（叫 `local`）**，加上 `[[peer.nodes]]` 里配的那些。
// 本机也是一行，不是特例——不这样的话"本地 vs 远程"永远是两条代码路径，
// 界面和调度都统一不了。
//
// 每台的「能不能」是问出来的（`GET /status`），「准不准」是配置里那几个
// `off`，「该不该」交给 `node_pick.hpp`。三层分得很干净，这一层只负责
// **把事实收齐**。
//
// 缓存 + 按需刷新：问一遍要发几个 HTTP，页面每秒刷一次的话纯属给对面
// 添乱；而缓存太久的话，一台刚上线的机器要等半天才出现在表上。

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "infer/node_pick.hpp"

namespace changji::infer {

class NodeRegistry {
public:
    /// 默认缓存多久。5 秒：页面点开时基本是新的，连着刷也不会把对面问烦。
    static constexpr std::chrono::seconds kDefaultMaxAge{5};

    /// 拿一份快照，超过 `max_age` 就先刷一遍。
    std::vector<NodeState> snapshot(
        const config::Settings& s,
        std::chrono::seconds max_age = kDefaultMaxAge);

    /// 不管缓存，现在就问一遍。配置改了之后要调它。
    void refresh(const config::Settings& s);

private:
    mutable std::mutex mu_;
    std::vector<NodeState> nodes_;
    std::chrono::steady_clock::time_point fetched_at_{};
};

/// 进程内那一份。
NodeRegistry& node_registry();

/// 那张表，给界面的形状。
///
/// 每台一行，每个能力一格，格子里带"为什么干不了"——**那句话是用户唯一
/// 的线索**，界面上悬停就能看见。
nlohmann::json nodes_json(const config::Settings& s);

/// 同上，但**不问任何机器**：拼那张表这一段是纯的，单独拎出来好撞。
/// 上面那个 = `snapshot()` 去问一圈 + 这个。
nlohmann::json nodes_json(const std::vector<NodeState>& nodes);

}  // namespace changji::infer
