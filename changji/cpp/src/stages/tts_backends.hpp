#pragma once

// 两个真配音后端：独立 HTTP 服务、ComfyUI 的 TTS 节点。
//
// 移植自 src/changji/stages/audio.py 的 HttpTTSBackend 和 ComfyTTSBackend。
//
// ---
//
// **这里最重要的一条不是怎么调，是怎么发现"调成功了但没出声"。**
//
// ComfyUI 的 TTS 节点内部捕获异常之后，会输出一个一秒的空音频然后正常返回，
// 服务端的执行状态报的是 success。只信状态码的客户端会被完全骗过，
// 拿到一堆静音文件还以为配音成功了。实测撞上过：节点缺 librosa 报错，
// 任务状态却是成功。
//
// 所以不能只信状态，必须验证产出物本身——见 reject_silent_audio。

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "config/settings.hpp"
#include "llm/client.hpp"
#include "media/ffmpeg.hpp"
#include "models/project.hpp"
#include "stages/audio.hpp"

namespace changji::stages {


/// 一秒以内的音频几乎不可能是一句正常台词，多半是节点失败后的占位输出。
inline constexpr double kMinPlausibleDurationS = 1.05;

/// 检查产出的音频是不是真的有声音。不合格就抛。
///
/// 判据是**两条下限都过才放行**：绝对下限（1.05 秒）和相对下限
/// （按语速估算的 35%）。只用相对下限的话，估算本身偏得厉害时挡不住。
///
/// ⚠️ 副作用：一个字的台词（"嗯。"估约 0.94 秒）配出来不到 1.05 秒时会被
/// 误判成空音频。**照抄 Python，不在这里悄悄放宽**——放宽的代价是
/// 一整集静音文件被当成配音成功，而那比偶尔误杀一句严重得多。
/// 真撞上了要改的是那个常数本身，并且要有实测支撑。
void reject_silent_audio(const std::filesystem::path& path, double duration_s,
                         const std::string& text);

/// 读音频时长。**wav 自己读，别的格式退回 ffprobe。**
///
/// 大部分 TTS 出的就是 wav，那条路不该为一个时长起子进程；
/// 而 mp3/flac 这些又只能靠 ffprobe。没有 ffmpeg 时非 wav 直接抛，
/// 话里说清是缺 ffmpeg，不是文件坏了。
double probe_audio_duration(const std::filesystem::path& path,
                            const std::optional<media::FFmpeg>& ff);

/// 指向一个独立的 HTTP 配音服务。
///
/// 很多 TTS 项目自带 api 服务，比 ComfyUI 节点更省事，
/// 而且可以跑在另一台机器上。
TTSBackend http_tts_backend(const std::string& base_url, double timeout_s,
                            llm::HttpPost post,
                            const std::optional<media::FFmpeg>& ff);


/// 进程内配音（阶段 9）。**要 CHANGJI_LLAMA=ON 编出来的二进制。**
///
/// 和另外两个后端的区别在于**模型只载一次**：1.5 GB 的权重，
/// 每句台词重载一遍的话，一集几十句就是几十次载入。
/// 所以这里握着一个 shared_ptr，跟着 TTSBackend 的生命周期走。
///
/// 载不起来返回 nullopt——调用方按"这条路搭不起来"处理，退回估算后端，
/// 和另外两个后端连不上时是同一套逻辑。
///
/// ⚠️ 这条路径**从来没跑过**，见 infer/llama_tts.hpp 开头。
std::optional<TTSBackend> local_tts_backend(
    const std::filesystem::path& backbone, const std::filesystem::path& decoder,
    bool use_gpu, const std::optional<media::FFmpeg>& ff, std::string& why);

}  // namespace changji::stages
