#include "llm/client.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "llm/sse.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::llm {

namespace {

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
    }
    return s;
}

/// 从错误响应里挖出服务说了什么。
///
/// 对应 Python 的
///   detail = str(body.get("error") or body.get("message") or "")[:200]
///   except: detail = response.text[:200]
std::string error_detail(const std::string& body) {
    const json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    std::string detail;
    if (!parsed.is_discarded() && parsed.is_object()) {
        for (const char* key : {"error", "message"}) {
            const auto it = parsed.find(key);
            if (it == parsed.end() || it->is_null()) continue;
            // 字符串直接用；别的类型 dump 成 JSON。
            //
            // 和 Python 有出入：那边是 str(dict)，吐的是 Python 的字典
            // repr（单引号、True/False）。在 C++ 里复刻那个格式既荒唐又没用——
            // 这段文字只是原样显示给用户看的，不解析。
            detail = it->is_string() ? it->get<std::string>() : it->dump();
            if (!detail.empty()) break;
        }
    } else {
        detail = body;
    }
    return text::truncate_utf8(detail, 200);
}

}  // namespace

ordered build_payload(const config::LLMConfig& cfg, const Request& req,
                      bool json_schema_mode) {
    ordered messages = ordered::array();
    messages.push_back(ordered{{"role", "user"}, {"content", req.prompt}});

    ordered payload = ordered::object();
    payload["model"] = cfg.model;
    payload["messages"] = messages;
    payload["temperature"] = cfg.temperature;

    if (req.schema.is_null() || req.schema.empty()) {
        // 没给 schema 就不加 response_format。加一个空的会被某些服务拒掉。
        return payload;
    }
    if (json_schema_mode) {
        payload["response_format"] = ordered{
            {"type", "json_schema"},
            {"json_schema", ordered{{"name", req.schema_name},
                                    {"strict", true},
                                    {"schema", req.schema}}}};
    } else {
        // 有些服务不支持 json_schema，退回普通 JSON 模式。
        // 退回之后模型的输出结构没保证，全靠提示词里那句"只输出 JSON"，
        // 以及解析阶段的括号扫描兜底。
        payload["response_format"] = ordered{{"type", "json_object"}};
    }
    return payload;
}

std::string extract_content(const std::string& raw_body) {
    const json body = json::parse(raw_body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) {
        throw LlmError("大模型返回的不是 JSON：" +
                       text::truncate_utf8(raw_body, 200));
    }
    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty()) {
        throw LlmError("大模型返回格式异常：" + text::truncate_utf8(body.dump(), 400));
    }
    const auto& first = (*choices)[0];
    if (!first.is_object()) {
        throw LlmError("大模型返回格式异常：" + text::truncate_utf8(body.dump(), 400));
    }
    const auto msg = first.find("message");
    if (msg == first.end() || !msg->is_object()) {
        throw LlmError("大模型返回格式异常：" + text::truncate_utf8(body.dump(), 400));
    }
    const auto content = msg->find("content");
    if (content == msg->end() || !content->is_string()) {
        throw LlmError("大模型返回格式异常：" + text::truncate_utf8(body.dump(), 400));
    }
    return content->get<std::string>();
}

std::string explain_status(const config::LLMConfig& cfg, int status,
                           const std::string& body) {
    const std::string url = cfg.base_url + "/chat/completions";
    const std::string detail = error_detail(body);

    std::string hint;
    if (status == 404) {
        // 404 有两种：地址不对，和模型没拉下来。Ollama 两种都回 404，
        // 一律说「地址填错了」会把人支到错误的地方去查。
        const std::string low = lower_ascii(detail);
        const bool about_model =
            low.find("model") != std::string::npos &&
            (low.find("not found") != std::string::npos ||
             low.find("not exist") != std::string::npos);
        if (about_model) {
            hint = "大模型服务在，但它没有 " + cfg.model + " 这个模型。"
                   "本地 Ollama 的话先 ollama pull " + cfg.model +
                   "，或者去设置页的「大模型」那一节换一个已经有的模型名。";
        } else {
            hint = "大模型服务在 " + url + " 上没有这个接口。"
                   "多半是地址填错了——地址要带 /v1 结尾，"
                   "而且那台机器上的服务得真的起着。去设置页的「大模型」那一节改。";
        }
    } else if (status == 401 || status == 403) {
        hint = "大模型服务拒绝了这次请求（" + std::to_string(status) +
               "），八成是 API Key 不对。去设置页改。";
    } else if (status == 429) {
        hint = "大模型服务说请求太频繁了，等一会儿再试。";
    } else if (status >= 500) {
        hint = "大模型服务自己出错了（" + std::to_string(status) + "）。"
               "本地服务的话看一眼它的日志，云服务的话过一会儿再试。";
    } else {
        hint = "大模型服务返回 " + std::to_string(status) + "。";
    }

    std::string out = hint;
    if (!cfg.model.empty()) out += "\n当前模型：" + cfg.model;
    if (!detail.empty()) out += "\n服务说：" + detail;
    return out;
}

// ---- RemoteClient ----

RemoteClient::RemoteClient(ConfigProvider cfg, HttpPost post,
                           HttpPostStream stream_post)
    : cfg_(std::move(cfg)),
      post_(std::move(post)),
      stream_post_(std::move(stream_post)) {}

RemoteClient::RemoteClient(config::LLMConfig cfg, HttpPost post,
                           HttpPostStream stream_post)
    : cfg_([cfg] { return cfg; }),
      post_(std::move(post)),
      stream_post_(std::move(stream_post)) {}

std::string RemoteClient::complete(const Request& req,
                                   pipeline::CancelToken& tok,
                                   const OnToken& on_token) {
    // 没人要逐字、或者没注入流式发送函数，就走整段那条。
    // 基类那个默认实现会把整段回调一次，形状是一样的。
    if (!on_token || !stream_post_) return Client::complete(req, tok, on_token);
    if (tok.cancelled()) throw LlmError("已取消");

    const config::LLMConfig cfg = cfg_();
    const std::string url = cfg.base_url + "/chat/completions";
    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + cfg.api_key},
        {"Content-Type", "application/json"},
        // 有的网关看这个头决定要不要给你加缓冲。加了缓冲就等于没有流式。
        {"Accept", "text/event-stream"},
    };

    // 跑一趟 SSE。回来的是「拿到了多少正文 / 出了什么事」。
    struct Attempt {
        std::string text;
        std::string sse_error;   ///< 服务端在流里塞的 error
        int status = 0;
        std::optional<std::string> transport_error;
        bool canceled = false;
    };
    const auto run = [&](bool json_schema_mode) {
        Attempt a;
        nlohmann::ordered_json payload = build_payload(cfg, req, json_schema_mode);
        payload["stream"] = true;

        SseDeltas sse;
        HttpResponse r = stream_post_(
            url, payload.dump(), headers, cfg.timeout_s,
            [&](const char* data, std::size_t len) {
                if (tok.cancelled()) {
                    a.canceled = true;
                    return false;   // 断掉，别让它继续生成
                }
                const std::string piece = sse.feed(data, len);
                if (piece.empty()) return true;
                a.text += piece;
                on_token(piece);
                return true;
            });
        a.status = r.status;
        a.transport_error = r.transport_error;
        a.sse_error = sse.error();
        // **服务端没理会 stream 的情况**：它回了一份普通的 JSON，SSE 解不
        // 出任何东西。那份 body 在 r.body 里（流式那条只在出错时收 body，
        // 但"整份 JSON"和"错误体"在传输上没区别），试着按整段解一次。
        if (a.text.empty() && a.status < 400 && !r.body.empty()) {
            try {
                a.text = extract_content(r.body);
                if (!a.text.empty()) on_token(a.text);
            } catch (const std::exception&) {
                // 解不出来就当这次没成，下面的退路会接手
            }
        }
        return a;
    };

    // **三条退路，一条都不能少。**
    //
    //   1. 带 schema 的 SSE —— 正常那条
    //   2. 不带 schema 的 SSE —— 有些服务不认 json_schema，理由和整段
    //      那条里那段注释一样：各家回的状态码五花八门，没法只按码判断
    //   3. 整段 —— 压根不支持 stream 的服务（或者流里什么都没给）
    //
    // 第 3 条是"接了 SSE 不会让任何一种服务变得更糟"的全部保证。
    Attempt a = run(true);
    if (a.canceled) throw LlmError("已取消");
    if (!a.text.empty() && a.sse_error.empty()) return a.text;

    if (a.status >= 400 || a.text.empty()) {
        if (tok.cancelled()) throw LlmError("已取消");
        Attempt b = run(false);
        if (b.canceled) throw LlmError("已取消");
        if (!b.text.empty() && b.sse_error.empty()) return b.text;
        a = std::move(b);
    }

    // 流里明说了出错：这条要报出来，不能当成"生成完了"——
    // 否则用户拿到的是一段空正文外加一句"写好了"。
    if (!a.sse_error.empty() && a.text.empty()) {
        throw LlmError("大模型服务报错：" + a.sse_error);
    }
    if (!a.text.empty()) return a.text;

    // 退回整段那条。连不上、认证错这类问题也在这儿统一报——那边的
    // explain_status 已经把各种状态码翻成了能照着做的话。
    return Client::complete(req, tok, on_token);
}

std::string Client::complete(const Request& req, pipeline::CancelToken& tok,
                             const OnToken& on_token) {
    // 拿不到逐字的后端就整段给一次。**不是不回调**——调用方按"回调拼出来
    // 的就是全文"写，少这一下的话流式那条路上什么都收不到，而它不会报错，
    // 只是编辑器里一直空着。
    std::string out = complete(req, tok);
    if (on_token && !out.empty()) on_token(out);
    return out;
}

std::string RemoteClient::complete(const Request& req,
                                   pipeline::CancelToken& tok) {
    if (tok.cancelled()) throw LlmError("已取消");

    // 每次取一份当前配置。中途 /api/connections 换了地址的话，
    // 下一次调用就走新地址——这正是那个接口的意义。
    const config::LLMConfig cfg = cfg_();
    const std::string url = cfg.base_url + "/chat/completions";
    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + cfg.api_key},
        {"Content-Type", "application/json"},
    };

    HttpResponse r = post_(url, build_payload(cfg, req, true).dump(), headers,
                           cfg.timeout_s);
    if (r.transport_error.has_value()) {
        throw LlmError("连不上大模型服务（" + cfg.base_url + "）。\n" +
                       *r.transport_error);
    }

    if (r.status >= 400) {
        // 有些服务不支持 json_schema，退回普通 JSON 模式再试一次。
        //
        // 无条件重试而不是只在特定状态码上重试，是抄 Python 的：
        // 各家服务对"不支持这个 response_format"回的码五花八门，
        // 400、404、422 都见过，判断哪个是"不支持"哪个是"真错了"不现实。
        // 代价是真的地址错了会多发一次请求。
        if (tok.cancelled()) throw LlmError("已取消");
        r = post_(url, build_payload(cfg, req, false).dump(), headers,
                  cfg.timeout_s);
        if (r.transport_error.has_value()) {
            throw LlmError("连不上大模型服务（" + cfg.base_url + "）。\n" +
                           *r.transport_error);
        }
    }

    if (r.status >= 400) throw LlmError(explain_status(cfg, r.status, r.body));
    if (tok.cancelled()) throw LlmError("已取消");
    return extract_content(r.body);
}

// ---- ReplayClient ----

ReplayClient::ReplayClient(std::vector<std::string> responses)
    : responses_(std::move(responses)) {}

std::string ReplayClient::complete(const Request& req,
                                   pipeline::CancelToken& tok) {
    if (tok.cancelled()) throw LlmError("已取消");
    calls_.push_back(req);
    if (next_ >= responses_.size()) {
        throw LlmError("回放录到头了：这是第 " + std::to_string(next_ + 1) +
                       " 次调用，只录了 " + std::to_string(responses_.size()) +
                       " 条");
    }
    return responses_[next_++];
}

}  // namespace changji::llm
