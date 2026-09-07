#pragma once

// 跑一集：按阶段把镜头过一遍。
//
// **阶段之间是分批的，不是按镜头串行的。** 四十个镜头的首帧全出完，
// 再统一出视频。这不是风格问题——预算只装得下一个模型时，
// 按镜头串行是加载 80 次，按阶段分批是 2 次。
// test_scheduler 里有一条用例专门钉这个差距。

#include <optional>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"

namespace changji::pipeline {

/// 流水线的阶段。**也是断点续跑的锚点**——每个阶段跑完就存盘，
/// 中途断了下次从没跑完的那个阶段接着来。
enum class Stage {
    Audio,      ///< 配音。阶段 7，现在还没有
    Frames,     ///< 逐镜首帧
    Draft,      ///< 草稿档视频
    Final,      ///< 成片档视频
    Assemble,   ///< 装配成一集。阶段 7
};

const char* to_string(Stage s);
/// 认不出返回 false。
bool stage_from_string(const std::string& s, Stage& out);

/// 跑一集的结果。
struct RunReport {
    std::string episode_id;
    std::vector<stages::FrameOutcome> frames;
    std::vector<stages::RenderOutcome> draft;
    std::vector<stages::RenderOutcome> final_;
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
    /// 出首帧的后端叫什么，只用来拼那句开始的话。
    ///
    /// Python 那边这句里带后端名是有用的：它有三条路
    /// （image_model / video_model / comfy），传了参考图但走的是视频模型
    /// 的话那些图一张都不会被用上，不写出来用户只会以为是模型不听话。
    std::string frame_backend_name = "sd.cpp";
};

/// 跑一集。
///
/// 每个阶段结束后**立刻存盘**。不存的话中途断电或者点了停止，
/// 前面几十分钟的产出全部作废——文件还在磁盘上，但项目文件里没记，
/// 下次跑会当成没跑过。
RunReport run_episode(const models::ProjectStore& store,
                      const models::HardwareProfile& profile,
                      const RunOptions& opts, const Backends& backends,
                      JobProgress& progress, CancelToken& tok);

}  // namespace changji::pipeline
