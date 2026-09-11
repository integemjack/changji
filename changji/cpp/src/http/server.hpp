// HTTP 服务。
//
// 接口契约必须与 Python 侧 src/changji/web/server.py 的 48 个接口
// 逐字节兼容。这是整个迁移能安全推进的前提：Node 前端零改动，
// 改一个地址就能在两个后端之间切换，随时能切回去。
//
// 不允许借重构之机顺手改接口。要改接口，等 Python 删掉之后再说。

#pragma once

#include <string>

#include "config/settings.hpp"

namespace changji::http {

struct Options {
    /// 默认只听回环。
    ///
    /// **原来是 0.0.0.0，也就是整个局域网都连得上。** 而这套接口没有任何
    /// 鉴权：连上就能读项目、改分镜、起流水线。桌面用户不会想到自己
    /// 起了个服务之后同一个 Wi-Fi 上的人都能操作它。
    ///
    /// Python 侧一直是 127.0.0.1（`run_server` 的默认值和 cli.py 的
    /// `--host` 都是），所以这也是一处对拍看不见的行为差异——
    /// 对拍比的是响应内容，比不到绑在哪个地址上。
    ///
    /// 容器里要 0.0.0.0 才连得进去，但那条路**本来就显式传了**
    /// （Dockerfile 最后一行 `changji serve --host 0.0.0.0 --port 8080`），
    /// 不依赖这个默认值。
    std::string host = "127.0.0.1";
    int port = 8080;
    /// Crow 的工作线程数。**0 = 按这台机器自己定**（见 server.cpp 里的
    /// resolve_concurrency）。
    ///
    /// 以前写死 4，注释里的理由是"业务跑在独立的流水线线程上，这里只处理
    /// 请求收发"。**那句话现在不成立了**：写一章、改一段稿、出一张参考图、
    /// 朗读一段，这几个接口是同步的，活就干在 Crow 的线程上，一干就是
    /// 一两分钟。
    ///
    /// 而 Crow 的一条线程管着一批连接——那条线程被占住，落在它上面的连接
    /// 全都干等。2026-09-11 在服务器上量到：写一章的那五十几秒里，
    /// `/api/health` 始终 0.6 毫秒，而 `/api/run`、`/api/system` 会整整卡
    /// 二十多秒——正好是四条线程里轮到那条被占住的。顶栏、镜头墙、任务
    /// 进度全在这几个接口上，于是"一写字界面就卡住"。
    unsigned concurrency = 0;
};

/// 起服务并阻塞，直到收到停止信号。
void run(const config::Settings& settings, const Options& opts);

}  // namespace changji::http
