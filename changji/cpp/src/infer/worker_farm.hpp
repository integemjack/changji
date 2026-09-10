#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "models/hardware.hpp"

namespace changji::infer {

/// 多卡时自己把每张卡的工作进程拉起来。
///
/// **"一个程序运行所有"不该等于"只能用一张卡"。** 用户启动的仍然是一个
/// 命令，多卡的编排由它自己做：探到 N 张卡就拉起 N 个 `--worker --gpu i`
/// 的子进程，把地址填进工作进程池，退出时收掉。
///
/// **为什么是子进程而不是进程内开 N 个上下文。** sd.cpp 的进度回调是
/// 全局的（`sd_set_progress_callback` 没有 user data 能区分是哪个上下文），
/// 进程内并发跑两张卡的话，两路进度会串到一起——而进度是判断"卡住没有"
/// 的唯一依据。另外 CUDA 设备是按进程选的，一个进程里换来换去容易出
/// 难复现的问题。子进程这条路把这两件事都绕开了，而且工作进程协议
/// 本来就有（多机部署用的是同一套）。
///
/// 拉起来的是**自己这一份可执行文件**（`paths::self_exe()`）。靠 PATH 找
/// "changji" 会找到别的构建——机器上常常有好几个 build- 目录，
/// 那时候症状是"工作进程行为和主进程对不上"，极难联想到是版本不同。
class WorkerFarm {
public:
    /// 按配置和这台机器的显卡拉起工作进程。
    ///
    /// 下面任何一条成立就**不拉，返回 nullptr**（调用方退回进程内单卡）：
    ///   - `[workers].endpoints` 非空：用户自己指定了，包括跨机的情况
    ///   - `[workers].auto_spawn = false`
    ///   - 只探到一张卡：单卡进程内跑更省事，也少一次进程间的图片搬运
    ///   - 取不到自己的可执行路径
    ///
    /// 起不来的那几张卡会被跳过（日志里说清是哪张）；一张都起不来时
    /// 同样返回 nullptr。**不抛异常**：多卡拉不起来该退回单卡继续干活，
    /// 而不是让整个服务起不来。
    /// `healthy(地址, 等几秒)` 用来确认某个工作进程真的能应答。
    /// **做成参数而不是写死**：真实那份要发 HTTP，而 HTTP 只链进主目标，
    /// 写死会让测试目标链不过。上面四种"不该动手"的情况根本走不到它。
    using HealthProbe = std::function<bool(const std::string&, int)>;

    static std::shared_ptr<WorkerFarm> start(
        const config::Settings& settings,
        const models::HardwareProfile& profile, HealthProbe healthy = {});

    ~WorkerFarm();
    WorkerFarm(const WorkerFarm&) = delete;
    WorkerFarm& operator=(const WorkerFarm&) = delete;

    /// 起来了的那几个的地址，直接喂给 `make_worker_pool`。
    const std::vector<std::string>& endpoints() const { return endpoints_; }

    /// 真正起来了几个。
    std::size_t size() const { return endpoints_.size(); }

private:
    WorkerFarm();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::string> endpoints_;
};

/// 第 `gpu` 张卡的工作进程该监听哪个端口。
///
/// 单独拿出来是为了能测：端口撞了的表现是"某张卡的工作进程起不来"，
/// 而日志里只有一句连不上，看不出是端口规则算错了。
int worker_port_for(int base_port, int gpu);

}  // namespace changji::infer
