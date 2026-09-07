#pragma once

// 配音编排：跑 TTS 并反推锁定镜头时长。
//
// 移植自 src/changji/stages/audio.py 的 AudioStage 那一半。
// 估时长和拆分在 audio_plan.hpp。
//
// ---
//
// **后端是注入的函数对象**，和 frames / render 两个阶段一样。
// 一是分层，二是很实际：这一层真正难的是**重切循环**——
// 估算的语速和引擎实测差得很远，估 4.8 秒出来 5.8 秒是真实发生过的，
// 那 1 秒会盖到下一镜上去。这条逻辑要能在毫秒级反复跑才测得动。

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "models/character.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"
#include "stages/audio_plan.hpp"

namespace changji::stages {

class AudioError : public std::runtime_error {
public:
    explicit AudioError(const std::string& what) : std::runtime_error(what) {}
};

/// 一句台词的配音结果。
struct SynthesisResult {
    double duration_s = 0.0;
    /// 没出音频时是空的（只估时长的那种后端）。
    std::optional<std::filesystem::path> audio_path;

    bool is_real_audio() const { return audio_path.has_value(); }
};

/// 合成一句台词，写到 out_path。
using Synthesizer = std::function<SynthesisResult(
    const std::string& text, const std::filesystem::path& out_path,
    const std::optional<std::string>& voice_id, const std::string& emotion,
    double intensity)>;

/// 服务端有哪些参考音色。返回空表示问不到——**那时候不要自作主张**，
/// 照角色资产里填的来。
using VoiceLister = std::function<std::vector<std::string>()>;

/// 配音后端。
struct TTSBackend {
    std::string name = "estimate";
    Synthesizer synthesize;
    /// 可以留空：那表示问不到音色列表。
    VoiceLister list_voices;
};

/// 只算时长不出音频的后端。
///
/// 用途有两个：在没装 TTS 的机器上把整条流水线跑通，以及排产时估算总时长。
/// **产出的是等长静音 wav**，所以后面的装配环节不用为它写特例。
TTSBackend estimate_backend(int sample_rate = 24000);

/// 写一段静音 wav。
void write_silence(const std::filesystem::path& path, double seconds,
                   int sample_rate);

/// 读 wav 的时长。
///
/// **不走 ffprobe**：配音每句都要读一次，为它多起一个子进程不划算，
/// 而且这样在没装 ffmpeg 的机器上配音那条路照样能跑。
double probe_wav_duration(const std::filesystem::path& path);

/// 跑配音并锁定镜头时长。
class AudioStage {
public:
    AudioStage(TTSBackend backend, config::TTSConfig config,
               models::ProjectPaths paths);

    /// 给这些镜头配音，然后反推锁定时长。
    ///
    /// **顺序跑，不并发。** Python 那边有个 concurrency 参数，但配音服务
    /// 通常就一张卡，并发只会让每句都变慢；而且并发时 shot.dialogue
    /// 被就地改，两条线程改同一个镜头的话结果是乱的。
    std::vector<ShotAudioPlan> run(std::vector<models::Shot*>& shots,
                                   const models::AssetLibrary& assets,
                                   pipeline::JobProgress& progress,
                                   pipeline::CancelToken& tok);

    /// 处理一个镜头。公开是为了能单独测重切循环。
    ShotAudioPlan process_shot(models::Shot& shot,
                               const models::AssetLibrary& assets,
                               pipeline::CancelToken& tok);

    /// 服务端不认的音色。**收集起来报给用户**——
    /// 不然自动换了声音他不知道，只会觉得"这个角色听着不像上次那个"。
    const std::set<std::string>& unknown_voices() const { return unknown_; }

private:
    /// 过长的台词先按字数估一遍切开。
    void split_long_lines(models::Shot& shot) const;
    std::optional<std::string> voice_for(const models::DialogueLine& line,
                                         const models::AssetLibrary& assets);
    const std::vector<std::string>& available_voices();
    /// 由配音时长反推镜头时长。
    double lock_duration(models::Shot& shot, double speech_s) const;

    TTSBackend backend_;
    config::TTSConfig config_;
    models::ProjectPaths paths_;
    std::optional<std::vector<std::string>> voices_;
    std::set<std::string> unknown_;
};

/// 一句台词最多重切几次。
///
/// 估算不准是常态，但切三次还装不下的话多半是引擎那边出了别的问题，
/// 再切下去只是把话剁碎。
inline constexpr int kMaxResplits = 3;

}  // namespace changji::stages
