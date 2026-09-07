// 真实的 HTTP 实现。**只有这个文件 include httplib。**
//
// 分出来是为了让 client.cpp 能进单元测试目标而不用链 httplib——
// 请求怎么拼、错误怎么翻成人话、返回怎么抽内容，那三件事全是纯逻辑。

#include <httplib.h>

#include <string>

#include "http/llm_info.hpp"
#include "llm/client.hpp"

namespace changji::llm {

namespace {

/// 把 base_url 拆成 "scheme://host:port" 和路径前缀两半。
///
/// httplib 的 Client 要单独的 host 和 path，而配置里给的是
/// "http://127.0.0.1:11434/v1" 这样一整条。
std::pair<std::string, std::string> split_base(const std::string& url) {
    const std::size_t scheme_end = url.find("://");
    const std::size_t host_start =
        scheme_end == std::string::npos ? 0 : scheme_end + 3;
    const std::size_t path_start = url.find('/', host_start);
    if (path_start == std::string::npos) return {url, "/"};
    return {url.substr(0, path_start), url.substr(path_start)};
}

}  // namespace

HttpPost default_http_post() {
    return [](const std::string& url, const std::string& body,
              const std::map<std::string, std::string>& headers,
              double timeout_s) -> HttpResponse {
        const auto [origin, path] = split_base(url);

        httplib::Client cli(origin);
        // 三个超时都要设。只设 read 的话，连不上的机器会卡在 connect 上
        // 直到系统默认超时——Windows 上那是 20 秒往上，用户以为程序死了。
        const int secs = static_cast<int>(timeout_s);
        cli.set_connection_timeout(secs, 0);
        cli.set_read_timeout(secs, 0);
        cli.set_write_timeout(secs, 0);
        cli.set_follow_location(true);

        httplib::Headers h;
        for (const auto& kv : headers) h.emplace(kv.first, kv.second);

        const auto res = cli.Post(path, h, body, "application/json");
        if (!res) {
            HttpResponse out;
            out.status = 0;
            out.transport_error = httplib::to_string(res.error());
            return out;
        }
        HttpResponse out;
        out.status = res->status;
        out.body = res->body;
        return out;
    };
}

}  // namespace changji::llm

namespace changji::http {

HttpGet default_http_get() {
    return [](const std::string& url,
              const std::map<std::string, std::string>& headers,
              double timeout_s) -> llm::HttpResponse {
        // 和 POST 那份共用同一套拆地址和超时设置。抽个公共函数不划算——
        // 两边加起来二十行，抽出来反而要多一层间接。
        const std::size_t scheme_end = url.find("://");
        const std::size_t host_start =
            scheme_end == std::string::npos ? 0 : scheme_end + 3;
        const std::size_t path_start = url.find('/', host_start);
        const std::string origin =
            path_start == std::string::npos ? url : url.substr(0, path_start);
        const std::string path =
            path_start == std::string::npos ? "/" : url.substr(path_start);

        httplib::Client cli(origin);
        const int secs = static_cast<int>(timeout_s);
        cli.set_connection_timeout(secs, 0);
        cli.set_read_timeout(secs, 0);
        cli.set_write_timeout(secs, 0);
        cli.set_follow_location(true);

        httplib::Headers h;
        for (const auto& kv : headers) h.emplace(kv.first, kv.second);

        const auto res = cli.Get(path, h);
        llm::HttpResponse out;
        if (!res) {
            out.status = 0;
            out.transport_error = httplib::to_string(res.error());
            return out;
        }
        out.status = res->status;
        out.body = res->body;
        return out;
    };
}

}  // namespace changji::http
