#pragma once
// GET /api/voices —— 服务端有哪些参考音色。
//
// **现在两条配音后端都没有服务端清单**，这个接口的全部工作就是把这件事
// 说清楚。以前走 ComfyUI 时它是个下拉框（装了哪些插件就有哪些选项），
// ComfyUI 拆掉之后剩下的两条是：
//
//   local —— 进程内跑。音色是用户自己给的一段参考音频，没有清单。
//   http  —— 外部服务。它有没有音色列表是那个服务自己的事，我们不代问。
//
// **接口保留而不是删掉**：角色页打开时会拉它，删了前端要跟着改，
// 而它现在恰好承担了"告诉用户音色怎么配"这件事——那句说明比一个
// 空下拉框有用。
#include <string>

#include "http/readonly.hpp"

namespace changji::http {

/// 列音色。
///
/// **任何一步失败都回 200 加一个 error 字段**，不回错误码：这个接口是
/// 角色页打开时顺带拉的，回 500 的话整个页面会弹错误框，
/// 而用户可能根本没打算配音。
///
/// `backend` 是 `[tts].backend` 的值，用来决定那句说明怎么写——
/// 说错了比不说更糟：让用 local 的人去查一个他根本没在用的服务。
ApiResult get_voices(const std::string& path, const std::string& backend);

}  // namespace changji::http
