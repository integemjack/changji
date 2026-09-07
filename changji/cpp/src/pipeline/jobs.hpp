#pragma once

// 任务表与工作线程池。
//
// 对应 Python 侧 web/server.py 里的 RunState 和 WriteState 两个任务槽。
// 那边是两个独立的 asyncio.Task 变量，WriteState 的注释明写着
// "跟跑流水线分开，两件事可以同时进行"——所以这里不能只有一条工作线程。
//
// 方案第三节定的是线程池加按类型限流：流水线最多 1 个、写作最多 1 个。
// 用池而不是两条固定线程，是为了以后想并发跑多集时不用改架构。
//
// 取消令牌**按 job 挂，不按线程挂**。/api/stop 和 /api/script/series/stop
// 各自取消对应的 job，互不影响。
//
// 这里**不引用 WebSocket**。消息往哪儿发是 main() 通过 set_sink() 注入的。
// 一是分层：流水线不该知道传输层存在；二是很实际的原因——ws.cpp 要链 Crow，
// 而单元测试目标没链，直接依赖的话这个文件就没法测。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace changji::pipeline {

/// 任务种类。一种一个槽，同种不能并发。
enum class JobKind {
    Run,    ///< 跑流水线，对应 RunState
    Write,  ///< 写整季 / 批量排分镜，对应 WriteState
};

const char* to_string(JobKind k);

/// 流水线事件。界面和命令行都靠它显示进度。
struct Event {
    double at = 0.0;      ///< Unix 秒，保留三位小数（对齐 Python 的 round(time(),3)）
    std::string stage;
    std::string kind;     ///< start / progress / shot_done / gate / warn / done / error
    std::string message;
    std::optional<std::string> shot_id;
    int current = 0;
    int total = 0;

    nlohmann::json to_json() const;
};

/// 取消令牌。
///
/// 协作式：工作线程要在耗时循环里主动查。Python 那边靠 asyncio 的
/// CancelledError 在 await 点抛出，效果一样但机制不同——
/// 这里没有 await 点，所以查询的位置要自己安排好。
///
/// 三个地方必须能被打断：sd.cpp 的采样回调、llama.cpp 的生成、ffmpeg 子进程。
class CancelToken {
public:
    void request() { cancelled_.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return cancelled_.load(std::memory_order_relaxed); }
    void reset() { cancelled_.store(false, std::memory_order_relaxed); }

private:
    std::atomic<bool> cancelled_{false};
};

/// 一个任务槽的状态。
///
/// 字段刻意和 Python 的 RunState / WriteState 并集对齐——两种任务的
/// 快照形状不同（见 snapshot()），但存在同一个结构里省得写两套。
struct JobState {
    bool running = false;
    std::string job_id;                    ///< 每次启动生成一个，WebSocket 按它订阅
    std::optional<std::string> episode_id;
    std::chrono::steady_clock::time_point started_at{};

    // 进度
    std::string stage;
    int current = 0;
    int total = 0;
    std::string message;

    // 产物与错误
    std::optional<std::string> output;
    std::vector<std::string> outputs;
    std::optional<std::string> error;

    // 一次跑多集时的队列进度。只跑一集时是 1/1。
    int queue_done = 0;
    int queue_total = 1;

    // Write 专用
    int done = 0;
    nlohmann::json episodes = nlohmann::json::array();

    /// 事件环。上限 500，对应 Python 的 deque(maxlen=500)。
    std::deque<Event> events;
};

/// 消息汇。job 表产生的进度和终止消息往这里送。
///
/// 默认什么都不做（命令行模式、单元测试都不需要推送）。
/// main() 里接到 ws::hub().broadcast 上。
using Sink = std::function<void(const std::string& job_id, const nlohmann::json& msg)>;

/// 任务表。所有公开方法可从任意线程调用。
class JobTable {
public:
    JobTable();
    ~JobTable();

    JobTable(const JobTable&) = delete;
    JobTable& operator=(const JobTable&) = delete;

    /// 任务体。收到取消令牌和一个上报进度的回调。
    using Body = std::function<void(CancelToken&, const std::function<void(Event)>&)>;

    /// 启动一个任务。同种已经在跑就返回 false，调用方回 409。
    ///
    /// episode_id 只是给快照显示用的，不影响调度。
    bool start(JobKind kind, const std::string& episode_id, Body body);

    /// 请求取消。没在跑返回 false，对应 Python 的 {"stopped": false}。
    bool cancel(JobKind kind);

    /// 快照。形状与 Python 侧对应的 State.snapshot() 一致。
    nlohmann::json snapshot(JobKind kind) const;

    bool running(JobKind kind) const;
    std::string job_id(JobKind kind) const;

    /// 等所有在跑的任务结束。析构和优雅关停时用。
    void wait_idle();

    /// 装消息汇。要在起任务之前调用。
    void set_sink(Sink s);

private:
    struct Slot {
        JobState state;
        CancelToken token;
        std::thread worker;
        /// 线程是否还在跑。
        ///
        /// 跟 state.running 不是一回事：手动停止后 running 立刻变 false
        /// （对齐 Python 的可观测行为，见 cancel()），但线程要跑到下一个
        /// 取消检查点才退。析构和 wait_idle() 要等的是**这个**，
        /// 等 running 的话会在线程还活着的时候就返回，然后析构掉它正在用的成员。
        bool active = false;
    };

    Slot& slot(JobKind k);
    const Slot& slot(JobKind k) const;
    void record(JobKind kind, Event ev);
    void emit(const std::string& job_id, const nlohmann::json& msg) const;

    mutable std::mutex mu_;
    Sink sink_;
    std::condition_variable idle_cv_;
    Slot run_;
    Slot write_;
};

/// 全局单例。接口层各处都要查状态和起任务，逐层传引用不划算。
JobTable& jobs();

// 手动停止时写进 error 的文案。**两种任务不是同一句**，别合并。
//
// 逐字抄 Python。这两句是契约——不是给开发看的日志，是前端直接显示给
// 用户的话。而且各自说的是各自的事：跑流水线保留的是镜头，
// 写作保留的是已经写完的集。合成一句必然有一半用户看着不对。
inline constexpr const char* kRunStoppedMessage =
    "已手动停止。已完成的镜头会保留，下次从这里继续。";
inline constexpr const char* kWriteStoppedMessage =
    "已手动停止。已经写好的几集留着。";

/// 取对应种类的停止文案。
const char* stopped_message(JobKind k);

/// 事件环上限。Python 那边是 deque(maxlen=500)。
inline constexpr std::size_t kMaxEvents = 500;
/// 快照里回传的事件条数。Python 那边是 list(events)[-80:]。
inline constexpr std::size_t kSnapshotEvents = 80;

}  // namespace changji::pipeline
