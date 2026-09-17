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
#include <vector>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"   // ApiResult
#include "llm/client.hpp"

namespace changji::http {

/// 请求体里带没带 `peek`。**取完就删掉**，免得下游的 forbid_extra 拦下来。
bool take_peek(nlohmann::json& body);

/// 请求体里带没带 `paste`：用户在别处跑完、粘回来的那段模型原文。
///
/// **取完就删掉**，同 take_peek。空串 = 没粘，照常去问模型。
///
/// 粘回来的东西走的是**同一条解析和守卫**——不是绕过检查的后门。整章没
/// 对白、正文复读、镜头没台词，在别处跑出来的一样会被打回，理由也一样。
std::string take_paste(nlohmann::json& body);

/// 把粘回来的那一大段按场次头切开。
///
/// 复制出去那份本来就是这个形状（见 StoryboardRunOptions::peek）：
///
///     ===== 第 1/3 场：夜 · 外 · 后门货场 =====
///     …第一场的提示词…
///     ===== 第 2/3 场：…… =====
///
/// 人在别处一场一场跑完，把结果按同样的头拼回来，这儿切开还原成一场一段。
/// **认不出任何一个头就整段当一段**——整集一次拆的那条路只有一段，
/// 那时候不该逼人去写一个分隔头。
std::vector<std::string> split_by_scene(const std::string& pasted);

/// 这一步的提示词，连 schema 一起（`llm::schema_as_prompt` 拼的那一份，
/// 也就是真正落进 user 消息里的那段）。
ApiResult peek_prompt(const llm::Request& req);

}  // namespace changji::http
