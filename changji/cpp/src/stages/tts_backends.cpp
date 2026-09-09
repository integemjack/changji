#include "stages/tts_backends.hpp"

#include <array>
#include <cstdio>
#include <fstream>

#include "infer/llama_tts.hpp"
#include "stages/audio.hpp"
#include "stages/audio_plan.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::stages {

namespace {

/// 常见 TTS 节点里放文本的键名。不同引擎叫法不同，逐个试。
constexpr std::array<const char*, 5> kTextKeys = {
    "text", "prompt", "input_text", "tts_text", "content"};
constexpr std::array<const char*, 6> kVoiceKeys = {
    "narrator_voice", "voice", "voice_id", "speaker", "reference_audio",
    "speaker_id"};
constexpr std::array<const char*, 4> kEmotionKeys = {"emotion", "emo", "style",
                                                     "instruct"};

std::string fmt(const char* spec, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

}  // namespace

void reject_silent_audio(const fs::path& path, double duration_s,
                         const std::string& text) {
    const double expected = estimate_speech_duration(text);
    // 两条下限**同时**不满足才拒。只用绝对下限会误杀"嗯。"这种一个字的
    // 台词；只用相对下限的话，估算本身偏得厉害时挡不住。
    if (duration_s >= kMinPlausibleDurationS && duration_s >= expected * 0.35) {
        return;
    }
    // **短过绝对下限但真有声音的，放行。** 5090 上整集跑通时被这条误杀过
    // 一句：「苏晚！」本地 TTS 出了 1.04 秒、五万字节的真声音，差 0.01 秒
    // 够不到 1.05。绝对下限挡的是 ComfyUI 节点失败时吐的那一秒占位音频，
    // 而那种是**全零**——看峰值就分开了：真话峰值有满幅的一成，占位是 0。
    // 相对下限照旧：估算 3 秒的台词只出 0.3 秒，有声音也不对。
    // （Python 只看时长；它的这道检查只挂在 Comfy 后端上，本地后端是
    // C++ 独有的，所以这里比 Python 宽是有意的。）
    if (duration_s > 0 && duration_s >= expected * 0.35) {
        const auto peak = wav_peak_ratio(path);
        if (peak.has_value() && *peak >= 0.02) return;
    }

    std::error_code ec;
    const auto size = fs::is_regular_file(path, ec) ? fs::file_size(path, ec) : 0;

    // 话要说到根因上：状态是成功的，所以用户第一反应不会是去看
    // ComfyUI 的日志。
    throw AudioError(
        "配音节点返回了一个疑似空音频：时长 " + fmt("%.2f", duration_s) +
        " 秒、" + std::to_string(size) + " 字节，而这句台词按语速估算应有 " +
        fmt("%.1f", expected) + " 秒。\n台词：" +
        text::truncate_utf8(text, 30) +
        "\n后端报的状态是成功，但产出物本身是空的。"
        "走 ComfyUI 的话去看它的日志，常见原因是缺少依赖或模型没下完。");
}

double probe_audio_duration(const fs::path& path,
                            const std::optional<media::FFmpeg>& ff) {
    std::string ext = paths::to_utf8(path.extension());
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".wav") {
        try {
            return probe_wav_duration(path);
        } catch (const AudioError&) {
            // 扩展名是 wav 但读不出来——可能引擎给的其实是别的格式。
            // 有 ffmpeg 就再试一次，没有就往下抛。
            if (!ff.has_value()) throw;
        }
    }
    if (!ff.has_value()) {
        throw AudioError("读不出音频时长：" + paths::to_utf8(path) +
                         "\n不是 wav 而且没有 ffmpeg。装上 ffmpeg，"
                         "或者在配置里填 assembly.ffmpeg_path");
    }
    try {
        return ff->probe(path).duration_s;
    } catch (const media::FFmpegError& e) {
        throw AudioError("读不出音频时长：" + paths::to_utf8(path) + "\n" +
                         e.what());
    }
}

TTSBackend http_tts_backend(const std::string& base_url, double timeout_s,
                            llm::HttpPost post,
                            const std::optional<media::FFmpeg>& ff) {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();

    TTSBackend b;
    b.name = "http";
    b.synthesize = [url, timeout_s, post, ff](
                       const std::string& text, const fs::path& out,
                       const std::optional<std::string>& voice,
                       const std::string& emotion, double intensity) {
        const nlohmann::json payload{
            {"text", text},
            {"voice_id", voice.has_value() ? nlohmann::json(*voice)
                                           : nlohmann::json(nullptr)},
            {"emotion", emotion},
            {"emotion_intensity", intensity},
        };
        const llm::HttpResponse r =
            post(url + "/tts", payload.dump(), {}, timeout_s);
        if (r.transport_error.has_value()) {
            throw AudioError("连不上配音服务（" + url + "）。\n" +
                             *r.transport_error);
        }
        if (r.status < 200 || r.status >= 300) {
            // 把服务端的话带上，截断到 300 字——TTS 服务出错时
            // 常常回一整页 traceback。
            throw AudioError("配音服务回了 " + std::to_string(r.status) + "：\n" +
                             r.body.substr(0, 300));
        }

        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f) throw AudioError("写不了音频文件：" + paths::to_utf8(out));
        f.write(r.body.data(), static_cast<std::streamsize>(r.body.size()));
        f.close();

        SynthesisResult res;
        // ⚠️ **这两行都和 Python 不一样，而且是故意的。**
        //
        // Python 的 HttpTTSBackend.synthesize 收尾是这么一行
        // （src/changji/stages/audio.py:161，2026-09-08 复核过还是这样）：
        //
        //     return SynthesisResult(duration_s=probe_wav_duration(out_path), ...)
        //
        // 也就是：**只认 wav**，而且**不验产出物**。
        //
        // 一，时长这里走 probe_audio_duration（wav 自己读，别的退 ffprobe）。
        //     Python 用的是 probe_wav_duration，服务端回 mp3 就直接抛
        //     "音频文件读不出时长"。而独立 TTS 服务回 mp3 很常见——
        //     Python 那边它自己的 Comfy 后端用的正是 probe_audio_duration，
        //     两个后端不一致更像是漏了，不是有意的。
        //
        // 二，**Python 的 HTTP 后端不做静音检查**，只有 Comfy 后端做。
        //     而"成功但没出声"和后端是谁没有关系：独立服务同样会在
        //     模型没载好时回一个合法的空 wav。漏掉的代价是一整集静音
        //     被当成配音成功，混音、字幕、时长锁全部照跑，
        //     等人听出来的时候前面几步都得重来。
        //
        // 两条都是**往严的方向偏**，不会把 Python 能过的正常输入判失败
        // （mp3 那条是放宽，静音那条挡的是本来就该挡的）。
        // 删掉 Python 之后没人记得这里差过，所以写在这儿。
        res.duration_s = probe_audio_duration(out, ff);
        reject_silent_audio(out, res.duration_s, text);
        res.audio_path = out;
        return res;
    };
    // 刻意不给 list_voices：独立服务的音色接口没有统一约定，
    // 猜一个路径去问，问不到会被当成"服务端一个音色都没有"，
    // 然后自动挑选挑出个空的。不给的话走"照填的来"，那是对的。
    return b;
}

std::optional<TTSBackend> local_tts_backend(const fs::path& backbone,
                                            const fs::path& decoder,
                                            bool use_gpu,
                                            const std::optional<media::FFmpeg>& ff,
                                            std::string& why) {
    if (!infer::llama_tts_available()) {
        why = "这个二进制没编进程内配音（构建时 CHANGJI_LLAMA=OFF）";
        return std::nullopt;
    }
    if (backbone.empty() || decoder.empty()) {
        // 两个都要。只填一个是最常见的配错法，所以要分别点名，
        // 不能笼统说一句"模型没配"。
        why = std::string("进程内配音要两份模型：") +
              (backbone.empty() ? "[models].tts 没填" : "[models].tts 已填") +
              "，" +
              (decoder.empty() ? "[models].tts_decoder 没填"
                               : "[models].tts_decoder 已填");
        return std::nullopt;
    }

    // **模型只载一次，跟着后端的生命周期走。**
    // 1.5 GB 的权重，每句台词重载一遍的话一集就是几十次。
    auto engine = std::shared_ptr<infer::LlamaTts>(
        infer::LlamaTts::load(backbone, decoder, use_gpu, why));
    if (!engine) return std::nullopt;

    TTSBackend b;
    b.name = "local";
    b.synthesize = [engine, ff](const std::string& text, const fs::path& out,
                                const std::optional<std::string>& voice,
                                const std::string& emotion, double intensity) {
        // 情绪和强度这一版用不上：Qwen3-TTS 的情绪是靠参考音色带的，
        // 没有独立的情绪参数。**不静默吞掉**——调用方以为设了情绪而实际没有，
        // 比明说不支持更糟。留在这儿等接参考音色时一起做。
        (void)emotion;
        (void)intensity;

        infer::LlamaTtsRequest req;
        req.text = text;
        req.out = out;
        // voice 在这一版当参考音色的文件路径用。资产库里角色的
        // voice_id 填的就是一个音频文件时才有意义，填别的会读不了，
        // 那时候报错说的是"参考音色读不了"，指向明确。
        if (voice.has_value() && !voice->empty()) {
            req.speaker_ref = paths::from_utf8(*voice);
        }

        double duration = 0;
        std::string why;
        if (!engine->synthesize(req, duration, why)) {
            throw AudioError("进程内配音失败：" + why);
        }

        SynthesisResult res;
        // **时长用合成器自己报的，不再读一遍文件。**
        // 它是按采样数算的，比解析 wav 头更直接；而且 ffprobe 那条路
        // 在没装 ffmpeg 的机器上会抛，进程内配音本来就不该依赖 ffmpeg。
        res.duration_s = duration;
        if (res.duration_s <= 0) res.duration_s = probe_audio_duration(out, ff);
        // 进程内也可能"成功但没出声"：模型第一帧就停、或者权重不匹配时
        // 出一段极短的噪音。判据和另外两个后端共用。
        reject_silent_audio(out, res.duration_s, text);
        res.audio_path = out;
        return res;
    };
    // 音色列表问不到：参考音色是用户自己给的音频文件，没有服务端清单。
    // 留空，AudioStage 会按角色资产里填的来（见 audio.hpp 的 VoiceLister）。
    return b;
}

}  // namespace changji::stages
