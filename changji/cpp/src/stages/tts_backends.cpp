#include "stages/tts_backends.hpp"

#include <array>
#include <cstdio>
#include <fstream>

#include "infer/llama_tts.hpp"
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

/// 把第一个匹配的**字符串型**输入换成 value。换了返回 true。
///
/// 只认字符串型是关键：同名的输入也可能是连线（["3", 0]）或者数字，
/// 往那上面写字符串会让工作流提交时被服务端拒绝。
template <std::size_t N>
bool set_first(comfy::OrderedJson& inputs,
               const std::array<const char*, N>& keys,
               const std::string& value) {
    for (const char* key : keys) {
        const auto it = inputs.find(key);
        if (it != inputs.end() && it->is_string()) {
            *it = value;
            return true;
        }
    }
    return false;
}

}  // namespace

bool apply_text(comfy::ApiWorkflow& wf, const std::string& text,
                const std::optional<std::string>& voice_id,
                const std::string& emotion) {
    bool filled = false;
    comfy::OrderedJson& prompt = wf.to_json();
    if (!prompt.is_object()) return false;

    for (auto& node : prompt) {
        if (!node.is_object()) continue;
        if (!node.contains("inputs") || !node["inputs"].is_object()) {
            node["inputs"] = comfy::OrderedJson::object();
        }
        comfy::OrderedJson& inputs = node["inputs"];

        if (set_first(inputs, kTextKeys, text)) filled = true;
        if (voice_id.has_value() && !voice_id->empty()) {
            set_first(inputs, kVoiceKeys, *voice_id);
        }
        // neutral 不填。填了的话有的引擎会按"中性"这个词去调语气，
        // 反而比不填更平。
        if (!emotion.empty() && emotion != "neutral") {
            set_first(inputs, kEmotionKeys, emotion);
        }
    }
    return filled;
}

void reject_silent_audio(const fs::path& path, double duration_s,
                         const std::string& text) {
    const double expected = estimate_speech_duration(text);
    // 两条下限**同时**不满足才拒。只用绝对下限会误杀"嗯。"这种一个字的
    // 台词；只用相对下限的话，估算本身偏得厉害时挡不住。
    if (duration_s >= kMinPlausibleDurationS && duration_s >= expected * 0.35) {
        return;
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
        "\nComfyUI 报的任务状态是成功，但节点内部很可能失败了。"
        "去看 ComfyUI 的日志，常见原因是缺少依赖或模型没下完。");
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
        res.duration_s = probe_audio_duration(out, ff);
        // HTTP 服务同样可能"成功但没出声"，一样要验产出物。
        reject_silent_audio(out, res.duration_s, text);
        res.audio_path = out;
        return res;
    };
    // 刻意不给 list_voices：独立服务的音色接口没有统一约定，
    // 猜一个路径去问，问不到会被当成"服务端一个音色都没有"，
    // 然后自动挑选挑出个空的。不给的话走"照填的来"，那是对的。
    return b;
}

TTSBackend comfy_tts_backend(std::shared_ptr<comfy::Client> client,
                             comfy::ApiWorkflow workflow,
                             models::ProjectPaths paths,
                             const std::optional<media::FFmpeg>& ff) {
    TTSBackend b;
    b.name = "comfy";

    b.list_voices = [client, workflow]() -> std::vector<std::string> {
        // 音色在节点里是个下拉框，装了哪些插件就有哪些选项。
        // 提交一条不在列表里的路径，节点会直接拒绝，整条流水线断在这。
        const auto& prompt = workflow.to_json();
        if (!prompt.is_object()) return {};
        for (const auto& kv : prompt.items()) {
            const auto cls = kv.value().find("class_type");
            if (cls == kv.value().end() || !cls->is_string()) continue;
            const auto inputs = kv.value().find("inputs");
            if (inputs == kv.value().end() || !inputs->is_object()) continue;
            for (const char* key : kVoiceKeys) {
                if (!inputs->contains(key)) continue;
                try {
                    return client->available_models(cls->get<std::string>(), key);
                } catch (const std::exception&) {
                    // 问不到就当没有列表。抛的话整个配音阶段起不来，
                    // 而音色列表只是个锦上添花的东西。
                    return {};
                }
            }
        }
        return {};
    };

    b.synthesize = [client, workflow, paths, ff](
                       const std::string& text, const fs::path& out,
                       const std::optional<std::string>& voice,
                       const std::string& emotion, double) {
        comfy::ApiWorkflow wf(workflow.to_json());   // 每句一份副本
        if (!apply_text(wf, text, voice, emotion)) {
            throw AudioError("配音工作流里找不到可以填文本的节点。"
                             "请确认 workflows/tts.json 里有一个文本输入节点");
        }
        try {
            wf.set_by_class("SaveAudio",
                            {{"filename_prefix",
                              "changji/" + paths::to_utf8(out.stem())}});
        } catch (const comfy::WorkflowError&) {
            // 有些工作流用别的保存节点，用默认前缀也能取到产出
        }

        comfy::JobResult result;
        pipeline::CancelToken tok;
        try {
            result = client->run(wf, nullptr, tok);
        } catch (const comfy::PromptValidationError& e) {
            throw AudioError("配音工作流被拒绝：\n" + e.human_summary());
        } catch (const comfy::ComfyError& e) {
            throw AudioError(std::string("配音失败：") + e.what());
        }

        // **产出可能落在三个键下的任意一个。** 不同的保存节点写法不同，
        // 只找 "audio" 的话换个节点包就一个文件都取不到，
        // 而那时候报的是"没有产出"，看不出是键名的问题。
        std::optional<comfy::OrderedJson> ref;
        for (const char* kind : {"audio", "audios", "images"}) {
            const auto files = result.files(kind);
            if (!files.empty()) {
                ref = files.front();
                break;
            }
        }
        if (!ref.has_value()) throw AudioError("配音完成但没有产出音频文件");

        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        client->download(*ref, out);

        SynthesisResult res;
        res.duration_s = probe_audio_duration(out, ff);
        // **只信状态码会被完全骗过。** 见文件头那段。
        reject_silent_audio(out, res.duration_s, text);
        res.audio_path = out;
        return res;
    };
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
