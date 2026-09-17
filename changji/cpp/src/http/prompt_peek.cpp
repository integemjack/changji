#include "http/prompt_peek.hpp"

#include <utility>

namespace changji::http {

bool take_peek(nlohmann::json& body) {
    if (!body.is_object()) return false;
    const auto it = body.find("peek");
    if (it == body.end()) return false;
    const bool on = it->is_boolean() ? it->get<bool>() : false;
    body.erase(it);
    return on;
}

std::string take_paste(nlohmann::json& body) {
    if (!body.is_object()) return {};
    const auto it = body.find("paste");
    if (it == body.end()) return {};
    std::string raw = it->is_string() ? it->get<std::string>() : std::string();
    body.erase(it);
    return raw;
}

std::vector<std::string> split_by_scene(const std::string& pasted) {
    // 行首是 `===== 第 ` 就算一个头。**按行认，不按正则**：场次名里什么
    // 字符都可能有（「日 · 外 · 后门货场」带点带空格），而这一行的开头
    // 是我们自己拼的，认它最稳。
    static const std::string kMark = "===== 第 ";
    std::vector<std::string> out;
    std::string cur;
    bool seen_head = false;
    std::size_t i = 0;
    while (i <= pasted.size()) {
        const std::size_t nl = pasted.find('\n', i);
        const std::string line =
            pasted.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        const bool head = line.rfind(kMark, 0) == 0;
        if (head) {
            if (seen_head) out.push_back(cur);
            cur.clear();
            seen_head = true;
        } else if (seen_head) {
            cur += line;
            cur += '\n';
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    if (seen_head) out.push_back(cur);
    // 一个头都没有：整集一次拆那条路只有一段，别逼人去写分隔头。
    if (out.empty() && !pasted.empty()) out.push_back(pasted);
    return out;
}

ApiResult peek_prompt(const llm::Request& req) {
    return {200,
            {{"peek", true},
             {"stage", req.schema_name},
             // **就是这一份**：user 消息里落的那段。schema 不走
             // response_format，是以文字贴在提示词后面的（见 schema_as_prompt），
             // 所以贴出去的这一段就是模型看到的全部。
             {"prompt", llm::schema_as_prompt(req.prompt, req.schema)}}};
}

}  // namespace changji::http
