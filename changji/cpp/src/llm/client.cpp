#include "llm/client.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <functional>
#include <string>
#include <utility>

#include "config/runtime.hpp"
#include "llm/schema_validate.hpp"
#include "llm/sse.hpp"
#include "stages/prompts.inc.hpp"
#include "util/cancel_words.hpp"
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

std::string completion_reason(const std::string& raw_body) {
    const json body = json::parse(raw_body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded() || !body.is_object()) return {};
    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty() ||
        !(*choices)[0].is_object()) {
        return {};
    }
    const auto finish = (*choices)[0].find("finish_reason");
    return finish != (*choices)[0].end() && finish->is_string()
               ? finish->get<std::string>()
               : std::string();
}

void require_complete_reason(const std::string& reason) {
    if (reason.empty() || reason == "stop" || reason == "tool_calls") return;
    if (reason == "length") {
        throw LlmError("大模型输出达到长度上限，返回内容被截断。请提高该模型的输出上限，或缩短输入后重试");
    }
    if (reason == "content_filter") {
        throw LlmError("大模型服务的内容过滤器中止了生成");
    }
    throw LlmError("大模型没有正常完成生成（finish_reason=" + reason + "）");
}

std::string checked_output(const Request& req, std::string out) {
    if (const auto err = validate_structured_output(out, req.schema)) {
        throw LlmError("大模型输出不符合 " +
                       (req.schema_name.empty() ? std::string("JSON Schema")
                                                : req.schema_name + " Schema") +
                       "：" + *err);
    }
    return out;
}

}  // namespace

namespace {

/// 把 pydantic 自动生成的 `title` 摘掉。
///
/// **它是纯噪声。** 那一栏的值就是键名换个写法——`shot_id` 配
/// 「"title": "Shot Id"」、`visual_desc` 配「"title": "Visual Desc"」，
/// 模型从键名已经知道的东西再说一遍。2026-09-17 量的：分镜那份里 17 个，
/// 369 字符；这份 schema 是整条提示词里最大的一块。
///
/// 仓库里**没有一个手写的 title 注解**（`{"title", …}` 在各阶段的
/// 构造里一次都没出现），所以摘掉不会丢掉谁写的话。
///
/// ⚠️ **`properties` 底下那一层的键是字段名，不是关键字。** 剧本那份
/// schema 里就真有一个叫 `title` 的字段（`props["title"]`，一章的标题）。
/// 不分这一层的话，递归摘 title 会把那个字段整个删掉——模型于是再也不
/// 会填标题，而且一声不响。`required` 数组里的 "title" 是值不是键，
/// 下面数组那一支原样留着。
///
/// 只摘文字这一份，校验那边拿到的还是原样。
ordered strip_titles(const ordered& node) {
    if (node.is_array()) {
        ordered out = ordered::array();
        for (const auto& v : node) out.push_back(strip_titles(v));
        return out;
    }
    if (!node.is_object()) return node;
    ordered out = ordered::object();
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string& k = it.key();
        if (k == "title" && it.value().is_string()) continue;
        // 这几个底下一层是名字，原样留着，只往各自的值里走。
        if (k == "properties" || k == "$defs" || k == "definitions" ||
            k == "patternProperties") {
            ordered kids = ordered::object();
            for (auto c = it.value().begin(); c != it.value().end(); ++c) {
                kids[c.key()] = strip_titles(c.value());
            }
            out[k] = std::move(kids);
            continue;
        }
        out[k] = strip_titles(it.value());
    }
    return out;
}

/// 把没人 $ref 的 $defs 摘掉。
///
/// **这些定义是随模型的 JSON Schema 整份带进来的**，而各阶段只挑用得上的
/// 那几个字段、还把枚举内联在属性上（storyboard.cpp 里
/// `defs["CameraMove"]["enum"]` 读出来再写进 kept）。于是定义本身成了没人
/// 指向的孤儿，却照样贴给模型读。2026-09-17 量的分镜那份：CameraAngle、
/// CameraMove、ShotStatus、Transition、Lens 五个一次都没被引用，合计 635
/// 字符（带缩进一千出头），其中 CameraMove 那串枚举还和属性上内联的那份
/// 一模一样——**同一串东西模型读两遍**。
///
/// 只摘文字这一份，校验那边拿到的还是原样（它也只解析引用得到的那些）。
/// 传递闭包：被引用的定义自己再引用别的，那些也得留。
ordered prune_unused_defs(const ordered& schema) {
    const auto defs = schema.find("$defs");
    if (defs == schema.end() || !defs->is_object()) return schema;

    const auto refs_in = [](const ordered& node, std::set<std::string>& out) {
        const std::function<void(const ordered&)> walk = [&](const ordered& n) {
            if (n.is_object()) {
                for (auto it = n.begin(); it != n.end(); ++it) {
                    if (it.key() == "$ref" && it.value().is_string()) {
                        const std::string r = it.value().get<std::string>();
                        const std::string head = "#/$defs/";
                        if (r.rfind(head, 0) == 0) out.insert(r.substr(head.size()));
                    }
                    walk(it.value());
                }
            } else if (n.is_array()) {
                for (const auto& v : n) walk(v);
            }
        };
        walk(node);
    };

    // 先看正文引用了谁（不含 $defs 自己）
    ordered body = schema;
    body.erase("$defs");
    std::set<std::string> want;
    refs_in(body, want);
    // 传递闭包
    for (bool grew = true; grew;) {
        grew = false;
        const std::set<std::string> seen = want;
        for (const auto& name : seen) {
            const auto it = defs->find(name);
            if (it == defs->end()) continue;
            std::set<std::string> more;
            refs_in(*it, more);
            for (const auto& m : more) {
                if (want.insert(m).second) grew = true;
            }
        }
    }
    if (want.size() == defs->size()) return schema;

    ordered out = schema;
    ordered kept = ordered::object();
    for (auto it = defs->begin(); it != defs->end(); ++it) {
        if (want.count(it.key()) != 0) kept[it.key()] = it.value();
    }
    if (kept.empty()) {
        out.erase("$defs");
    } else {
        out["$defs"] = std::move(kept);
    }
    return out;
}

}  // namespace

std::string schema_as_prompt(const std::string& prompt, const ordered& schema) {
    if (schema.is_null() || schema.empty()) return prompt;
    // **换行留着，缩进的空格不留。**
    //
    // 这份东西是给模型读的，分镜那份有几十个字段；压成一行之后连括号配对
    // 都要一个一个数，所以换行要留。但**缩进的空格一个字都不告诉人任何
    // 事**——它是这份提示词里最大的一块里最没用的那一层：
    //
    //   分镜那份 schema，真正发出去的（剪过没人引用的 $defs）
    //     缩进 2   ~9300 字符                      ← 2026-09-17 之前
    //     缩进 1    7438 字符（空白 2000 上下）    ← 上一版
    //     缩进 0    5600 上下（只剩换行）          ← 现在
    //     压成一行  5000 上下
    //
    // 同一份提示词里那张硬性要求表才 859 字符。**真正臃肿的是 schema，
    // 而且一大半是排版。** 每一个带 schema 的阶段都省这一份（2026-09-14
    // 起 schema 只有这一种发法，见下面 build_payload）。
    //
    // 一个约束都没动：摘掉的是缩进和 pydantic 自动生成的 title，
    // enum / required / minItems / description 一条不少。
    return prompt + stages::prompt::llm::kSchemaSuffix +
           strip_titles(prune_unused_defs(schema)).dump(0);
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

    // **少想一点，只对认得这个参数的模型说。**
    //
    // 智谱的 `reasoning_effort` 默认是 max；文档写着 GLM-4.5 及以上支持
    // `thinking`，GLM-5.2 及以上支持 `reasoning_effort`（5.3 / 5.3-Flash
    // 只收 low / high / max）。这儿按**模型名**认，不按地址：同一个网关
    // 后面可以挂别家的模型，而认错的代价是别家收到不认识的字段直接 400——
    // 那会让所有生成一起挂，比"想得久"严重得多。
    //
    // 认不出就一个字段都不多发，和这一行加进来之前完全一样。
    if (!req.reasoning_effort.empty()) {
        std::string m = cfg.model;
        for (char& c : m) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        // glm-5.2 及以上。glm-5.3、glm-5.3-flash、glm-5.2-air 都在内。
        const bool glm5 = m.rfind("glm-5", 0) == 0;
        if (glm5) {
            payload["thinking"] = ordered{{"type", "enabled"}};
            payload["reasoning_effort"] = req.reasoning_effort;
        }
    }
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
                   "，或者去项目页「模型」那一行点一下编剧模型的名字，"
                   "在弹出来的窗口里换一个已经有的。";
        } else {
            hint = "大模型服务在 " + url + " 上没有这个接口。"
                   "多半是地址填错了——地址要带 /v1 结尾，"
                   "而且那台机器上的服务得真的起着。"
                   "去项目页「模型」那一行点一下编剧模型的名字，在那个窗口里改。";
        }
    } else if (status == 401 || status == 403) {
        // **三种情况要说三句不同的话。** 都说成「API Key 不对」的话，
        // 前两种会把人支去检查一个他根本没填过的东西。
        if (cfg.api_key.empty()) {
            hint = "还没填 API Key。去项目页「模型」那一行点一下编剧模型的"
                   "名字，在弹出来的窗口里填——"
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
                   "\n密钥本身多半没问题——去项目页那个模型窗口里换一个"
                   "模型试试。";
        } else {
            hint = "大模型服务拒绝了这次请求（" + std::to_string(status) +
                   "），八成是 API Key 不对。"
                   "去项目页「模型」那一行点一下编剧模型的名字，在那个窗口里换一把。";
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
                   "或者去服务商那边充值。改在项目页那个模型窗口里。";
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

/// 这一趟没走通时那句话。三处在用，措辞得一样。
///
/// **「连不上」和「连上了但没读完」是两回事，不能都说成连不上。**
///
/// 2026-09-16 实撞：拆分镜连砸三次，日志里写着「连不上大模型服务
/// （https://open.bigmodel.cn/…）」，而同一台机器 curl 那个地址秒回。
/// 照着这句话查了半小时网络、代理、重复进程，全是好的——真正的原因在
/// **第二行**：`Failed to read connection`，也就是连接建起来了、请求发出去
/// 了，读响应的时候断的。同一个服务几十分钟前写大纲还是好的，只有分镜这
/// 一步砸——那是这条流水线上最大的一个请求。
///
/// 两种说法指向两个完全不同的地方：「连不上」让人去查网络和地址，
/// 「读一半断了」让人去看请求是不是太大、超时够不够、服务端掐没掐。
/// 所以按 why 里的措辞分开说。
std::string connect_failed(const config::LLMConfig& cfg,
                           const std::string& why) {
    // cpp-httplib 的错误字样：连接阶段是 Connection/Connect，读写阶段是
    // Read/Write，超时是 Timeout。只要不是"压根没连上"，就别说连不上。
    const auto has = [&why](const char* w) {
        return why.find(w) != std::string::npos;
    };
    if (has("read") || has("Read") || has("write") || has("Write") ||
        has("timeout") || has("Timeout")) {
        return "和大模型服务连上了，但这一趟没走完（" + cfg.base_url + "）。\n" +
               why +
               "\n请求发出去了、响应没读回来。多半是这一趟太大或太慢："
               "把 [llm].timeout_s 调大，或者换一个上下文更长的模型；"
               "服务端限流时也会这样。**不是网络不通**——网络不通的话下面"
               "那行写的是连接失败。";
    }
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
    // 没人要逐字、也没人要看思考，或者没注入流式发送函数，就走整段那条。
    // 基类那个默认实现会把整段回调一次，形状是一样的。
    if ((!on_token && !req.on_thinking) || !stream_post_) {
        return Client::complete(req, tok, on_token);
    }
    if (tok.cancelled()) throw LlmError(util::kCancelled);

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
        std::string response_body;
        std::string finish_reason;
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
                if (on_token) on_token(piece);
                return true;
            });
        a.status = r.status;
        a.response_body = r.body;
        a.transport_error = r.transport_error;
        a.sse_error = sse.error();
        a.finish_reason = sse.finish_reason();
        // **服务端没理会 stream 的情况**：它回了一份普通的 JSON，SSE 解不
        // 出任何东西。那份 body 在 r.body 里（流式那条只在出错时收 body，
        // 但"整份 JSON"和"错误体"在传输上没区别），试着按整段解一次。
        if (a.text.empty() && a.status < 400 && !r.body.empty()) {
            try {
                a.finish_reason = completion_reason(r.body);
                a.text = extract_content(r.body);
                if (on_token && !a.text.empty()) on_token(a.text);
            } catch (const std::exception&) {
                // 解不出来就由下面按本次响应直接报错；不重复发送同一个请求。
            }
        }
        return a;
    };

    // **只发一次。** 不支持 SSE 但直接返回普通 OpenAI JSON 的服务会在
    // run() 里就地解析同一份响应；空响应和错误响应都不会自动再发一次，
    // 避免重复生成和重复计费。
    Attempt a = run();
    if (a.canceled) throw LlmError(util::kCancelled);
    if (a.transport_error.has_value()) {
        throw LlmError(connect_failed(cfg, *a.transport_error));
    }
    if (a.status >= 400) {
        throw LlmError(explain_status(cfg, a.status, a.response_body), a.status);
    }

    // 服务在流里报错时，即使前面已经吐过半份正文也必须报真实错误。
    // 把半份正文交给解析器，只会把它伪装成“JSON 格式错误”。
    if (!a.sse_error.empty()) {
        throw LlmError("大模型服务报错：" + a.sse_error);
    }
    require_complete_reason(a.finish_reason);
    if (a.text.empty()) {
        throw LlmError("大模型服务没有返回正文；本次请求不会自动重发，以免重复生成或重复计费");
    }
    return checked_output(req, std::move(a.text));
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
    // ⚠️ **有人要看思考，就走流式那条，哪怕没人要逐字。**
    //
    // 这一条**只能写在这儿**：不带 `on_token` 的调用方（改编成剧本、拆
    // 分镜、定妆）走的就是这个两参重载，根本到不了上面那个三参的分流点。
    // 我 2026-09-17 第一次就改错了地方，用例当场红。
    //
    // 为什么非流式不行：整段那条只能在**收完之后**把整段思考一次性给出去
    // （见下面那段）。而「改编成剧本」实测跑 11 分半——这 11 分半里任务
    // 页面上那一行一个字都没有，跑完那一下才蹦出 19 万字。用户
    // 2026-09-17：「思考的内容还是看不到」。**思考的用处全在跑的过程里**，
    // 它是唯一能回答"它还活着吗、在想什么"的东西，跑完再给等于没给。
    //
    // 走流式不多花什么：除了 `stream: true`，发的是同一份；服务端不理会
    // stream 的情况那条路上本来就有退路。
    if (stream_post_ && req.on_thinking) return complete(req, tok, OnToken{});

    if (tok.cancelled()) throw LlmError(util::kCancelled);

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

    if (r.status >= 400) {
        throw LlmError(explain_status(cfg, r.status, r.body), r.status);
    }
    if (tok.cancelled()) throw LlmError(util::kCancelled);
    if (req.on_thinking) {
        const std::string think = extract_thinking(r.body);
        if (!think.empty()) req.on_thinking(think);
    }
    require_complete_reason(completion_reason(r.body));
    return checked_output(req, extract_content(r.body));
}

// ---- ReplayClient ----

namespace {

ordered message_json(const Message& m) {
    ordered j = ordered::object();
    j["role"] = m.role;
    if (m.role == "assistant" && !m.tool_calls.empty()) {
        // OpenAI 那套：带 tool_calls 的那条 content 可以是 null
        if (m.content.empty()) j["content"] = nullptr;
        else j["content"] = m.content;
        ordered calls = ordered::array();
        for (const auto& c : m.tool_calls) {
            calls.push_back(ordered{{"id", c.id},
                                    {"type", "function"},
                                    {"function", ordered{{"name", c.name},
                                                         {"arguments", c.arguments}}}});
        }
        j["tool_calls"] = calls;
    } else {
        j["content"] = m.content;
    }
    if (m.role == "tool") j["tool_call_id"] = m.tool_call_id;
    return j;
}

ChatReply parse_chat_reply(const std::string& raw_body) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(raw_body);
    } catch (const std::exception&) {
        throw LlmError("大模型返回的不是 JSON：" + text::truncate_utf8(raw_body, 400));
    }
    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty()) {
        throw LlmError("大模型返回格式异常：" + text::truncate_utf8(body.dump(), 400));
    }
    const auto& first = (*choices)[0];
    const auto msg = first.find("message");
    if (msg == first.end() || !msg->is_object()) {
        throw LlmError("大模型返回格式异常：" + text::truncate_utf8(body.dump(), 400));
    }
    ChatReply r;
    if (const auto c = msg->find("content"); c != msg->end() && c->is_string()) {
        r.content = c->get<std::string>();
    }
    if (const auto fr = first.find("finish_reason"); fr != first.end() && fr->is_string()) {
        r.finish_reason = fr->get<std::string>();
    }
    if (const auto tc = msg->find("tool_calls"); tc != msg->end() && tc->is_array()) {
        for (const auto& item : *tc) {
            if (!item.is_object()) continue;
            ToolCall call;
            call.id = item.value("id", std::string());
            const auto fn = item.find("function");
            if (fn != item.end() && fn->is_object()) {
                call.name = fn->value("name", std::string());
                const auto args = fn->find("arguments");
                if (args != fn->end()) {
                    call.arguments = args->is_string() ? args->get<std::string>() : args->dump();
                }
            }
            if (!call.name.empty()) r.tool_calls.push_back(std::move(call));
        }
    }
    return r;
}

}  // namespace

ChatReply Client::chat(const std::vector<Message>&, const ordered&, const Request&,
                       pipeline::CancelToken&) {
    throw LlmError("这个大模型后端不带工具调用");
}

ChatReply RemoteClient::chat(const std::vector<Message>& messages, const ordered& tools,
                             const Request& opts, pipeline::CancelToken& tok) {
    if (tok.cancelled()) throw LlmError(util::kCancelled);

    config::LLMConfig cfg = cfg_();
    cfg.model = cfg.model_for(opts.schema_name);
    cfg.temperature = cfg.temperature_for(opts.schema_name);
    const std::string url = cfg.base_url + "/chat/completions";
    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + cfg.api_key},
        {"Content-Type", "application/json"},
    };

    // 和 build_payload 同一套：模型、温度、GLM-5 的 thinking。多的只是
    // messages 是整段来回、外加 tools。
    ordered payload = ordered::object();
    payload["model"] = cfg.model;
    ordered msgs = ordered::array();
    for (const auto& m : messages) msgs.push_back(message_json(m));
    payload["messages"] = msgs;
    payload["temperature"] = opts.temperature.value_or(cfg.temperature);
    if (tools.is_array() && !tools.empty()) {
        payload["tools"] = tools;
        payload["tool_choice"] = "auto";
    }
    if (!opts.reasoning_effort.empty()) {
        std::string m = cfg.model;
        for (char& c : m) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (m.rfind("glm-5", 0) == 0) {
            payload["thinking"] = ordered{{"type", "enabled"}};
            payload["reasoning_effort"] = opts.reasoning_effort;
        }
    }

    HttpResponse r = post_(url, payload.dump(), headers, cfg.timeout_s);
    if (r.transport_error.has_value()) {
        throw LlmError(connect_failed(cfg, *r.transport_error));
    }
    if (r.status >= 400) {
        throw LlmError(explain_status(cfg, r.status, r.body), r.status);
    }
    if (tok.cancelled()) throw LlmError(util::kCancelled);
    if (opts.on_thinking) {
        const std::string think = extract_thinking(r.body);
        if (!think.empty()) opts.on_thinking(think);
    }
    return parse_chat_reply(r.body);
}

ReplayClient::ReplayClient(std::vector<std::string> responses)
    : responses_(std::move(responses)) {}

ChatReply ReplayClient::chat(const std::vector<Message>& messages, const ordered&,
                            const Request& opts, pipeline::CancelToken& tok) {
    if (tok.cancelled()) throw LlmError(util::kCancelled);
    Request seen = opts;
    seen.prompt = messages.empty() ? std::string() : messages.back().content;
    calls_.push_back(seen);
    last_messages_ = messages;
    if (next_ >= responses_.size()) {
        throw LlmError("回放录到头了：这是第 " + std::to_string(next_ + 1) +
                       " 次调用，只录了 " + std::to_string(responses_.size()) +
                       " 条");
    }
    const std::string raw = responses_[next_++];
    ChatReply r;
    try {
        const nlohmann::json j = nlohmann::json::parse(raw);
        if (j.is_object() && j.contains("tool_calls") && j["tool_calls"].is_array()) {
            int k = 0;
            for (const auto& item : j["tool_calls"]) {
                ToolCall c;
                c.id = item.value("id", "call_" + std::to_string(++k));
                c.name = item.value("name", std::string());
                const auto a = item.find("arguments");
                if (a != item.end()) c.arguments = a->is_string() ? a->get<std::string>() : a->dump();
                r.tool_calls.push_back(std::move(c));
            }
            r.finish_reason = "tool_calls";
            return r;
        }
    } catch (const std::exception&) {
        // 不是 JSON：就是一段话
    }
    r.content = raw;
    r.finish_reason = "stop";
    return r;
}

std::string ReplayClient::complete(const Request& req,
                                   pipeline::CancelToken& tok) {
    if (tok.cancelled()) throw LlmError(util::kCancelled);
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
    //
    // 2026-09-18 多了第二条：`backend = "command"`，把本机装着的大模型命令行
    // （claude / codex）当后端。**在这儿分叉，不在每个调用点**——叫模型的地方
    // 散在六七个文件里，它们只认 `Client` 这个接口。
    //
    // 判断放在工厂里而不是造一次记一次：`ConfigProvider` 每次都读最新配置，
    // 用户在设置页上换了后端，下一趟就该走新的那条。
    if (config::runtime().snapshot().llm.backend == "command") {
        return std::make_shared<CommandClient>(
            ConfigProvider([] { return config::runtime().snapshot().llm; }));
    }
    return std::make_shared<RemoteClient>(
        ConfigProvider([] { return config::runtime().snapshot().llm; }),
        std::move(post), std::move(stream_post));
}

}  // namespace changji::llm
