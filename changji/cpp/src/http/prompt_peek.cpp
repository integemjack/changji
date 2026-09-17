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
