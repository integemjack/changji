#pragma once

// 跑一集：按阶段把镜头过一遍。
//
// **阶段之间是分批的，不是按镜头串行的。** 四十个镜头的首帧全出完，
// 再统一出视频。这不是风格问题——预算只装得下一个模型时，
// 按镜头串行是加载 80 次，按阶段分批是 2 次。
// test_scheduler 里有一条用例专门钉这个差距。

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "media/ffmpeg.hpp"
#include "stages/audio.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"

namespace changji::pipeline {

/// 流水线的阶段。**也是断点续跑的锚点**——每个阶段跑完就存盘，
/// 中途断了下次从没跑完的那个阶段接着来。
enum class Stage {
    Audio,      ///< 配音。**在生成任何画面之前跑完**——它决定镜头时长
    Frames,     ///< 逐镜首帧
    Draft,      ///< 草稿档视频
    Final,      ///< 成片档视频
    Assemble,   ///< 拼成一集
};

const char* to_string(Stage s);
/// 认不出返回 false。
bool stage_from_string(const std::string& s, Stage& out);

/// 跑一集的结果。
struct RunReport {
    std::string episode_id;
    std::vector<stages::ShotAudioPlan> audio;
    std::vector<stages::FrameOutcome> frames;
    std::vector<stages::RenderOutcome> draft;
    std::vector<stages::RenderOutcome> final_;
    /// 成片的路径。没跑装配阶段时是空的。
    std::optional<std::string> output;
    std::vector<std::string> errors;
    double elapsed_s = 0.0;

    bool ok() const { return errors.empty(); }
};

/// 跑一集要用的东西。
struct RunOptions {
    std::string episode_id;
    /// 只跑草稿档就停。快速验证叙事时用——成片档一个镜头几分钟，
    /// 而叙事对不对看草稿就够了。
    bool skip_final = false;
    /// 跳过草稿档，首帧出完直接上成片档。
    ///
    /// **两档画质拉不开差距的时候，草稿档就是白跑一遍。** 挂 Turbo LoRA
    /// 之后就是这个局面：草稿 6 步、成片 6 步，同一个分辨率，出来几乎一样，
    /// 而每镜多花两分钟。用户 2026-09-10 的原话："草稿档和成片档一样，
    /// 现在根本不需要草稿档了还浪费时间"。
    ///
    /// 跳过之后成片档要收 FRAME_DONE 那批镜头——它们没经过草稿档，
    /// 状态停在首帧完成。这一条在 render_entry_states 里。
    bool skip_draft = false;
    /// 无视状态，全部重跑。
    bool force = false;
    /// 只跑这几个阶段。
    ///
    /// **没给（nullopt）表示跑全流程，给了一个空的表示一个阶段都不跑。**
    /// 这两者不是一回事，而且都是前端能触发的：Python 那边判的是
    /// `if req.stages:`，所以 `stages: []` 走全流程（空列表是假值），
    /// `stages: ["  "]` 走"只跑指定阶段"、而指定的阶段过滤完是空的。
    /// 合成一个空 vector 的话，后一种会变成把整集重跑一遍。
    std::optional<std::vector<Stage>> only;
};

/// 出图和出片的后端。注入的，理由同各阶段：
/// 真跑一集要几十分钟，而阶段编排的逻辑（谁先谁后、哪些镜头要跑、
/// 什么时候存盘）能在几毫秒内测死。
struct Backends {
    stages::FrameRenderer frame;
    stages::VideoRenderer video;
    /// 配音。留空就用估算后端——只算时长不出声音，
    /// 成片是静音的，但整条流水线能跑通。
    std::optional<stages::TTSBackend> tts;
    /// 装配和闸门要用。留空就跳过装配阶段——没装 ffmpeg 的机器上
    /// 前面几步照样能跑，而"跑到最后一步才说缺 ffmpeg"最气人。
    std::optional<media::FFmpeg> ffmpeg;
    /// 出首帧的后端叫什么，只用来拼那句开始的话。
    ///
    /// Python 那边这句里带后端名是有用的：它有三条路
    /// （image_model / video_model / comfy），传了参考图但走的是视频模型
    /// 的话那些图一张都不会被用上，不写出来用户只会以为是模型不听话。
    /// 出图出片同时跑几镜。**1 = 逐镜串行，和以前一模一样。**
    ///
    /// 进程内那条路只能是 1：sd.cpp 的进度回调是全局的，同进程两个生成
    /// 会互相串。走工作进程池时取池的大小——那时候每个生成在各自的进程、
    /// 各自的卡上，互不干扰。
    int render_lanes = 1;

    std::string frame_backend_name = "sd.cpp";

    /// 只是让某些东西活到渲染结束。
    ///
    /// **`frame` 和 `video` 是 std::function，捕获的是裸指针**——
    /// 工作进程池那种"造出来交给它俩用"的东西，没有这个的话
    /// 出了 `backends()` 那个作用域就析构了，而 lambda 还留着指针。
    /// 表现是跑到第一镜就崩，且崩在池的析构里，看不出和配置有什么关系。
    std::vector<std::shared_ptr<void>> keepalive;
};

/// 跑一集。
///
/// 每个阶段结束后**立刻存盘**。不存的话中途断电或者点了停止，
/// 前面几十分钟的产出全部作废——文件还在磁盘上，但项目文件里没记，
/// 下次跑会当成没跑过。
RunReport run_episode(const models::ProjectStore& store,
                      const models::HardwareProfile& profile,
                      const config::Settings& settings, const RunOptions& opts,
                      const Backends& backends, JobProgress& progress,
                      CancelToken& tok);

/// 挑出这一阶段要跑的镜头。按 `order` 排，不按在数组里的位置。
///
/// **挑漏一个状态是最难查的一类错**：那一镜永远轮不到它——不报错、
/// 不重试、日志里一行都没有，表现是"成片里少了一个镜头"，
/// 而你会先怀疑分镜、怀疑渲染、怀疑装配。
/// 所以它和下面那个不留在匿名 namespace 里，好让语料够得着。
std::vector<models::Shot*> pick(models::Episode& ep,
                                const std::set<models::ShotStatus>& want,
                                bool force);

/// 一个档位的入口状态。
///
/// 草稿档收 `AUDIO_DONE` 是关键：首帧失败的镜头状态停在那里，
/// 不收的话它永远出不了片；收了就退回纯文生视频。
/// `skip_draft` 为真时成片档还要收 FRAME_DONE 那批——它们没走草稿档。
std::set<models::ShotStatus> render_entry_states(models::Tier tier,
                                                 bool skip_draft = false);

}  // namespace changji::pipeline
