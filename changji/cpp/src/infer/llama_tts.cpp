#include "infer/llama_tts.hpp"

#ifdef CHANGJI_HAVE_LLAMA
#include "mtmd.h"
#endif

namespace changji::infer {

#ifdef CHANGJI_HAVE_LLAMA

bool llama_tts_available() { return true; }

LlamaTtsProbe probe_llama_tts() {
    LlamaTtsProbe out;

    // 两个纯函数，没有副作用，也不碰模型文件。
    // 叫得动就说明 mtmd 真在这个二进制里。
    const mtmd_context_params p = mtmd_context_params_default();
    const char* marker = mtmd_default_marker();

    if (marker == nullptr) {
        out.detail = "mtmd 链进来了，但 mtmd_default_marker() 返回空";
        return out;
    }
    out.ok = true;
    out.detail = std::string("mtmd 已链入，媒体标记 ") + marker + "，默认 " +
                 std::to_string(p.n_threads) + " 线程。" +
                 "Qwen3-TTS 的权重还没接（talker + tokenizer 两份 GGUF），"
                 "配音仍走 [tts].backend 指定的外部后端。";
    return out;
}

#else

bool llama_tts_available() { return false; }

LlamaTtsProbe probe_llama_tts() {
    LlamaTtsProbe out;
    out.ok = true;  // 没编进来不算故障，配音走外部后端
    out.detail = "没编进来（CHANGJI_LLAMA=OFF），配音走外部后端";
    return out;
}

#endif

}  // namespace changji::infer
