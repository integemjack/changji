#include "llm/client.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>

#include "config/runtime.hpp"
#include "llm/sse.hpp"
#include "stages/prompts.inc.hpp"
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

/// 服务说的是"这个模型不让你用"吗？是的话返回它那句原话，否则空串。
///
/// **只认几个说法明确的短语**，宁可漏判。漏判的代价是退回原来那句
/// 「八成是 API Key 不对」——不理想但不致命，而且服务的原话本来就跟在
/// 后面（explain_status 末尾会附「服务说：…」）。误判的代价大得多：
/// 真的密钥过期时告诉人家"换个模型试试"，那才是把人支到沟里。
std::string model_not_allowed(const std::string& detail) {
    static const char* kMarks[] = {
        "only available on",      // OpenRouter: only available on agentic harnesses
        "not available to you",
        "no access to",
        "does not have access",
        "requires a paid",
        "not allowed to use",
    };
    const std::string low = lower_ascii(detail);
    for (const char* m : kMarks) {
        if (low.find(m) != std::string::npos) return detail;
    }
    return {};
}

}  // namespace

std::string schema_as_prompt(const std::string& prompt, const ordered& schema) {
    if (schema.is_null() || schema.empty()) return prompt;
    // 缩进两格而不是压成一行：这份东西是给模型读的，分镜那份有几十个字段，
    // 压成一行之后连人都看不出哪个字段套在哪个里面。
    return prompt + stages::prompt::llm::kSchemaSuffix + schema.dump(2);
}

ordered build_payload(const config::LLMConfig& cfg, const Request& req) {
    ordered messages = ordered::array();
    // **schema 以文字贴在提示词后面，不走 response_format。**
    //
    // 2026-09-14 决定：结构约束整个交给提示词。理由是那层"硬约束"早就不硬了——
    // 本地那条的 GBNF 随本地后端一起删了；远端严格模式各家支持得七零八落
    // （OpenRouter 上一大半模型压根不收 response_format，智谱那个免费档收下
    // 之后既不报错也不照做，直接回一段散文），为了兜住这些差异原来挂着一部
    // 四档退档梯子，而梯子本身又会被别的 400 误触发——2026-09-14 glm-5.3
    // 那次就是：它只是不收 thinking 的关闭值，结果十次生成全被退到最底一档。
    //
    // 一档到底反而实在：每次发的都一样，没有"这次到底走的哪一档"这个问题。
    messages.push_back(ordered{
        {"role", "user"}, {"content", schema_as_prompt(req.prompt, req.schema)}});

    ordered payload = ordered::object();
    payload["model"] = cfg.model;
    payload["messages"] = messages;
    // **这里原来发的是 cfg.temperature，req 里那个从来没人读。** 于是
    // kChapterTemperature（写正文 0.5）在远端那条路上一直空转——而远端
    // 正是现在的默认后端。两条路都返回 200，所以这件事只能靠读代码发现。
    // 见 Request::temperature 上那段。
    payload["temperature"] = req.temperature.value_or(cfg.temperature);
    return payload;
}

/// 整段那条回来的思考。**没有就是空串。**
///
/// 整段那条拿不到过程，只能在收完之后一次性把整段思考给出去——界面上
/// 那个浮层于是"想完了才有内容"。比没有强：用户至少能回头看它想了什么。
std::string extract_thinking(const std::string& raw_body) {
    const json body = json::parse(raw_body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) return {};
    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty()) return {};
    const auto& first = (*choices)[0];
    if (!first.is_object()) return {};
    const auto msg = first.find("message");
    if (msg == first.end() || !msg->is_object()) return {};
    for (const char* k : {"reasoning_content", "reasoning"}) {
        const auto r = msg->find(k);
        if (r != msg->end() && r->is_string()) return r->get<std::string>();
    }
    return {};
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
        // **三种情况要说三句不同的话。** 都说成「API Key 不对」的话，
        // 前两种会把人支去检查一个他根本没填过的东西。
        if (cfg.api_key.empty()) {
            hint = "还没填 API Key。去设置页的「大模型」那一节填上——"
                   "当前地址是 " + cfg.base_url + "。";
            if (cfg.base_url.find("bigmodel.cn") != std::string::npos ||
                cfg.base_url.find("z.ai") != std::string::npos) {
                hint += "去 bigmodel.cn 控制台领一把——默认挑的"
                        "glm-4.7-flash 本身不要钱，但服务仍然要认人。";
            }
        } else if (!model_not_allowed(detail).empty()) {
            // **403 不都是密钥问题。** 2026-09-13 实测：OpenRouter 上
            // thinkingmachines/inkling:free 回的是
            //   "only available on agentic harnesses. Try plugging it into
            //    a coding agent or productivity app listed on …/apps"
            // ——密钥完全正常（同一把密钥列得出 445 个模型），是**这个模型
            // 的免费档限定入口**，只给它登记过的那些应用用。
            //
            // 照老话术报的话，用户会去反复换一把其实没问题的密钥，而真正
            // 该做的是换一个模型。**服务已经把原因说清楚了，照抄就是**——
            // 我们猜的那句反而盖住了它。
            hint = "这个模型不让我们用（" + std::to_string(status) + "）：" +
                   model_not_allowed(detail) +
                   "\n密钥本身多半没问题——去设置页的「大模型」那一节换一个"
                   "模型试试。";
        } else {
            hint = "大模型服务拒绝了这次请求（" + std::to_string(status) +
                   "），八成是 API Key 不对。去设置页改。";
        }
    } else if (status == 429) {
        // **429 不一定是"太频繁"。** 智谱把"余额不足/没有可用资源包"也回
        // 429（code 1113），2026-09-13 实测：拿免费密钥调 glm-4.7 这类收费
        // 模型就是这个。报成"等一会儿再试"的话，用户会一直等一件永远不会
        // 自己好起来的事。
        if (detail.find("1113") != std::string::npos ||
            detail.find("余额") != std::string::npos) {
            hint = "这个模型要钱，而账上没余额（服务回的是 429 / 1113）。"
                   "换一个免费模型（智谱这边是 glm-4.7-flash），"
                   "或者去服务商那边充值。去设置页的「大模型」那一节改。";
            // **手上有 GLM Coding Plan 订阅的人会撞在这儿，而且想不明白。**
            // 那份额度只认智谱登记在册的编程工具（Claude Code、Cline、
            // Cursor 那些），官方原话是"在除规定工具外调用 API，不可享用
            // Coding 套餐的额度"——我们这个程序不在册，于是同一把密钥
            // 调过来要么报余额不足，要么**直接去扣按量余额**。
            // 不说这一句的话，用户会盯着一个明明还有额度的订阅反复怀疑
            // 是自己填错了。
            if (cfg.base_url.find("bigmodel.cn") != std::string::npos ||
                cfg.base_url.find("z.ai") != std::string::npos) {
                hint +=
                    "\n⚠️ 有 GLM Coding Plan 订阅也一样：那份额度只认智谱"
                    "登记在册的编程工具，自己写的程序调不到，正是这个报错。"
                    "订阅之外另充一点按量余额，或者就用免费那个。";
            }
        } else {
            hint = "大模型服务说请求太频繁了，等一会儿再试。"
                   "免费档限流很紧，隔十几秒再点一次多半就过了。";
        }
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

std::vector<std::pair<std::string, std::string>> known_models(
    const std::string& base_url) {
    const bool zhipu = base_url.find("bigmodel.cn") != std::string::npos ||
                       base_url.find("z.ai") != std::string::npos;
    if (!zhipu) return {};
    // 顺序是**推荐顺序**，不是字母序：设置页换家时挑的就是头一个，
    // 而字母序头一个是 glm-4.5，谁也不该先看见它。改顺序前想一下这件事。
    return {
        {"glm-4.7-flash",
         "免费 · 默认。限流很紧，成批写会慢；写作榜 47.8"},
        {"glm-5.3", "这一档最会写：写作榜 81.8、套话 7.09，八章几乎不降"},
        {"glm-5.3-flash", "便宜档。⚠️ 写作榜上没测过，别照 5.3 的分想当然"},
        {"glm-5.2", "写作榜 77.9"},
        {"glm-5", "写作榜 70.9"},
        {"glm-4.7", "写作榜 66.0"},
        {"glm-4.6", "写作榜 57.3"},
        {"glm-4.5", "写作榜 55.5"},
    };
}

namespace {

/// 连不上时那句话。三处在用，措辞得一样。
std::string connect_failed(const config::LLMConfig& cfg,
                           const std::string& why) {
    return "连不上大模型服务（" + cfg.base_url + "）。\n" + why;
}

}  // namespace

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

    config::LLMConfig cfg = cfg_();
    // **按任务分流。** 哪一步用哪个模型由 [llm.models] 定，见
    // config::LLMConfig::task_models：写正文那几步走文采好的，分镜人物
    // 那几步走听话的。认不出的任务落到 cfg.model。
    cfg.model = cfg.model_for(req.schema_name);
    // **温度也按任务分。** 编东西那几步要发散，拆分镜那几步要听话，
    // 见 LLMConfig::temperature_for。放在这儿而不是 build_payload 里：
    // 那个函数被一条「和 Python 逐字段一样」的语料钉着，加工混进去就分不清
    // 差异是谁造成的——和 model_for 当初放在这儿是同一个理由。
    // req 里自己填了温度的（比如写正文那步）照样盖过它，见 build_payload。
    cfg.temperature = cfg.temperature_for(req.schema_name);
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
    const auto run = [&] {
        Attempt a;
        nlohmann::ordered_json payload = build_payload(cfg, req);
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
                // **思考先推，正文后推。** 顺序反了的话，界面上会先冒出
                // 第一句正文、再冒出"它正在想"，看着像倒放。
                if (req.on_thinking) {
                    const std::string think = sse.take_thinking();
                    if (!think.empty()) req.on_thinking(think);
                }
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
                // 解不出来就当这次没成，下面退回整段那条
            }
        }
        return a;
    };

    // **只发一次。**
    //
    // 这儿原来挂着一部四档退档梯子（json_schema → 削过的 schema →
    // json_object → 什么都不发），为的是兜住各家对 response_format 支持得
    // 七零八落。2026-09-14 把 schema 整个搬进提示词之后它没有存在的理由了，
    // 而它的代价一直很实在：任何一个别的 400（限流之外的，比如 glm-5.3
    // 不收 thinking 的关闭值）都会被它读成"这家不支持 json_schema"，
    // 于是**悄悄**退到最宽那一档接着生成，日志上看一切正常。
    //
    // 剩下的唯一一条退路是"退回整段"，那条治的是另一件事：有的服务压根
    // 不支持 stream。
    Attempt a = run();
    if (a.canceled) throw LlmError("已取消");

    // 流里明说了出错：这条要报出来，不能当成"生成完了"——
    // 否则用户拿到的是一段空正文外加一句"写好了"。
    if (!a.sse_error.empty() && a.text.empty()) {
        throw LlmError("大模型服务报错：" + a.sse_error);
    }
    if (!a.text.empty()) return a.text;

    // 一个字都没流出来。退回整段那条：**它治的是"这家不支持 stream"**，
    // 不是结构问题。连不上、认证错这类也在那边统一报——explain_status
    // 已经把各种状态码翻成了能照着做的话。
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
    config::LLMConfig cfg = cfg_();
    // **按任务分流。** 哪一步用哪个模型由 [llm.models] 定，见
    // config::LLMConfig::task_models：写正文那几步走文采好的，分镜人物
    // 那几步走听话的。认不出的任务落到 cfg.model。
    cfg.model = cfg.model_for(req.schema_name);
    // **温度也按任务分。** 编东西那几步要发散，拆分镜那几步要听话，
    // 见 LLMConfig::temperature_for。放在这儿而不是 build_payload 里：
    // 那个函数被一条「和 Python 逐字段一样」的语料钉着，加工混进去就分不清
    // 差异是谁造成的——和 model_for 当初放在这儿是同一个理由。
    // req 里自己填了温度的（比如写正文那步）照样盖过它，见 build_payload。
    cfg.temperature = cfg.temperature_for(req.schema_name);
    const std::string url = cfg.base_url + "/chat/completions";
    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + cfg.api_key},
        {"Content-Type", "application/json"},
    };

    // **只发一次。** 退档梯子随 response_format 一起删了，理由见
    // build_payload 里那段。
    const nlohmann::ordered_json payload = build_payload(cfg, req);
    HttpResponse r = post_(url, payload.dump(), headers, cfg.timeout_s);
    if (r.transport_error.has_value()) {
        throw LlmError(connect_failed(cfg, *r.transport_error));
    }

    if (r.status >= 400) throw LlmError(explain_status(cfg, r.status, r.body));
    if (tok.cancelled()) throw LlmError("已取消");
    if (req.on_thinking) {
        const std::string think = extract_thinking(r.body);
        if (!think.empty()) req.on_thinking(think);
    }
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

// ---- 造一个客户端 ----

std::shared_ptr<Client> make_client(HttpPost post, HttpPostStream stream_post) {
    // **只有远端这一条路了。** 2026-09-14 把进程内那条（LocalClient +
    // LlamaChat）整个删了：现在的模型都要思考，而本地那条唯一的独门武器是
    // GBNF 语法采样——它和思考是冲突的（思考被语法堵在 JSON 里之后会挤进
    // 键名和字符串，实见于 chapter_write 那段注释），而结构约束已经整个交给
    // 提示词了。留着它只是养一条没人跑、迟早腐烂的路。
    //
    // llama.cpp 本身还在链：进程内配音（llama_tts）用的是它。
    return std::make_shared<RemoteClient>(
        ConfigProvider([] { return config::runtime().snapshot().llm; }),
        std::move(post), std::move(stream_post));
}

}  // namespace changji::llm
