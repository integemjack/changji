#pragma once

// 一个够用的 WebSocket 客户端。
//
// **为什么自己写。** Crow 只有服务端（前端连过来看进度），
// 而对拍工具要当客户端去连我们自己的 /ws。
// 为一个客户端再拉一个库（websocketpp、Boost.Beast）代价不小：
// Beast 要整个 Boost，websocketpp 停更多年且 API 依赖旧版 asio。
// 而我们要的功能是**收文本帧**——发只发一次握手，连 ping 都可以只回不发。
//
// 协议部分（握手串、帧的编解码）是纯函数，单独暴露出来测。
// 那部分错了不会崩，只会"连不上"或者"收到的消息是乱码"，
// 而在真实服务端上试错一次要几十秒。
//
// 不支持的：wss（TLS）、分片续帧的**跨帧文本拼接**除外的扩展、压缩扩展。
// 我们自己的 /ws 是明文，要 TLS 的部署放个反向代理。
//
// **原来放在 src/comfy/ 底下**，那是历史位置——它一直是通用的，
// 只是最早的用途是连 ComfyUI。2026-09-10 拆 ComfyUI 时挪到 src/net/。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace changji::ws {

/// RFC 6455 那个固定 GUID。拼在客户端 key 后面算 accept。
inline constexpr const char* kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// 拆一个 ws:// 地址。
struct Url {
    std::string host;
    std::string port = "80";
    /// 路径带查询串，直接写进请求行。
    std::string target = "/";
    bool tls = false;
};

/// 认不出返回 nullopt。
std::optional<Url> parse_url(const std::string& url);

/// 握手请求。key 是 16 字节随机数的 base64。
std::string handshake_request(const Url& u, const std::string& key);

/// 服务端该回的 Sec-WebSocket-Accept。
///
/// base64(sha1(key + GUID))，**中间不经过十六进制**。
/// 拿 sha1 的十六进制串去 base64 会得到 56 个字符，服务端算的是 28 个。
std::string accept_key(const std::string& client_key);

/// 16 字节随机数的 base64。每次连接都要不同——固定的话，
/// 中间有缓存的代理可能把上一次的响应当成这一次的。
std::string random_key();

/// 帧的操作码。只列用得上的。
enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

/// 解出来的一帧。
struct Frame {
    Opcode opcode = Opcode::Text;
    bool fin = true;
    std::string payload;
};

/// 从缓冲区头部解一帧。
///
/// 返回 nullopt 表示**数据还不够**，调用方再读一点再来（不是错误）。
/// consumed 是这一帧占了多少字节，只有返回有值时才有意义。
///
/// 长度有三种编码（7 位 / 16 位 / 64 位），只实现前两种是最常见的错误：
/// 一段几十 KB 的预览图就会超过 65535，然后整条连接的解析全乱套。
std::optional<Frame> decode_frame(const std::string& buf, std::size_t& consumed);

/// 编一帧。**客户端发出的帧必须打掩码**，不打的话服务端按协议要断开连接。
std::string encode_frame(Opcode op, const std::string& payload,
                         std::uint32_t mask_key);

}  // namespace changji::ws
