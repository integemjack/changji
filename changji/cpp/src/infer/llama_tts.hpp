#pragma once

// 进程内配音（阶段 9）的门面。
//
// Qwen3-TTS 在 2026-08-04 进了 llama.cpp 主线（PR #26254，走 mtmd），
// 所以配音的 C++ 化不再需要自己写 codec——但**实现在 mtmd 里，不在
// libllama 里**（`tools/mtmd/models/qwen3tts-gen.cpp`、`qwen3tts-spkenc.cpp`）。
// mtmd 挂在 `tools/` 下面，上游留了 `LLAMA_BUILD_MTMD` 这个钩子可以单独编，
// 不用把整个 tools/ 和 common 拖进来。
//
// ---
//
// **这一层现在只回答"编进来了没有"。** 真正合成还没接，理由写在这里
// 免得下一个人以为是漏了：
//
// mtmd 那套音频生成的 API 在头文件里明写着
// `EXPERIMENTAL API for audio generation, subjected to breaking changes`，
// 而且**是无状态的**——`mtmd_gen_audio_process` 的注释说
// "caller must handle state management and audio frame accumulation"。
// 也就是说这不是"调一个函数出一段音频"，是要自己驱动骨干模型逐 token 跑、
// 把隐状态喂进 GEN_CODE、攒够码本再走 GEN_WAV。那是几百行，
// 而且**在拿到权重之前一行都验不了**（talker 1.7B Q4_K_M 1.2 GB +
// tokenizer 255 MB）。
//
// 先把编译和链接这一段做扎实、能自检，比先写一堆跑不了的代码有用。
//
// 门面写法同 sd_backend.hpp：头文件不带 #ifdef、不 include mtmd。

#include <string>

namespace changji::infer {

/// mtmd（也就是进程内配音的实现）编进来了没有。编译期决定。
bool llama_tts_available();

struct LlamaTtsProbe {
    bool ok = false;
    /// 给人看的一句话。
    std::string detail;
};

/// 真的调一次 mtmd，确认它不只是"编过了"，而是**链进来且叫得动**。
///
/// 为什么要这一下：mtmd 是静态库，我们又还没有任何地方调它的函数，
/// 链接器完全可以把它整个丢掉，而构建日志里照样有一行
/// "Linking CXX static library mtmd.lib"。**"编出来了"不等于"在二进制里"。**
LlamaTtsProbe probe_llama_tts();

}  // namespace changji::infer
