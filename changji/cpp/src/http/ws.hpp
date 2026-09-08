// WebSocket 广播。
//
// 进度推送的通道。REST 那 48 个接口原样保留，这里是加在旁边的第二条路，
// 不是替代——前端可以继续轮询，两种方式并存互为退路。
//
// 两条从 ComfyUI 客户端那边学来的教训，直接写进了这里的数据结构：
//
// 一，订阅按 job_id 记，不按连接记。ComfyUI 按 clientId 记订阅，同一个 id
//     上并发跑两个任务，后连的会把先连的挤下线，先连的永远等不到完成消息。
//     这里一个连接可订阅多个 job，一个 job 可被多个连接订阅，是多对多。
//
// 二，断线不是致命错误。每个 job 的最新状态另外留在 job 表里，
//     REST 仍然查得到完整状态。前端 WS 掉线就退回轮询，重连后先拉一次
//     全量再续订阅，任务本身不受连接影响。

#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace crow::websocket {
struct connection;
}

namespace changji::ws {

/// 连接注册表与广播器。
///
/// 线程模型：Crow 的事件循环线程负责收发，流水线工作线程负责产生消息。
/// 所有公开方法都可以从任意线程调用，内部一把大锁。
/// 这把锁保护的临界区都很短（改集合、发一个已序列化的字符串），
/// 不值得为它做分段锁。
class Hub {
public:
    void add(crow::websocket::connection* conn);
    void remove(crow::websocket::connection* conn);

    /// 订阅。`job_id` 可以是**具体的任务 id**，也可以是**任务类别**
    /// （"run" / "write"）。
    ///
    /// **按类订阅不是方便，是唯一可行的方式。** 具体的 job_id 由
    /// `new_job_id` 随机生成，而且从来不从任何接口暴露出去——
    /// `POST /api/run` 回 {started, queue}，`GET /api/run` 那些字段里也没有。
    /// 只认具体 id 的话，客户端永远订不上任何任务。
    void subscribe(crow::websocket::connection* conn, const std::string& job_id);
    void unsubscribe(crow::websocket::connection* conn, const std::string& job_id);

    /// 处理客户端上行消息。
    ///
    /// 上行只认 subscribe / unsubscribe 两种。业务指令一律走 REST，
    /// 上行通道越窄越好——WebSocket 上没有 HTTP 那套现成的鉴权、
    /// 幂等和错误语义，把写操作放进来是自找麻烦。
    void handle_client_message(crow::websocket::connection* conn,
                               const std::string& raw);

    /// 向订阅了该 job 的所有连接推一条消息。
    ///
    /// msg 必须已经带好 type 和 job_id 字段。
    /// type 为 progress 的消息会被节流；done / error 永远立即发送。
    void broadcast(const std::string& job_id, const nlohmann::json& msg);

    /// 广播给所有连接，不分 job。用于服务级事件（配置变更、服务即将关闭）。
    void broadcast_all(const nlohmann::json& msg);

    size_t connection_count();

    /// 有多少个连接订阅了这个 job。
    ///
    /// 加它一半是为了能测——`subs_` 是私有的，不然订阅有没有生效
    /// 只能靠 broadcast 观察，而 broadcast 要解引用 connection，
    /// 拿假指针就崩了。另一半是诊断：没人听的时候值不值得去凑那条消息。
    size_t subscriber_count(const std::string& job_id);

    /// 这条消息该不该被节流掉。
    ///
    /// **放在 public 是为了能测。** 它本来是私有的，只有 `broadcast` 调；
    /// 但 `broadcast` 要解引用 `connection*` 才能发消息，拿假指针会崩，
    /// 于是这条规则一直没有任何测试。而它错了的后果很重：
    /// **done / error 一旦被节流掉，前端会一直显示「生成中」**，
    /// 直到用户手动刷新——比进度条不流畅严重得多。
    ///
    /// ⚠️ 调用方要自己持锁：它读写 `last_sent_`，而 `broadcast` 已经
    /// 拿着 `mu_` 了，这里再锁一次就是自锁。
    bool should_throttle(const std::string& job_id, const std::string& type);

private:

    std::mutex mu_;
    std::set<crow::websocket::connection*> conns_;
    std::map<std::string, std::set<crow::websocket::connection*>> subs_;
    /// 节流状态：job_id -> 上次发送时刻。
    /// 逐步回调每秒可能触发几十次，直接透传会把前端淹掉。
    std::map<std::string, std::chrono::steady_clock::time_point> last_sent_;
};

/// 进度消息的节流间隔。
inline constexpr auto kThrottleInterval = std::chrono::milliseconds(200);

/// 全局单例。流水线各处都要推进度，逐层传引用不划算。
Hub& hub();

}  // namespace changji::ws
