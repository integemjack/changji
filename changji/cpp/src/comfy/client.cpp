#include "comfy/client.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <sstream>

#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::comfy {

namespace {

/// WebSocket 静默多久就去查一次历史兜底。
///
/// 查得太勤给服务端添负担，太懒则任务早跑完了界面还在等。
constexpr double kRecheckEveryS = 5.0;

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

OrderedJson parse_or_null(const std::string& body) {
    OrderedJson j = OrderedJson::parse(body, nullptr, false);
    return j.is_discarded() ? OrderedJson(nullptr) : j;
}

std::string str_or(const OrderedJson& j, const char* key, const char* def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

}  // namespace

std::string random_client_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream os;
    os << std::hex << rng() << std::hex << rng();
    return os.str();
}

std::string PromptValidationError::human_summary() const {
    if (!node_errors_.is_object() || node_errors_.empty()) {
        return what();
    }
    std::vector<std::string> lines;
    for (const auto& kv : node_errors_.items()) {
        const OrderedJson& err = kv.value();
        const std::string cls = str_or(err, "class_type", "未知节点");
        const auto errors = err.find("errors");
        if (errors == err.end() || !errors->is_array()) continue;
        for (const auto& detail : *errors) {
            const std::string message = str_or(detail, "message", "");
            const auto extra = detail.find("extra_info");
            const OrderedJson info =
                (extra != detail.end() && extra->is_object()) ? *extra
                                                              : OrderedJson::object();
            if (message.find("not in list") != std::string::npos) {
                // 这一类占实际报错的绝大多数：工作流是在别人的机器上导出的，
                // 那台有这个模型文件。原样透传的话用户要从一坨 JSON 里
                // 自己找出文件名。
                lines.push_back("节点 " + kv.key() + "（" + cls + "）要的 " +
                                str_or(info, "input_name", "") + " 是 " +
                                str_or(info, "received_value", "") +
                                "，服务端上没有这个文件");
            } else {
                lines.push_back("节点 " + kv.key() + "（" + cls + "）：" +
                                (message.empty() ? detail.dump() : message));
            }
        }
    }
    if (lines.empty()) return what();
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\n";
        out += lines[i];
    }
    return out;
}

std::vector<OrderedJson> JobResult::files(const std::string& kind) const {
    std::vector<OrderedJson> out;
    if (!outputs.is_object()) return out;
    for (const auto& kv : outputs.items()) {
        const auto it = kv.value().find(kind);
        if (it == kv.value().end() || !it->is_array()) continue;
        for (const auto& f : *it) out.push_back(f);
    }
    return out;
}

std::optional<OrderedJson> JobResult::first_file() const {
    const auto all = files();
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::string error_from_history(const OrderedJson& status) {
    const auto messages = status.find("messages");
    if (messages != status.end() && messages->is_array()) {
        for (const auto& msg : *messages) {
            // 形状是 ["execution_error", {...}]，数组套数组。
            if (!msg.is_array() || msg.size() < 2) continue;
            if (!msg[0].is_string() || msg[0] != "execution_error") continue;
            const OrderedJson& detail = msg[1];
            return "节点 " + str_or(detail, "node_type", "?") + " 执行失败：" +
                   str_or(detail, "exception_message", "");
        }
    }
    return "任务执行失败";
}

// ---- Client ----

Client::Client(ConfigProvider cfg, Transport transport, std::string client_id)
    : cfg_(std::move(cfg)),
      t_(std::move(transport)),
      client_id_(client_id.empty() ? random_client_id() : std::move(client_id)) {}

OrderedJson Client::get_json(const std::string& path) {
    const config::ComfyConfig c = cfg_();
    const HttpResponse r = t_.get(path, c.timeout_s);
    if (r.transport_error.has_value()) {
        throw ComfyUnavailable("连不上 ComfyUI（" + c.base_url +
                               "）。请确认服务已启动且地址正确。\n" +
                               *r.transport_error);
    }
    if (r.status < 200 || r.status >= 300) {
        throw ComfyUnavailable("ComfyUI 回了 " + std::to_string(r.status) +
                               "（" + c.base_url + path + "）");
    }
    OrderedJson j = parse_or_null(r.body);
    if (j.is_null()) {
        throw ComfyUnavailable("ComfyUI 回的不是 JSON（" + path + "）");
    }
    return j;
}

bool Client::ping() {
    try {
        get_json("/system_stats");
        return true;
    } catch (const ComfyError&) {
        return false;
    }
}

OrderedJson Client::system_stats() { return get_json("/system_stats"); }

const OrderedJson& Client::object_info(bool refresh) {
    if (!object_info_loaded_ || refresh) {
        object_info_ = get_json("/object_info");
        object_info_loaded_ = true;
    }
    return object_info_;
}

WorkflowConverter Client::converter() { return WorkflowConverter(object_info()); }

std::vector<std::string> Client::available_models(const std::string& node_class,
                                                  const std::string& input_name) {
    const OrderedJson& info = object_info();
    const auto node = info.find(node_class);
    if (node == info.end()) return {};
    const auto input = node->find("input");
    if (input == node->end() || !input->is_object()) return {};
    for (const char* section : {"required", "optional"}) {
        const auto sec = input->find(section);
        if (sec == input->end() || !sec->is_object()) continue;
        const auto spec = sec->find(input_name);
        if (spec == sec->end() || !spec->is_array() || spec->empty()) continue;
        if (!(*spec)[0].is_array()) continue;
        std::vector<std::string> out;
        for (const auto& v : (*spec)[0]) {
            if (v.is_string()) out.push_back(v.get<std::string>());
        }
        return out;
    }
    return {};
}

std::string Client::submit(const ApiWorkflow& w, const std::string& client_id) {
    const config::ComfyConfig c = cfg_();
    const OrderedJson payload{
        {"prompt", w.to_json()},
        {"client_id", client_id.empty() ? client_id_ : client_id}};

    const HttpResponse r = t_.post_json("/prompt", payload.dump(), c.timeout_s);
    if (r.transport_error.has_value()) {
        throw ComfyUnavailable("提交任务失败：" + *r.transport_error);
    }

    if (r.status == 400) {
        // **400 是校验失败，重试没有意义。** 归到可重试那一类的话，
        // 一个打错的文件名会让界面转四分钟才报错。
        const OrderedJson body = parse_or_null(r.body);
        if (body.is_null()) {
            throw PromptValidationError("提交被拒绝：" + r.body.substr(0, 400),
                                        OrderedJson::object());
        }
        const auto err = body.find("error");
        const std::string message =
            (err != body.end() && err->is_object()) ? str_or(*err, "message", "")
                                                    : "";
        const auto ne = body.find("node_errors");
        throw PromptValidationError(
            message.empty() ? "工作流校验失败" : message,
            (ne != body.end() && ne->is_object()) ? *ne : OrderedJson::object());
    }
    if (r.status < 200 || r.status >= 300) {
        throw ComfyUnavailable("提交任务失败，ComfyUI 回了 " +
                               std::to_string(r.status));
    }

    const OrderedJson body = parse_or_null(r.body);
    const auto id = body.is_object() ? body.find("prompt_id") : body.end();
    if (!body.is_object() || id == body.end() || !id->is_string()) {
        throw ComfyUnavailable("提交成功但没拿到 prompt_id，回的是：" +
                               r.body.substr(0, 200));
    }
    return id->get<std::string>();
}

std::optional<OrderedJson> Client::history_outputs(const std::string& prompt_id,
                                                   bool missing_ok) {
    const OrderedJson hist = get_json("/history/" + prompt_id);
    const auto entry = hist.is_object() ? hist.find(prompt_id) : hist.end();
    if (!hist.is_object() || entry == hist.end()) {
        return missing_ok ? std::optional<OrderedJson>{}
                          : std::optional<OrderedJson>{OrderedJson::object()};
    }
    const auto status = entry->find("status");
    const OrderedJson st =
        (status != entry->end() && status->is_object()) ? *status
                                                        : OrderedJson::object();
    if (str_or(st, "status_str", "") == "error") {
        throw ExecutionError(error_from_history(st));
    }
    if (missing_ok) {
        // completed 缺省当成 true：老版本的 ComfyUI 不带这个字段，
        // 当成 false 的话兜底查询永远返回"还没跑完"，白等到超时。
        const auto done = st.find("completed");
        const bool completed =
            (done == st.end() || !done->is_boolean()) ? true : done->get<bool>();
        if (!completed) return std::nullopt;
    }
    const auto outputs = entry->find("outputs");
    return (outputs != entry->end()) ? *outputs : OrderedJson::object();
}

JobResult Client::consume(const WsRecv& recv, const std::string& prompt_id,
                          const OnProgress& on_progress, double deadline,
                          pipeline::CancelToken& tok) {
    const double start = now_seconds();
    while (true) {
        if (tok.cancelled()) throw ExecutionError("已取消");
        const double remaining = deadline - now_seconds();
        if (remaining <= 0) {
            throw ExecutionError("任务 " + prompt_id + " 超时");
        }

        const std::optional<std::string> raw =
            recv(std::min(remaining, kRecheckEveryS));
        if (!raw.has_value()) {
            // 一段时间没消息**不代表任务还在跑**。完成消息可能根本没送到：
            // 任务在 WebSocket 连上之前就结束了，或者同一个 clientId 上有
            // 并发任务、后连的把先连的挤掉了。查一次历史兜底，
            // 否则这里会白等到 job_timeout_s，默认是半小时。
            if (const auto outputs = history_outputs(prompt_id, true)) {
                return {prompt_id, *outputs, now_seconds() - start};
            }
            continue;
        }

        const OrderedJson msg = parse_or_null(*raw);
        if (!msg.is_object()) continue;

        const std::string type = str_or(msg, "type", "");
        const auto data_it = msg.find("data");
        const OrderedJson data =
            (data_it != msg.end() && data_it->is_object()) ? *data_it
                                                           : OrderedJson::object();

        // 别人的任务的消息要滤掉。没有 prompt_id 的（比如状态心跳）留着。
        const auto pid = data.find("prompt_id");
        if (pid != data.end() && !pid->is_null()) {
            if (!pid->is_string() || pid->get<std::string>() != prompt_id) continue;
        }

        if (type == "progress") {
            if (on_progress) {
                JobProgress p;
                p.prompt_id = prompt_id;
                p.node_id = str_or(data, "node", "");
                const auto v = data.find("value");
                const auto m = data.find("max");
                p.step = (v != data.end() && v->is_number()) ? v->get<int>() : 0;
                p.total = (m != data.end() && m->is_number()) ? m->get<int>() : 0;
                on_progress(p);
            }
        } else if (type == "execution_error") {
            throw ExecutionError("节点 " + str_or(data, "node_type", "?") +
                                 " 执行失败：" +
                                 str_or(data, "exception_message", ""));
        } else if (type == "execution_interrupted") {
            throw ExecutionError("任务 " + prompt_id + " 被中断");
        } else if (type == "executing") {
            // **node 为 null 表示整个任务跑完。** 这是最容易看漏的一条：
            // 它和"正在执行某个节点"是同一个消息类型，只差 node 是不是 null。
            const auto node = data.find("node");
            if (node != data.end() && node->is_null()) {
                const auto outputs = history_outputs(prompt_id, false);
                return {prompt_id, outputs.value_or(OrderedJson::object()),
                        now_seconds() - start};
            }
        }
    }
}

JobResult Client::wait_poll(const std::string& prompt_id, double deadline,
                            pipeline::CancelToken& tok) {
    const double start = now_seconds();
    while (now_seconds() < deadline) {
        if (tok.cancelled()) throw ExecutionError("已取消");
        const OrderedJson hist = get_json("/history/" + prompt_id);
        const auto entry = hist.is_object() ? hist.find(prompt_id) : hist.end();
        if (hist.is_object() && entry != hist.end()) {
            const auto status = entry->find("status");
            const OrderedJson st =
                (status != entry->end() && status->is_object()) ? *status
                                                                : OrderedJson::object();
            if (str_or(st, "status_str", "") == "error") {
                throw ExecutionError(error_from_history(st));
            }
            const auto outputs = entry->find("outputs");
            return {prompt_id,
                    (outputs != entry->end()) ? *outputs : OrderedJson::object(),
                    now_seconds() - start};
        }
        if (t_.sleep) t_.sleep(2.0);
    }
    throw ExecutionError("任务 " + prompt_id + " 超时");
}

JobResult Client::wait(const std::string& prompt_id, const OnProgress& on_progress,
                       double timeout_s, const std::string& client_id,
                       pipeline::CancelToken& tok) {
    const config::ComfyConfig c = cfg_();
    const double deadline =
        now_seconds() + (timeout_s > 0 ? timeout_s : c.job_timeout_s);
    const std::string id = client_id.empty() ? client_id_ : client_id;

    WsRecv recv;
    try {
        if (t_.connect_ws) recv = t_.connect_ws(c.ws_url() + "?clientId=" + id);
    } catch (const std::exception&) {
        recv = nullptr;   // 连不上不是致命问题
    }
    if (!recv) {
        // WebSocket 不可用不是致命问题，退回轮询。反向代理不转发 WS 升级
        // 是很常见的部署问题，为它整条路都跑不了不值得。
        return wait_poll(prompt_id, deadline, tok);
    }
    try {
        return consume(recv, prompt_id, on_progress, deadline, tok);
    } catch (const ComfyError&) {
        throw;   // 执行失败、超时、取消都是真的失败，不该退回轮询再等一遍
    } catch (const std::exception&) {
        // 连接中途断了，退回轮询把这一次跑完
        return wait_poll(prompt_id, deadline, tok);
    }
}

JobResult Client::run(const ApiWorkflow& w, const OnProgress& on_progress,
                      pipeline::CancelToken& tok, double timeout_s) {
    const config::ComfyConfig c = cfg_();
    std::string last;
    for (int attempt = 0; attempt <= c.max_retries; ++attempt) {
        if (tok.cancelled()) throw ExecutionError("已取消");
        try {
            // **每个任务用独立的 clientId。** ComfyUI 按 clientId 记订阅，
            // 同一个 id 上并发跑两个任务，后连的会把先连的挤下线，
            // 先连的那个永远等不到完成消息，白等到超时为止。
            const std::string job_id =
                client_id_ + "-" + random_client_id().substr(0, 8);
            const std::string prompt_id = submit(w, job_id);
            return wait(prompt_id, on_progress, timeout_s, job_id, tok);
        } catch (const PromptValidationError&) {
            throw;   // 重试也不会变好
        } catch (const ComfyError& e) {
            last = e.what();
            if (tok.cancelled()) throw;
            if (attempt < c.max_retries && t_.sleep) {
                // 指数退避。显存不足是最常见的可重试错误，而它往往是因为
                // 另一个任务正占着——等一会儿比立刻再撞一次强。
                t_.sleep(std::pow(2.0, attempt));
            }
        }
    }
    throw ExecutionError("重试 " + std::to_string(c.max_retries) +
                         " 次后仍失败：" + last);
}

std::string Client::upload_image(const fs::path& p, const std::string& subfolder) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        throw ComfyError("要上传的文件不存在：" + paths::to_utf8(p));
    }
    const config::ComfyConfig c = cfg_();
    const HttpResponse r = t_.upload("/upload/image", p, subfolder, c.timeout_s);
    if (r.transport_error.has_value()) {
        throw ComfyUnavailable("上传失败：" + *r.transport_error);
    }
    if (r.status < 200 || r.status >= 300) {
        throw ComfyError("上传失败，ComfyUI 回了 " + std::to_string(r.status));
    }
    const OrderedJson body = parse_or_null(r.body);
    if (!body.is_object() || !body.contains("name")) {
        throw ComfyError("上传成功但没拿到文件名，回的是：" + r.body.substr(0, 200));
    }
    const std::string name = str_or(body, "name", "");
    const std::string sub = str_or(body, "subfolder", "");
    // 服务端可能把文件放进了别的子目录（重名时）。**要用它回的那个**，
    // 用本地文件名的话工作流引用的是一个不存在的路径。
    return sub.empty() ? name : sub + "/" + name;
}

fs::path Client::download(const OrderedJson& file_ref, const fs::path& dest) {
    if (!file_ref.is_object() || !file_ref.contains("filename")) {
        throw ComfyError("产出文件的描述里没有 filename");
    }
    const std::map<std::string, std::string> query{
        {"filename", str_or(file_ref, "filename", "")},
        {"subfolder", str_or(file_ref, "subfolder", "")},
        {"type", str_or(file_ref, "type", "output")},
    };
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    const config::ComfyConfig c = cfg_();
    // 下载用 job_timeout_s 而不是 timeout_s：一段成片档视频几十兆，
    // 60 秒在慢网络上不够。
    const HttpResponse r = t_.download("/view", query, dest, c.job_timeout_s);
    if (r.transport_error.has_value()) {
        throw ComfyUnavailable("下载产出失败：" + *r.transport_error);
    }
    if (r.status < 200 || r.status >= 300) {
        throw ComfyError("下载产出失败，ComfyUI 回了 " + std::to_string(r.status));
    }
    return dest;
}

void Client::interrupt() {
    const config::ComfyConfig c = cfg_();
    // 打断失败不抛：调用方一定是在处理取消，那时候再抛一个异常只会
    // 盖掉真正的原因。
    try {
        t_.post_json("/interrupt", "", c.timeout_s);
    } catch (const std::exception&) {
    }
}

}  // namespace changji::comfy
