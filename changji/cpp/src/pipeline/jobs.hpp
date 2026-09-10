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

    /// 手动停止时写进 error 的话。空表示用这一类的默认值。
    std::string stop_message;
};

/// 消息汇。job 表产生的进度和终止消息往这里送。
///
/// 默认什么都不做（命令行模式、单元测试都不需要推送）。
/// main() 里接到 ws::hub().broadcast 上。
using Sink = std::function<void(const std::string& job_id, const nlohmann::json& msg)>;

/// **一镜落定就调一次**，让上层把结果落盘。
///
/// 各阶段原来都是"整批跑完再统一写回 Shot、再存一次盘"。那在一批只有几镜
/// 的时候没问题，一集二十二镜、一镜两分钟的时候就是一个小时——这一个小时里：
///
///   * 制作页每六秒问一次 `/api/shots`，问到的永远是开跑那一刻的样子。
///     镜头墙上没有缩略图、没有可以点开看的片子，而用户要的正是
///     "已经生产的可以点击播放看效果"。
///   * 进程要是被杀掉（不是优雅停止），磁盘上躺着十几个 mp4，
///     project.json 里一条都没记——下次跑会当成没跑过，全部重来。
///
/// 给了就每出完一镜写回一次并调它；没给就是老行为（整批跑完再写回）。
/// 并发跑时这个调用**在锁里**，写回和存盘都串成一个写者。
using ShotCommit = std::function<void()>;

class JobTable;

/// 任务体能改的那部分状态。
///
/// 不直接把 JobState 交出去，是因为里面有一半字段只该由 job 表自己动
/// （running、job_id、started_at）。任务体拿到整个结构就迟早会去改它们，
/// 而改错的表现是"任务明明跑完了界面还转着圈"。
///
/// 所有方法都是线程安全的，可以从工作线程随便调。
class JobProgress {
public:
    /// 记一条事件：进环形缓冲、更新进度、广播出去。
    void report(Event ev);

    /// 只改一句话，不动进度条。跑长任务时"正在写第 3 集"这种。
    void set_message(std::string m);

    /// 完成数 / 总数。对应 Python 的 writing.done / writing.total。
    void set_done(int done);
    void set_total(int total);

    /// 追加一集的结果。写整季和批量出分镜都靠它，
    /// 每写完一集就往里加一条，界面能边跑边看。
    void add_episode(nlohmann::json ep);

    /// 产物路径。
    void set_output(std::string path);
    void add_output(std::string path);

    /// 当前跑到哪一集，以及队列进度。
    void set_episode_id(std::string id);
    void set_queue(int done, int total);

    /// 记一条错误。**不终止任务**——写整季时一集写砸了不该让前面几集白写。
    void set_error(std::string e);

    /// 该停了吗。耗时循环里要主动查。
    bool cancelled() const;

    /// 这个任务的取消令牌。
    ///
    /// 光有 cancelled() 不够：sd.cpp 的采样、llama.cpp 的生成、ffmpeg 子进程
    /// 都要拿到令牌**本身**才能被打断，它们不会回来问 JobProgress。
    /// 只给 cancelled() 的话，点停止要等当前这一镜跑完才有反应——
    /// 成片档一镜就是几分钟。
    ///
    /// 引用一直有效：令牌挂在 job 表的槽上，槽是表的成员，地址不变。
    CancelToken& token();

private:
    friend class JobTable;
    JobProgress(JobTable* t, JobKind k) : table_(t), kind_(k) {}
    JobTable* table_;
    JobKind kind_;
};

/// 任务表。所有公开方法可从任意线程调用。
class JobTable {
public:
    JobTable();
    ~JobTable();

    JobTable(const JobTable&) = delete;
    JobTable& operator=(const JobTable&) = delete;

    /// 任务体。拿到一个能改进度的句柄。
    using Body = std::function<void(JobProgress&)>;

    /// 启动一个任务。同种已经在跑就返回 false，调用方回 409。
    ///
    /// episode_id 只是给快照显示用的，不影响调度。
    ///
    /// stop_message 是手动停止时写进 error 的那句话。留空用这一类的默认值。
    /// 要能按任务指定，是因为同一个槽上跑的两件事说法不一样：
    /// 写整季停了是"已经写好的几集留着"，批量出分镜停了是
    /// "已经出好的分镜留着"。用同一句必然有一半场合是错的。
    bool start(JobKind kind, const std::string& episode_id, Body body,
               const std::string& stop_message = "");

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
    friend class JobProgress;
    void record(JobKind kind, Event ev);
    void emit(const std::string& job_id, const nlohmann::json& msg) const;
    /// 在锁里改一下这个槽的状态。JobProgress 的所有 setter 都走它。
    template <typename F>
    void mutate(JobKind kind, F&& fn);

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
