#pragma once

// 工作进程：一张卡，一次一个任务。
//
// 起法：`changji --worker --gpu 0 --port 9001`
//
// **它就是现在这个单卡程序，只是不发接口、不碰项目文件。**
// 自己的 `Scheduler`、自己的显存预算、自己的 `ActiveGeneration`——
// 进程边界把"每卡一份预算"白送了，所以 `Scheduler` 一行都不用改。
//
// 四条路由，够用就行：
//
//     POST /task            派一个任务   → 202 {id}，忙着回 409
//     GET  /task/{id}       查进度       → {state, step, steps, loading, result}
//     POST /task/{id}/cancel 取消
//     GET  /health          活着没       → {gpu, busy}
//
// **忙着的时候回 409，不排队。** 排队会让协调者那边的并发上限失效——
// 它以为派出去的都在跑，实际上有几个在对面排着。

#include <string>

#include "config/settings.hpp"

namespace changji::infer {

struct WorkerOptions {
    int port = 9001;
    /// 用哪张卡。**通过 CUDA_VISIBLE_DEVICES 生效**——
    /// 这一条在只有一张卡的机器上验不了，见方案里那三件要先验的事。
    int gpu = 0;
    std::string host = "127.0.0.1";
};

/// 起一个工作进程。阻塞到进程结束。
///
/// **回 false 表示压根没起**：对外监听而 `[peer].token` 没设时会当场
/// 拒绝（见 `infer/peer_auth.hpp`），拒绝的理由已经打到 stderr 了。
/// 调用方要把它变成一个非零退出码——起没起来必须能从退出码看出来，
/// 不然拿脚本拉起一堆工作进程时，"没起来"和"起来了"长得一模一样。
bool run_worker(const config::Settings& settings, const WorkerOptions& opts);

}  // namespace changji::infer
