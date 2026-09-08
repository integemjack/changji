#pragma once

// GET /api/voices —— 服务端有哪些参考音色。
//
// 音色在 ComfyUI 那边是个**下拉框**，装了哪些插件就有哪些选项。
// 界面上得让人从这个列表里选，而不是手打一条路径然后在跑到配音那一步
// 才发现填错了——那时候前面的分镜和首帧已经跑完了。
//
// 移植自 web/server.py 的 voices()。阶段 6 的完成判据之一。

#include <optional>
#include <string>
#include <utility>

#include "comfy/client.hpp"
#include "comfy/workflow.hpp"
#include "http/readonly.hpp"

namespace changji::http {

/// 在工作流里找音色那个下拉框：返回 (节点类型, 输入名)。找不到返回空。
///
/// **靠输入名认，不靠节点类型认。** TTS 那一片的自定义节点包换得很勤，
/// 类型名各不相同，而输入名反而稳定（voice、speaker、reference_audio 那几个）。
/// 按类型名列白名单的话，用户换一个节点包音色列表就空了。
std::optional<std::pair<std::string, std::string>> find_voice_input(
    const comfy::ApiWorkflow& w);

/// GET /api/voices。
///
/// **任何一步失败都回 200 加一个 error 字段**，不回错误码：这个接口是
/// 角色页打开时顺带拉的，回 500 的话整个页面会弹错误框，
/// 而用户可能根本没打算配音。
/// backend 是 `[tts].backend` 的值。
///
/// **进程内配音（local）根本不问 ComfyUI 要音色。** 它的"音色"是用户自己
/// 给的一段参考音频，服务端没有清单可列。不区分的话，选了 local 的用户
/// 会在角色页看到一个 ComfyUI 的音色下拉框（或者一句"连不上 ComfyUI"），
/// 而那个后端压根没在用——**答非所问比答不出来更糟**。
ApiResult get_voices(const std::string& path, comfy::Client& client,
                     const std::string& backend = "comfy");

}  // namespace changji::http
