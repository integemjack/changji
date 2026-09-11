#include "http/tts_api.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include "config/runtime.hpp"
#include "llm/client.hpp"
#include "media/ffmpeg.hpp"
#include "models/project.hpp"
#include "stages/audio.hpp"
#include "stages/tts_backends.hpp"
#include "pipeline/activity.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

std::string need_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        throw ApiError(400, std::string("请求里缺少字符串字段 ") + key);
    }
    return body.at(key).get<std::string>();
}

std::string opt_str(const json& body, const char* key) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

/// 按 `[tts].backend` 搭一个后端。和出片那一步同一套选法。
///
/// 搭不起来就退回估算后端——那会写出一段等长静音。在出片那条路上这是
/// 刻意的（画面那几步照样能验），但在"念给我听"这件事上没有意义，
/// 所以响应里把后端名字回出去，界面照实说。
stages::TTSBackend pick_backend(const config::Settings& s,
                                const std::optional<media::FFmpeg>& ff) {
    if (s.tts.backend == "http" && s.tts.base_url.has_value() &&
        !s.tts.base_url->empty()) {
        return stages::http_tts_backend(*s.tts.base_url, 300.0,
                                        llm::default_http_post(), ff);
    }
    if (s.tts.backend == "local") {
        std::string why;
        const auto ws = s.workspace_path();
        auto local = stages::local_tts_backend(s.models.resolve(s.models.tts, ws),
                                               s.models.resolve(s.models.tts_decoder, ws),
                                               /*use_gpu=*/true, ff, why);
        if (local.has_value()) return std::move(*local);
    }
    return stages::estimate_backend();
}

}  // namespace

ApiResult post_tts_say(const json& body) {
    const std::string project_path = need_str(body, "project");
    if (project_path.empty()) throw ApiError(400, "没有指定项目目录");
    ProjectStore store(paths::from_utf8(project_path));

    const std::string raw = text::strip_ws(need_str(body, "text"));
    if (raw.empty()) throw ApiError(400, "没有要念的字");

    // **超了就念前面那一段，并且明说。** 静默截断的话，用户以为听完了整段，
    // 而实际后半截根本没念。
    const std::string said = text::truncate_utf8(raw, kSayMaxChars);
    const bool truncated = said.size() != raw.size();

    const config::Settings s = config::runtime().snapshot();
    const auto ff = media::FFmpeg(s.assembly.ffmpeg_path, s.assembly.ffprobe_path,
                                  media::default_runner());
    const stages::TTSBackend backend = pick_backend(s, ff);
    if (!backend.synthesize) throw ApiError(503, "配音后端没准备好");

    // 落点：项目的 audio/ 下面一个固定名字。**固定名字是刻意的**——朗读是
    // 随手点的，一天点几十次；按内容起名的话 audio/ 里会堆满再也用不上的
    // wav，而它们和镜头配音混在同一个目录里，看着像是出了一堆废文件。
    std::error_code ec;
    const fs::path dir = store.paths().audio();
    fs::create_directories(dir, ec);
    const fs::path out = dir / "say.wav";

    // 念一段几秒钟，但它要借配音那一槽——而那一槽和大模型抢同一张卡。
    // 顶栏那本账上要看得见，否则别的活被它挡住时没人知道是谁挡的。
    pipeline::Activity act{"say", paths::to_utf8(store.root()), "", "正在朗读"};

    stages::SynthesisResult res;
    try {
        const std::string voice = opt_str(body, "voice");
        res = backend.synthesize(
            said, out, voice.empty() ? std::nullopt : std::optional(voice), "",
            1.0);
    } catch (const std::exception& e) {
        throw ApiError(502, std::string("念不出来：") + e.what());
    }

    return {200, {
        {"rel", store.paths().rel(out)},
        {"seconds", res.duration_s},
        {"chars", static_cast<int>(text::utf8_len(said))},
        {"truncated", truncated},
        // estimate 那条出来的是静音。界面照实说，别让人对着一段没声音的
        // 音频以为是自己音箱坏了。
        {"backend", backend.name},
    }};
}

}  // namespace changji::http
