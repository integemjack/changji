#include "http/ws.hpp"

#include <iterator>

#include <crow/websocket.h>

namespace changji::ws {

using json = nlohmann::json;

Hub& hub() {
    static Hub instance;
    return instance;
}

void Hub::add(crow::websocket::connection* conn) {
    std::lock_guard<std::mutex> lk(mu_);
    conns_.insert(conn);
}

void Hub::remove(crow::websocket::connection* conn) {
    std::lock_guard<std::mutex> lk(mu_);
    conns_.erase(conn);
    // 订阅表里也要清干净。漏掉的话下一次 broadcast 会向已析构的连接
    // 发送，这是最典型的一类崩溃，而且只在压测或者用户频繁刷新时才复现。
    for (auto it = subs_.begin(); it != subs_.end();) {
        it->second.erase(conn);
        it = it->second.empty() ? subs_.erase(it) : std::next(it);
    }
}

void Hub::subscribe(crow::websocket::connection* conn, const std::string& job_id) {
    if (job_id.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (conns_.count(conn)) subs_[job_id].insert(conn);
}

void Hub::unsubscribe(crow::websocket::connection* conn, const std::string& job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = subs_.find(job_id);
    if (it == subs_.end()) return;
    it->second.erase(conn);
    if (it->second.empty()) subs_.erase(it);
}

void Hub::handle_client_message(crow::websocket::connection* conn,
                                const std::string& raw) {
    // 客户端发来的东西一律当不可信输入：解析失败就静默丢弃，
    // 不回错误也不断开连接。畸形消息不该影响一个正在跑的任务。
    json msg = json::parse(raw, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) return;

    std::string type = msg.value("type", std::string{});
    std::string job_id = msg.value("job_id", std::string{});
    if (type == "subscribe") {
        subscribe(conn, job_id);
    } else if (type == "unsubscribe") {
        unsubscribe(conn, job_id);
    }
}

bool Hub::should_throttle(const std::string& job_id, const std::string& type) {
    // 完成和失败永远不节流。这两条消息漏发一次，前端就会一直显示
    // 「生成中」直到用户手动刷新——比进度条不流畅严重得多。
    if (type != "progress") return false;

    auto now = std::chrono::steady_clock::now();
    auto it = last_sent_.find(job_id);
    if (it != last_sent_.end() && now - it->second < kThrottleInterval) {
        return true;
    }
    last_sent_[job_id] = now;
    return false;
}

// 发送为什么必须在锁内：
//
// 直觉上应该先在锁里拷一份连接列表，出锁再发，避免持锁做 IO。
// 但那样有个 use-after-free 窗口——出锁到发送之间，onclose 可能已经
// 在事件循环线程上跑完并析构了连接，我们手里的裸指针就悬空了。
//
// 而 Crow 的 send_text 并不真的写 socket，它把消息塞进连接的写队列
// 交给 asio 异步发出，是个 O(1) 的入队操作。持锁做这个的代价可以忽略，
// 换来的是和 remove() 严格互斥，指针在发送期间保证有效。
//
// 前提是 send_text 确实不阻塞。哪天换掉 Crow 或者它改了实现，
// 这个假设不成立时必须改成让连接由 shared_ptr 管理。

void Hub::broadcast(const std::string& job_id, const json& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (should_throttle(job_id, msg.value("type", std::string{}))) return;
    auto it = subs_.find(job_id);
    if (it == subs_.end() || it->second.empty()) return;
    std::string payload = msg.dump();
    for (auto* c : it->second) c->send_text(payload);
}

void Hub::broadcast_all(const json& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string payload = msg.dump();
    for (auto* c : conns_) c->send_text(payload);
}

size_t Hub::connection_count() {
    std::lock_guard<std::mutex> lk(mu_);
    return conns_.size();
}

}  // namespace changji::ws
