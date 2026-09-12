#pragma once

// 工作进程池：把出图和出片派给别的进程去算。
//
// **为什么。** 见方案「多卡和多机怎么用起来」。一句话：sd.cpp 的进度回调是
// 全局的，同进程两个生成会互相串——同种模型的多实例只能靠多进程。
//
// **换实现的接缝早就在了。** `FrameRenderer` / `VideoRenderer` 是
// `std::function`，ComfyUI 那条路就是从这里换的实现，而那本质上就是
// "把渲染交给另一个进程"。这里是同一个接缝上的第三种实现，
// 只是另一头跑的是我们自己的 sd.cpp。
//
// **协调者这一侧不改状态。** 池只负责"把活派出去、把结果拿回来"，
// `Shot` 仍然由阶段那一层在协调者的线程上顺序改、顺序存盘。
// 并行的是推理，不是状态改动——这条守住了，单一写者就不破。

#include <memory>
#include <string>
#include <vector>

#include <functional>

#include "infer/worker_proto.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"

namespace changji::infer {

/// 池子里代表**本机进程内**的那个槽位的地址。
///
/// **本机是池里正经的一个槽，不是特例。** 不这样的话，只要配了一台远程
/// 机器，本机那张卡就整个闲着——而那正是"本地还是远程"被做成两条代码
/// 路径的后果。这个约定串让它变成同一条路上的一个取值。
inline constexpr const char* kLocalEndpoint = "local";

/// 在本机进程内把任务跑完。由调用方注入。
///
/// **做成回调而不是直接调 `run_task_locally`**：那个函数要 `Settings`，
/// 而这个文件是给"把活发出去"用的，不该知道配置长什么样。理由和
/// `WorkerFarm::HealthProbe` 那处一样。
using LocalRunner = std::function<TaskResult(
    const Task&, const StepCallback&, pipeline::CancelToken&)>;

/// 一个工作进程的地址。
struct WorkerEndpoint {
    /// `http://127.0.0.1:9001` 这样。
    std::string url;
    /// 派活时带的口令，对应那台的 `[peer].token`。
    ///
    /// **本机自己拉起的那些不用填**：它们听回环，那边根本不查
    /// （见 `infer/peer_auth.hpp`）。跨机才要。
    std::string token;
};

/// 池。**线程安全**：阶段那一层会从多个线程同时借。
class WorkerPool {
public:
    explicit WorkerPool(std::vector<WorkerEndpoint> endpoints,
                        LocalRunner local_runner = {});
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// 有几个工作进程。阶段那一层拿它当并发上限。
    std::size_t size() const;

    /// 挨个 ping 一遍，回活着的个数。起跑前查一次，
    /// **别等跑到一半才发现有一个是死的**。
    std::size_t alive() const;

    /// 出图。签名和 `stages::sd_renderer()` 回的那个一样，可以直接顶替。
    stages::FrameRenderer frame_renderer();

    /// 出片。同上。
    stages::VideoRenderer video_renderer();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 从配置里那几个地址造一个池。地址为空回 nullptr——
/// **调用方看到 nullptr 就走进程内那条路**，行为和以前一模一样。
///
/// `token` 会带给每一个地址。**一个口令走遍自己这几台**是刻意的：
/// 都是自己的机器，每台配一个不同的口令只是给自己添麻烦。
/// `local_runner` 给了的话，`endpoints` 里那个 `kLocalEndpoint` 就不发
/// HTTP，直接在本进程里跑。没给而地址里又有 `local` 的话，那个槽会被
/// 当成一台连不上的机器——所以两者要一起给。
std::shared_ptr<WorkerPool> make_worker_pool(
    const std::vector<std::string>& endpoints, const std::string& token = {},
    LocalRunner local_runner = {});

}  // namespace changji::infer
