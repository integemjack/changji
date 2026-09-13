#include "llm/client.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <set>
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

/// 严格模式不认的校验关键字。见 remote_schema 的注释。
bool dropped_keyword(const std::string& k) {
    static const std::set<std::string> kDrop = {
        "minItems",     "maxItems",      "uniqueItems",
        "minContains",  "maxContains",   "contains",
        "unevaluatedItems", "unevaluatedProperties",
        "minLength",    "maxLength",     "pattern",        "format",
        "minimum",      "maximum",       "exclusiveMinimum",
        "exclusiveMaximum", "multipleOf",
        "minProperties", "maxProperties", "patternProperties",
        "propertyNames"};
    return kDrop.count(k) != 0;
}

/// 这些键下面挂的是「名字 → 子 schema」，键名是属性名不是关键字。
///
/// 不分这一支的话，一个真的叫 `pattern` 的字段会被当成关键字摘掉。
bool schema_map_key(const std::string& k) {
    return k == "properties" || k == "$defs" || k == "definitions";
}

/// 这些键下面挂的是**值**，不是 schema，不能往里递归。
bool literal_key(const std::string& k) {
    return k == "enum" || k == "const" || k == "required" ||
           k == "default" || k == "examples";
}

std::string num_to_text(const ordered& v) {
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) {
        std::string s = std::to_string(v.get<double>());
        // 去掉小数点后面那串没意义的 0
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }
    return v.dump();
}

/// 一条「N~M」的话。两头都没有就返回空串。
std::string range_note(const ordered& node, const char* lo_key,
                       const char* hi_key, const char* unit) {
    const auto lo = node.find(lo_key);
    const auto hi = node.find(hi_key);
    const bool has_lo = lo != node.end() && lo->is_number();
    const bool has_hi = hi != node.end() && hi->is_number();
    if (!has_lo && !has_hi) return {};
    if (has_lo && has_hi) {
        return "（" + num_to_text(*lo) + "~" + num_to_text(*hi) + unit + "）";
    }
    if (has_lo) return "（至少 " + num_to_text(*lo) + unit + "）";
    return "（最多 " + num_to_text(*hi) + unit + "）";
}

ordered strip_schema(const ordered& node) {
    if (node.is_array()) {
        ordered out = ordered::array();
        for (const auto& item : node) out.push_back(strip_schema(item));
        return out;
    }
    if (!node.is_object()) return node;

    // 先把要摘掉的那些数折成一句话。**次序固定**：项数、字数、取值范围。
    std::string note = range_note(node, "minItems", "maxItems", " 项");
    note += range_note(node, "minLength", "maxLength", " 字");
    note += range_note(node, "minimum", "maximum", "");
    const auto uniq = node.find("uniqueItems");
    if (uniq != node.end() && uniq->is_boolean() && uniq->get<bool>()) {
        note += "（不要重复）";
    }

    ordered out = ordered::object();
    for (const auto& [k, v] : node.items()) {
        if (dropped_keyword(k)) continue;
        if (literal_key(k)) {
            out[k] = v;
        } else if (schema_map_key(k) && v.is_object()) {
            ordered m = ordered::object();
            for (const auto& [name, sub] : v.items()) m[name] = strip_schema(sub);
            out[k] = m;
        } else {
            out[k] = strip_schema(v);
        }
    }

    if (!note.empty()) {
        const auto desc = out.find("description");
        // 赋值给已有的键不会挪位置，所以描述还在它原来那一栏上。
        out["description"] =
            (desc != out.end() && desc->is_string())
                ? desc->get<std::string>() + note
                : note;
    }
    return out;
}

}  // namespace

ordered remote_schema(const ordered& schema) {
    if (schema.is_null() || schema.empty()) return schema;
    return strip_schema(schema);
}

std::string schema_as_prompt(const std::string& prompt, const ordered& schema) {
    if (schema.is_null() || schema.empty()) return prompt;
    // 缩进两格而不是压成一行：这份东西是给模型读的，分镜那份有几十个字段，
    // 压成一行之后连人都看不出哪个字段套在哪个里面。
    return prompt +
           "\n\n只输出一个 JSON 对象，前后不要有别的话，也不要放进代码块里。"
           "它必须符合下面这份 JSON Schema——字段名、层级、枚举值、"
           "还有描述里写的那些数量和字数限制，都要照着来：\n" +
           schema.dump(2);
}

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
        // **三种情况要说三句不同的话。** 都说成「API Key 不对」的话，
        // 前两种会把人支去检查一个他根本没填过的东西。
        if (cfg.api_key.empty()) {
            hint = "还没填 API Key。去设置页的「大模型」那一节填上——"
                   "当前地址是 " + cfg.base_url + "。";
            if (cfg.base_url.find("openrouter.ai") != std::string::npos) {
                hint += "去 openrouter.ai 注册领一把——默认挑的那几个"
                        "模型本身不要钱，但网关仍然要认人。";
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
                   "换一个免费模型（OpenRouter 上是带 :free 后缀的那些），"
                   "或者去服务商那边充值。去设置页的「大模型」那一节改。";
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

namespace {

/// 发往 OpenRouter 时额外要带的字段。
///
/// 现在只有一个：**关掉「先想再写」**。理由见
/// config::LLMConfig::reasoning——一句话是，开着的话长任务上这几个模型
/// 要么把英文思考稿当正文交上来，要么回一个空的 content。
///
/// **只对 OpenRouter 发**：`reasoning` 是它的统一参数，别家不认，
/// 发过去可能被当成非法字段整个打回——而那会被我们的退路误判成
/// "这家不支持 json_schema"，白白退两档。
void apply_remote_extras(const config::LLMConfig& cfg,
                         nlohmann::ordered_json& payload) {
    if (cfg.reasoning) return;
    if (cfg.base_url.find("openrouter.ai") == std::string::npos) return;
    payload["reasoning"] = nlohmann::ordered_json{{"enabled", false}};
}

/// 远端这条路上真正发出去的那份 Request。
///
/// 两条路各做各的加工，见 remote_schema / schema_as_prompt 的注释：
///   json_schema 那一下  —— 削掉严格模式不认的校验关键字；
///   json_object 那一下  —— 把整份 schema 抄进提示词，否则这一下模型
///                          连字段名都不知道。
Request remote_request(const Request& req, bool json_schema_mode) {
    Request out = req;
    if (req.schema.is_null() || req.schema.empty()) return out;
    if (json_schema_mode) {
        out.schema = remote_schema(req.schema);
    } else {
        out.prompt = schema_as_prompt(req.prompt, req.schema);
    }
    return out;
}

/// 最严那一档：**schema 一个字不动**，minItems 那些照样发出去。

/// 最宽那一档：**连 `response_format` 都不发**，schema 只在提示词里。
///
/// 把 `schema` 清空是让 build_payload 整个跳过 response_format
/// （它对空 schema 就是这么处理的，注释写着"加一个空的会被某些服务拒掉"）。
/// OpenRouter 上一大半模型的 `supported_parameters` 里压根没有
/// response_format——它们不是不会写 JSON，是网关不收这个字段。
Request plain_request(const Request& req) {
    Request out = req;
    out.prompt = schema_as_prompt(req.prompt, req.schema);
    out.schema = nlohmann::ordered_json();
    return out;
}

/// 退到 json_object 那一下说一声。
///
/// **不能只是悄悄退。** 这一步之后结构就没有语法保证了，只剩提示词里
/// 那份 schema 和解析阶段的兜底；写出来的东西会变差，而所有接口照样
/// 返回 200。用户看到的是"最近生成的质量忽然不稳"，日志里什么都没有。
/// 这个状态码值不值得"去掉 schema 再试一次"。
///
/// 原来的规矩是**无条件**退（抄 Python 的），理由写着"各家对『不支持这个
/// response_format』回的码五花八门，400、404、422 都见过，判断不现实"。
/// 那句话只对了一半：**有几个码的含义是明确的，和 schema 一点关系没有。**
///
/// 2026-09-13 实测撞上了：智谱那个免费模型限流很勤，连发三次全是
/// `429 / code 1305 该模型当前请求量较大`。按老规矩，每一次限流都会变成
/// 一次"去掉 schema 重发"——**而第二次多半会成功**，于是这一集的分镜就在
/// 没有任何结构约束的情况下生成完了，日志上看起来一切正常。
/// 密钥错（401/403）同理：重发一次照样错，白多一次请求。
///
/// 所以这几个码一律不退，直接把原来那句人话报出去。剩下的仍然无条件退：
/// 那部分"判断不现实"的论证还是成立的。
bool worth_dropping_schema(int status) {
    switch (status) {
        case 401:   // 密钥不对
        case 403:   // 没权限
        case 408:   // 超时
        case 429:   // 限流
            return false;
        default:
            return true;
    }
}

/// 要了 JSON，回来的却不是 JSON。
///
/// **2026-09-13 在智谱的 glm-4.7-flash 上实测到的，这条比 400 那条更阴。**
/// 它对 `response_format: json_schema` 既不报错也不照做——**回 200，内容是
/// 一段 markdown 散文**（"1. **暴风雨中，他独自伫立在天台边缘……**"）。
/// 只按状态码判的话退路永远不触发：这一层拿到 200 就把散文原样交出去，
/// 到调用方那里才炸成一句"找不到合法 JSON"，而真正的原因（这家不吃
/// json_schema）在任何一条日志里都看不见。
///
/// 判据只看第一个非空白字符是不是 `{` 或 `[`。**不做完整解析**：模型常在
/// JSON 外面包一层 ```json（解析阶段的括号扫描本来就兜着这种），那种情况
/// 结构是在的，不该白白再发一次请求。散文开头不可能是花括号。
bool looks_like_json(const std::string& s) {
    for (const char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '{' || c == '[';
    }
    return false;   // 全是空白：当成没拿到东西，也该退一步再试
}

void warn_schema_fallback(const config::LLMConfig& cfg, int next_rung) {
    static const char* kWhat[] = {
        "",
        "「削掉 minItems 那些校验关键字的 schema」——数量和字数限制只剩描述里那句话",
        "「json_object + schema 写进提示词」——结构没有硬保证了",
        "「什么 response_format 都不发」——全靠提示词",
    };
    std::fprintf(stderr, "[llm] %s 上的 %s 这一下没成，退到%s\n",
                 cfg.base_url.c_str(), cfg.model.c_str(),
                 kWhat[next_rung < 1 || next_rung > 3 ? 3 : next_rung]);
}

/// 记下"这个地址上的这个模型不吃 json_schema"，往后直接走退路。
///
/// **这一笔省的不是一点点。** glm-4.7-flash 上实测：json_schema 那一下要
/// 78.6 秒（吐一篇散文），退路那一下 12.4 秒（吐的是合规 JSON）。不记的话
/// 每一次调用都要先白花那 78 秒，而免费档本来就限流——一集十几次调用，
/// 光浪费的就是二十分钟，还把配额烧在没用的请求上。
///
/// **只在"退路真的成功了"时才记。** 这是唯一确凿的证据：带 schema 那一下
/// 拿不到东西、不带的那一下拿到了。按状态码记的话，一次偶发的 500 就会让
/// 整个进程往后都不再用 json_schema，而那是我们在别的服务上最想要的东西。
///
/// 进程内的，不落盘：换服务商、对方上线了 json_schema 支持，重启一次就重新
/// 探。键里带模型名——同一个地址上不同模型的支持情况不一样。
class SchemaSupport {
public:
    /// 这个服务能吃到哪一档。**从严到宽**，编号就是退的顺序。
    enum class Mode {
        Raw = 0,         ///< json_schema，**schema 原样**（minItems 都在）
        Schema = 1,      ///< json_schema，削掉严格模式不认的校验关键字
        JsonObject = 2,  ///< response_format: json_object，schema 写进提示词
        Plain = 3,       ///< 什么 response_format 都不发，只靠提示词
    };

    /// **第一档为什么是"原样发"。** 2026-09-13 实测：OpenRouter 根本不拒
    /// minItems/minLength，原样发回 200 而且真按数量给了。而我们本来是
    /// 无条件削的——那等于把 project-ai-chapter-quality 里那根
    /// 「管得住模型的不是措辞，是它没得选」的杠杆白丢了。
    ///
    /// 削是给**真会退 400** 的服务（OpenAI 严格模式那类）准备的退路，
    /// 不该当第一手。退一档的代价是多发一次请求，只在第一次发生，
    /// 之后 SchemaSupport 记着。

    /// **第三档是给"连 json_object 都不认"的模型用的。**
    /// OpenRouter 上一大半模型属于这种：`supported_parameters` 里压根没有
    /// `response_format`（2026-09-13 查的清单：Inkling、Nemotron 3 Ultra、
    /// Ling 3.0、Laguna、North 都没有）。少这一档的话，那些模型上两次请求
    /// 都会被网关打回来，而它们其实只要把 schema 写在提示词里就写得出 JSON。
    static Mode mode_for(const config::LLMConfig& cfg) {
        std::lock_guard<std::mutex> g(mu_);
        const auto it = known_.find(key(cfg));
        // 没记过就从最严那一档起：schema 原样发，minItems 那些都在。
        return it == known_.end() ? Mode::Raw : it->second;
    }

    /// 记下"这一档才走得通"。只在那一档**真的成功了**时调。
    static void remember(const config::LLMConfig& cfg, Mode m) {
        if (m == Mode::Raw) return;   // 最严那档本来就是默认，不用记
        std::lock_guard<std::mutex> g(mu_);
        const std::string k = key(cfg);
        const auto it = known_.find(k);
        if (it != known_.end() && it->second >= m) return;   // 已经记得更宽了
        known_[k] = m;
        std::fprintf(stderr,
                     "[llm] 记下了：%s 上的 %s %s，这个进程往后直接走那一档\n",
                     cfg.base_url.c_str(), cfg.model.c_str(),
                     m == Mode::Schema
                         ? "不收带 minItems 那些关键字的 schema，得先削一遍"
                     : m == Mode::JsonObject
                         ? "不支持 json_schema，只能 json_object + schema 写进提示词"
                         : "连 response_format 都不收，只能把 schema 写进提示词");
    }

private:
    static std::string key(const config::LLMConfig& cfg) {
        return cfg.base_url + "\n" + cfg.model;
    }
    static void forget_all() {
        std::lock_guard<std::mutex> g(mu_);
        known_.clear();
    }
    friend void ::changji::llm::reset_schema_support();

    static std::mutex mu_;
    static std::map<std::string, Mode> known_;
};

std::mutex SchemaSupport::mu_;
std::map<std::string, SchemaSupport::Mode> SchemaSupport::known_;

}  // namespace

void reset_schema_support() { SchemaSupport::forget_all(); }

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
    using Mode = SchemaSupport::Mode;
    const auto run = [&](Mode m) {
        Attempt a;
        const bool as_schema = (m == Mode::Raw || m == Mode::Schema);
        const Request sent = m == Mode::Raw      ? req
                             : m == Mode::Plain  ? plain_request(req)
                                                 : remote_request(req, as_schema);
        nlohmann::ordered_json payload = build_payload(cfg, sent, as_schema);
        apply_remote_extras(cfg, payload);
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

    // **四条退路，一条都不能少。**
    //
    //   1. json_schema 的 SSE —— 正常那条
    //   2. json_object 的 SSE —— 有些服务不认 json_schema（智谱那个免费模型
    //      甚至不报错，直接给你一段散文）
    //   3. 什么 response_format 都不发的 SSE —— OpenRouter 上一大半模型的
    //      supported_parameters 里压根没有 response_format
    //   4. 整段 —— 压根不支持 stream 的服务（或者流里什么都没给）
    //
    // 第 4 条是"接了 SSE 不会让任何一种服务变得更糟"的全部保证。
    const bool wanted_json = !req.schema.is_null() && !req.schema.empty();

    /// 这一趟的东西能不能用。
    ///
    /// **"流出来了东西"不等于"流出来的是我们要的东西"。** 智谱那个免费模型
    /// 对 json_schema 既不报错也不照做，流里下来的是一段散文；照单收下的话
    /// 这一层返回 200 和一段好好的文字，炸在调用方的 JSON 解析上。
    const auto usable = [&](const Attempt& x) {
        return !x.text.empty() && x.sse_error.empty() &&
               (!wanted_json || looks_like_json(x.text));
    };

    Mode mode = SchemaSupport::mode_for(cfg);
    Attempt a = run(mode);
    if (a.canceled) throw LlmError("已取消");

    // 退一步重来会让 on_token 再走一遍——界面上那一下会先看到散文再被
    // JSON 顶掉，难看，但比拿不到结果强；而且真正吃这条流的是按字段解析的
    // JsonFieldStreamer，散文在它眼里什么都不是。
    //
    // 限流/认证错那几个码不退（见 worth_dropping_schema）：重发一次照样错，
    // 白烧一次配额，而免费档的配额本来就紧。
    while (!usable(a) && mode != Mode::Plain &&
           (a.status < 400 || worth_dropping_schema(a.status))) {
        if (tok.cancelled()) throw LlmError("已取消");
        mode = static_cast<Mode>(static_cast<int>(mode) + 1);
        warn_schema_fallback(cfg, static_cast<int>(mode));
        Attempt b = run(mode);
        if (b.canceled) throw LlmError("已取消");
        if (usable(b)) {
            SchemaSupport::remember(cfg, mode);
            return b.text;
        }
        a = std::move(b);
    }
    if (usable(a)) return a.text;

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
    config::LLMConfig cfg = cfg_();
    // **按任务分流。** 哪一步用哪个模型由 [llm.models] 定，见
    // config::LLMConfig::task_models：写正文那几步走文采好的，分镜人物
    // 那几步走听话的。认不出的任务落到 cfg.model。
    cfg.model = cfg.model_for(req.schema_name);
    const std::string url = cfg.base_url + "/chat/completions";
    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + cfg.api_key},
        {"Content-Type", "application/json"},
    };

    const bool wanted_json = !req.schema.is_null() && !req.schema.empty();

    // 三档，从严到宽（见 SchemaSupport::Mode）。已经验明这个服务只吃某一档的
    // 话，第一下就从那儿开始——省下的是一次白花的完整生成。
    using Mode = SchemaSupport::Mode;
    Mode mode = SchemaSupport::mode_for(cfg);

    const auto send = [&](Mode m) {
        const bool as_schema = (m == Mode::Raw || m == Mode::Schema);
        const Request sent = m == Mode::Raw      ? req
                             : m == Mode::Plain  ? plain_request(req)
                                                 : remote_request(req, as_schema);
        nlohmann::ordered_json payload = build_payload(cfg, sent, as_schema);
        apply_remote_extras(cfg, payload);
        return post_(url, payload.dump(), headers, cfg.timeout_s);
    };

    /// 这一下算不算"没成"。两种：状态码说不行，**或者回了 200 却不是 JSON**。
    ///
    /// 后一条是智谱那个免费模型的实际行为（见 looks_like_json）：对
    /// json_schema 既不报错也不照做，回 200 加一段散文。只判状态码的版本在
    /// 它身上一次都退不成，而且不报错。
    const auto failed = [&](const HttpResponse& resp) {
        if (resp.status >= 400) return worth_dropping_schema(resp.status);
        if (!wanted_json) return false;
        const json body = json::parse(resp.body, nullptr, false);
        // 抽不出内容是另一类问题（返回格式异常），交给 extract_content 去报，
        // 别在这儿多发一次请求。
        if (body.is_discarded()) return false;
        const auto c = body.find("choices");
        if (c == body.end() || !c->is_array() || c->empty()) return false;
        const auto m = (*c)[0].find("message");
        if (m == (*c)[0].end() || !m->is_object()) return false;
        const auto x = m->find("content");
        if (x == m->end() || !x->is_string()) return false;
        return !looks_like_json(x->get<std::string>());
    };

    HttpResponse r = send(mode);
    if (r.transport_error.has_value()) {
        throw LlmError("连不上大模型服务（" + cfg.base_url + "）。\n" +
                       *r.transport_error);
    }

    // 往下退，一档一档试。
    //
    // 除了那几个含义明确的码（见 worth_dropping_schema），其余一律退：
    // 各家服务对"不支持这个 response_format"回的码五花八门，400、404、422
    // 都见过，判断哪个是"不支持"哪个是"真错了"不现实。代价是真的地址错了
    // 会多发几次请求。
    while (failed(r) && mode != Mode::Plain) {
        if (tok.cancelled()) throw LlmError("已取消");
        mode = static_cast<Mode>(static_cast<int>(mode) + 1);
        warn_schema_fallback(cfg, static_cast<int>(mode));
        r = send(mode);
        if (r.transport_error.has_value()) {
            throw LlmError("连不上大模型服务（" + cfg.base_url + "）。\n" +
                           *r.transport_error);
        }
        // 上一档不行、这一档行了——这就是"它只吃到这儿"的确凿证据。
        if (r.status < 400 && !failed(r)) SchemaSupport::remember(cfg, mode);
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
