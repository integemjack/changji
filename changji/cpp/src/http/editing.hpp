#pragma once

// 阶段 3 的编辑接口。
//
// 移植自 src/changji/web/server.py 里对应的 POST 路由。
//
// 这一层和只读层的区别：它会写盘，而且写之前要校验。校验不过必须
// **原样不动**——Python 那边是先 model_validate 出一个新对象、验过了
// 才逐字段赋回原对象，中途失败原对象一个字段都没被改。这个性质要保住，
// 否则一次失败的编辑会留下半改的镜头。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"  // ApiResult / ApiError / guard

namespace changji::http {

/// POST /api/shot —— 改一个镜头。
///
/// body: {project, episode_id, shot_id, patch: {...}}
///
/// 改了画面相关的字段就把状态退回未开工，否则下次运行会跳过它，
/// 用户会以为改动没生效。
ApiResult post_shot(const nlohmann::json& body);

}  // namespace changji::http
