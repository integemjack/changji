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
    std::string host = "0.0.0.0";
    int port = 8080;
    /// Crow 的工作线程数。业务跑在独立的流水线线程上，
    /// 这里只处理请求收发，不需要开很多。
    unsigned concurrency = 4;
};

/// 起服务并阻塞，直到收到停止信号。
void run(const config::Settings& settings, const Options& opts);

}  // namespace changji::http
