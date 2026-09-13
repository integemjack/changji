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
#include <vector>

#include <nlohmann/json.hpp>

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

/// 一个预置音色：一个种子，外加那次实测的基频。
struct PresetVoice {
    unsigned int seed = 0;
    /// 实测基频（Hz）。**给界面当"摇之前的提示"用**，不是承诺——
    /// 真正显示的是摇完当场量的那个。
    ///
    /// **这个数只在配 kVoiceText 那一段文本时成立。** 音色是
    /// (种子, 文本) 的函数，换一段文本同一个种子就是另一个人
    /// （2026-09-13 实测，见 voices.cpp 里 kVoiceText 上面那段）。
    /// 所以改那段文本的话，这一列要跟着重量一遍。
    int hz = 0;
};

/// 预置音色用的那几个种子。
///
/// **"预置"在这一版是一组固定的种子，不是一组下载来的音频。** 我们这条
/// 运行时（llama.cpp 的 mtmd）只实现了 Qwen3-TTS 的 Base 模式，也就是
/// 参考音频克隆；自带 9 个说话人的 CustomVoice 和用文字描述造音色的
/// VoiceDesign 都不在里面（上游 PR #26254）。而不给参考音频时说话人是
/// 被采样出来的，**种子定死了，摇出来的人就定死了**——所以固定几个种子
/// 就等于固定几个音色，每个人在每台机器上拿到的是同一套。
///
/// 数字本身没有含义，别去解读；它们只需要**互不相同而且不再改动**。
/// 改了的话，已经存进项目的那些片段还在（存的是音频不是种子），
/// 但"预置 3"从此指向另一个人。
const std::vector<PresetVoice>& preset_voices();

/// POST /api/voice/take —— 摇一段音色试听。
///
/// body: `{project, seed?, text?}`。不给 seed 就随机一个并在回包里带上
/// ——用户要是喜欢这一摇，得能把它存下来。
ApiResult post_voice_take(const nlohmann::json& body);

/// POST /api/voice/save —— 把某个种子摇出来的音色存成参考音频。
///
/// body: `{project, seed, name, char_id?}`。存进 `voices/<名字>.wav`，
/// 给了 char_id 就顺手挂到那个角色的 voice_id 上。
///
/// **存的时候重出一段更长的**：试听那段只要几秒，而参考音频越长克隆
/// 越稳（社区实测 3 秒能认出来，8~15 秒明显更好）。种子一样，人就一样。
ApiResult post_voice_save(const nlohmann::json& body);

}  // namespace changji::http
