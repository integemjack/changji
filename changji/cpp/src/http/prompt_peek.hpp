#pragma once

// 「只看不发」：把这一步真正要发给大模型的那段字原样回给界面。
//
// 用户 2026-09-17：「每个用大语言模型的地方增加一个复制提示词的按钮，我可以
// 复制放到别的地方生产后粘贴内容过来」。
//
// **不另拼一份。** 复制出去的那段字必须和真发出去的一模一样，差一个字，他在
// 别处跑出来的东西就对不上我们的解析器——而那种错没有任何报错，表现要么是
// "粘回来解析失败"，要么更糟："解析成功但内容不是我要的"。
//
// 所以做法是在每个处理函数里、`req` 拼好之后插一句：带了 peek 就把 req 里
// 那份原样回去，不调模型。同一行代码产生的字，天生不会和真跑那份飘开。
// （抄一份的话两边迟早只改一边，这个仓库里已经栽过好几次。）

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"   // ApiResult
#include "llm/client.hpp"

namespace changji::http {

/// 请求体里带没带 `peek`。**取完就删掉**，免得下游的 forbid_extra 拦下来。
bool take_peek(nlohmann::json& body);

/// 这一步的提示词，连 schema 一起（`llm::schema_as_prompt` 拼的那一份，
/// 也就是真正落进 user 消息里的那段）。
ApiResult peek_prompt(const llm::Request& req);

}  // namespace changji::http
