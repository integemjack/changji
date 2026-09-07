// ComfyUI 客户端的真实传输。**只有这个文件 include httplib 和 asio。**
//
// 分出来的理由同 llm/client_http.cpp：让 client.cpp 能进单元测试目标。
// 那边测的是"什么时候算跑完""哪类错误该重试"，这边只负责把字节搬来搬去。

#include <asio.hpp>
#include "util/httplib.hpp"

#include <chrono>
#include <fstream>
#include <memory>
#include <random>
#include <string>

#include "comfy/client.hpp"
#include "comfy/ws_client.hpp"
#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::comfy {

namespace {

httplib::Client make_client(const std::string& base_url, double timeout_s) {
    httplib::Client cli(base_url);
    // 三个超时都要设。只设 read 的话，连不上的机器会卡在 connect 上
    // 直到系统默认超时——Windows 上那是 20 秒往上，用户以为程序死了。
    const int secs = static_cast<int>(timeout_s);
    cli.set_connection_timeout(secs, 0);
    cli.set_read_timeout(secs, 0);
    cli.set_write_timeout(secs, 0);
    cli.set_follow_location(true);
    return cli;
}

HttpResponse from_result(const httplib::Result& res) {
    HttpResponse out;
    if (!res) {
        out.status = 0;
        out.transport_error = httplib::to_string(res.error());
        return out;
    }
    out.status = res->status;
    out.body = res->body;
    return out;
}

/// 一条 WebSocket 连接。活到 WsRecv 被丢弃为止。
///
/// 用 asio 的**阻塞**接口加超时，不是异步回调：调用方要的就是
/// "等最多 N 秒，有消息就给我"，异步在这里只会多一层状态机。
class Connection : public std::enable_shared_from_this<Connection> {
public:
    static std::shared_ptr<Connection> open(const std::string& url,
                                            double connect_timeout_s) {
        const auto parsed = ws::parse_url(url);
        if (!parsed.has_value() || parsed->tls) return nullptr;   // wss 走轮询

        auto self = std::shared_ptr<Connection>(new Connection());
        try {
            asio::ip::tcp::resolver resolver(self->io_);
            const auto endpoints = resolver.resolve(parsed->host, parsed->port);
            asio::connect(self->sock_, endpoints);
            self->sock_.set_option(asio::ip::tcp::no_delay(true));

            const std::string key = ws::random_key();
            const std::string req = ws::handshake_request(*parsed, key);
            asio::write(self->sock_, asio::buffer(req));

            if (!self->read_handshake(key, connect_timeout_s)) return nullptr;
        } catch (const std::exception&) {
            return nullptr;   // 连不上不是致命问题，调用方退回轮询
        }
        return self;
    }

    /// 取下一条文本消息。没消息返回 nullopt（不是错误）。
    std::optional<std::string> recv(double timeout_s) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(
                                  static_cast<long long>(timeout_s * 1000));
        while (true) {
            // 先看缓冲区里有没有攒着的整帧。服务端在一次 write 里塞几条
            // 进度消息是常见的，不先看的话它们要等到下一次网络活动才被处理。
            if (auto msg = take_buffered()) return msg;
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;

            if (!fill(deadline)) {
                // 读超时就当这一轮没消息。**连接没坏**——
                // 当成坏了的话，一个安静的服务端会让整条路退回轮询。
                if (closed_) throw std::runtime_error("WebSocket 连接已关闭");
                return std::nullopt;
            }
        }
    }

private:
    Connection() : sock_(io_) {}

    bool read_handshake(const std::string& key, double timeout_s) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(
                                  static_cast<long long>(timeout_s * 1000));
        while (buf_.find("\r\n\r\n") == std::string::npos) {
            if (!fill(deadline)) return false;
        }
        const std::size_t end = buf_.find("\r\n\r\n");
        const std::string head = buf_.substr(0, end);
        buf_.erase(0, end + 4);

        // 101 之外的任何东西都不是升级成功。有的反向代理会回 200 加一个
        // HTML 页面，那时候继续按帧解析出来的全是垃圾。
        if (head.rfind("HTTP/1.1 101", 0) != 0 &&
            head.rfind("HTTP/1.0 101", 0) != 0) {
            return false;
        }
        // 校验 accept。不校验的话，一个把请求转到别处的代理也能"握手成功"。
        const std::string want = ws::accept_key(key);
        return head.find(want) != std::string::npos;
    }

    std::optional<std::string> take_buffered() {
        while (true) {
            std::size_t used = 0;
            const auto frame = ws::decode_frame(buf_, used);
            if (!frame.has_value()) return std::nullopt;
            buf_.erase(0, used);

            switch (frame->opcode) {
                case ws::Opcode::Text:
                    if (!frame->fin) {
                        // 分片的文本帧。ComfyUI 不发这种，但真遇上了
                        // 攒起来比丢掉强——丢掉的话消息缺一半，
                        // JSON 解析失败，表现是"偶尔漏掉一条进度"。
                        pending_ += frame->payload;
                        continue;
                    }
                    if (!pending_.empty()) {
                        std::string whole = pending_ + frame->payload;
                        pending_.clear();
                        return whole;
                    }
                    return frame->payload;
                case ws::Opcode::Continuation:
                    pending_ += frame->payload;
                    if (frame->fin) {
                        std::string whole = pending_;
                        pending_.clear();
                        return whole;
                    }
                    continue;
                case ws::Opcode::Ping:
                    // 必须回 pong，否则服务端会认为客户端死了然后断开。
                    send(ws::Opcode::Pong, frame->payload);
                    continue;
                case ws::Opcode::Close:
                    closed_ = true;
                    return std::nullopt;
                case ws::Opcode::Binary:
                case ws::Opcode::Pong:
                default:
                    continue;   // 预览图之类的，忽略
            }
        }
    }

    void send(ws::Opcode op, const std::string& payload) {
        static std::mt19937 rng{std::random_device{}()};
        try {
            asio::write(sock_, asio::buffer(ws::encode_frame(op, payload, rng())));
        } catch (const std::exception&) {
            closed_ = true;
        }
    }

    /// 读一点进缓冲区。到点了还没数据返回 false。
    bool fill(std::chrono::steady_clock::time_point deadline) {
        // asio 的同步接口没有超时，所以用非阻塞加轮询。
        // 粒度 20 毫秒：进度消息几百毫秒一条，这个粒度感知不到延迟，
        // 而空转的开销可以忽略。
        char chunk[8192];
        while (true) {
            asio::error_code ec;
            sock_.non_blocking(true, ec);
            const std::size_t n = sock_.read_some(asio::buffer(chunk), ec);
            if (n > 0) {
                buf_.append(chunk, n);
                return true;
            }
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            closed_ = true;
            return false;
        }
    }

    asio::io_context io_;
    asio::ip::tcp::socket sock_;
    std::string buf_;
    std::string pending_;
    bool closed_ = false;
};

}  // namespace

Transport default_transport(ConfigProvider cfg) {
    Transport t;

    t.get = [cfg](const std::string& path, double timeout_s) {
        auto cli = make_client(cfg().base_url, timeout_s);
        return from_result(cli.Get(path));
    };

    t.post_json = [cfg](const std::string& path, const std::string& body,
                        double timeout_s) {
        auto cli = make_client(cfg().base_url, timeout_s);
        return from_result(cli.Post(path, body, "application/json"));
    };

    t.upload = [cfg](const std::string& path, const fs::path& file,
                     const std::string& subfolder, double timeout_s) {
        HttpResponse out;
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            out.status = 0;
            out.transport_error = "读不了 " + paths::to_utf8(file);
            return out;
        }
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

        // 文件名用 UTF-8 原样传。项目里全是中文名，转成 ANSI 的话
        // 服务端存下来的是乱码文件名，而工作流引用的是原名。
        httplib::MultipartFormDataItems items;
        httplib::MultipartFormData img;
        img.name = "image";
        img.content = std::move(content);
        img.filename = paths::to_utf8(file.filename());
        img.content_type = "application/octet-stream";
        items.push_back(std::move(img));

        httplib::MultipartFormData over;
        over.name = "overwrite";
        over.content = "true";
        items.push_back(std::move(over));

        if (!subfolder.empty()) {
            httplib::MultipartFormData sub;
            sub.name = "subfolder";
            sub.content = subfolder;
            items.push_back(std::move(sub));
        }

        auto cli = make_client(cfg().base_url, timeout_s);
        return from_result(cli.Post(path, items));
    };

    t.download = [cfg](const std::string& path,
                       const std::map<std::string, std::string>& query,
                       const fs::path& dest, double timeout_s) {
        HttpResponse out;
        httplib::Params params;
        for (const auto& kv : query) params.emplace(kv.first, kv.second);
        const std::string full =
            path + "?" + httplib::detail::params_to_query_str(params);

        std::ofstream f(dest, std::ios::binary | std::ios::trunc);
        if (!f) {
            out.status = 0;
            out.transport_error = "写不了 " + paths::to_utf8(dest);
            return out;
        }
        auto cli = make_client(cfg().base_url, timeout_s);
        // 流式写盘，不先攒进内存：一段成片档视频几十兆，
        // 攒在内存里正好和出片那一刻的显存/内存高峰撞上。
        const auto res = cli.Get(full,
            [&f](const char* data, std::size_t len) {
                f.write(data, static_cast<std::streamsize>(len));
                return f.good();
            });
        f.close();
        return from_result(res);
    };

    t.connect_ws = [cfg](const std::string& url) -> WsRecv {
        const auto conn = Connection::open(url, cfg().timeout_s);
        if (!conn) return nullptr;
        // 连接握在闭包里，WsRecv 被丢弃时一起析构。
        return [conn](double timeout_s) { return conn->recv(timeout_s); };
    };

    t.sleep = [](double seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<long long>(seconds * 1000)));
    };

    return t;
}

}  // namespace changji::comfy
