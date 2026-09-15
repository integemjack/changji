#include "http/job_stream.hpp"

#include <map>
#include <mutex>
#include <utility>

#include "http/ws.hpp"

namespace changji::http {

void job_done(const std::string& stream_id, nlohmann::json result) {
    if (stream_id.empty()) return;
    ws::hub().broadcast(stream_id, {{"type", "job_done"},
                                    {"job_id", stream_id},
                                    {"result", std::move(result)}});
}

void job_error(const std::string& stream_id, const std::string& message) {
    if (stream_id.empty()) return;
    ws::hub().broadcast(stream_id, {{"type", "job_error"},
                                    {"job_id", stream_id},
                                    {"message", message}});
}

void job_progress(const std::string& stream_id, int current, int total,
                  const std::string& message) {
    if (stream_id.empty()) return;
    ws::hub().broadcast(stream_id, {{"type", "job_progress"},
                                    {"job_id", stream_id},
                                    {"current", current},
                                    {"total", total},
                                    {"message", message}});
}

void job_thinking(const std::string& stream_id, const std::string& piece) {
    if (stream_id.empty() || piece.empty()) return;
    ws::hub().broadcast(stream_id, {{"type", "job_thinking"},
                                    {"job_id", stream_id},
                                    {"text", piece}});
}

namespace {
/// 当前线程在给哪条 stream 干活。见 JobScope。
thread_local std::string g_stream;
/// 当前线程这件活的取消令牌。指向那个 JobScope 里的。
thread_local pipeline::CancelToken* g_cancel = nullptr;

/// 在跑的那几件活：stream_id → 它的令牌。
///
/// **不存 JobScope 本身，只存令牌的地址。** 令牌活在那条后台线程的栈上
/// （JobScope 的成员），生命周期严格包住这件活；表里那一条也是 JobScope
/// 的构造/析构成对加减的，所以不会出现"活干完了还能按停"的悬空指针。
std::mutex g_mu;
std::map<std::string, pipeline::CancelToken*> g_live;

/// 同步那条路上没人能按停，给个不会被触发的。
pipeline::CancelToken& dummy_token() {
    static pipeline::CancelToken t;
    return t;
}
}  // namespace

std::string current_stream() { return g_stream; }

pipeline::CancelToken& current_cancel() {
    return g_cancel != nullptr ? *g_cancel : dummy_token();
}

bool cancel_job(const std::string& stream_id) {
    if (stream_id.empty()) return false;
    std::lock_guard<std::mutex> g(g_mu);
    const auto it = g_live.find(stream_id);
    if (it == g_live.end()) return false;
    it->second->request();
    return true;
}

JobScope::JobScope(std::string stream_id)
    : id_(std::move(stream_id)),
      prev_(g_stream),
      prev_token_(nullptr),
      prev_cancel_(g_cancel),
      use_(&token_) {
    enter();
}

JobScope::JobScope(std::string stream_id, pipeline::CancelToken& token)
    : id_(std::move(stream_id)),
      prev_(g_stream),
      prev_token_(nullptr),
      prev_cancel_(g_cancel),
      use_(&token) {
    enter();
}

void JobScope::enter() {
    g_stream = id_;
    g_cancel = use_;
    if (!id_.empty()) {
        std::lock_guard<std::mutex> g(g_mu);
        // **记下同一个 id 上一层登记的那个，出去时还原。** 同一个 stream id
        // 被重用时（用户连点两下、前一件还没退干净）直接 erase 的话，
        // 外层那件活就再也停不了了——而它还在跑。
        const auto it = g_live.find(id_);
        if (it != g_live.end()) prev_token_ = it->second;
        g_live[id_] = use_;
    }
}

JobScope::~JobScope() {
    if (!id_.empty()) {
        std::lock_guard<std::mutex> g(g_mu);
        // 按地址比一次再动：不是我登记的那条就别碰（同名的下一层还在跑）。
        const auto it = g_live.find(id_);
        if (it != g_live.end() && it->second == use_) {
            if (prev_token_ != nullptr) {
                it->second = prev_token_;
            } else {
                g_live.erase(it);
            }
        }
    }
    // **还原上一层，不是置空。** 嵌套时置空的话，外层那件活的 current_cancel()
    // 会变成哑元——它还在跑，而按停就没反应了。和 g_stream 一个道理。
    g_cancel = prev_cancel_;
    g_stream = std::move(prev_);
}

std::function<void(const std::string&)> thinking_sink(std::string stream_id) {
    if (stream_id.empty()) stream_id = current_stream();
    if (stream_id.empty()) return {};
    return [stream_id](const std::string& piece) {
        job_thinking(stream_id, piece);
    };
}

void job_preview(const std::string& stream_id, int step,
                 std::string data_url) {
    if (stream_id.empty() || data_url.empty()) return;
    ws::hub().broadcast(stream_id, {{"type", "job_preview"},
                                    {"job_id", stream_id},
                                    {"current", step},
                                    {"image", std::move(data_url)}});
}

namespace {

void ref_send(const std::string& target, nlohmann::json msg) {
    if (target.empty()) return;
    msg["job_id"] = kRefChannel;   // Hub 按订阅的那串字分发
    msg["target"] = target;
    ws::hub().broadcast(kRefChannel, msg);
}

}  // namespace

void ref_progress(const std::string& target, int current, int total) {
    ref_send(target, {{"type", "ref_progress"},
                      {"current", current},
                      {"total", total}});
}

void ref_preview(const std::string& target, int step, std::string data_url) {
    if (data_url.empty()) return;
    ref_send(target,
             {{"type", "ref_preview"}, {"current", step}, {"image", std::move(data_url)}});
}

void ref_done(const std::string& target) {
    ref_send(target, {{"type", "ref_done"}});
}

void ref_error(const std::string& target, const std::string& message) {
    ref_send(target, {{"type", "ref_error"}, {"message", message}});
}

}  // namespace changji::http
