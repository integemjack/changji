#pragma once

// 图生视频：拿首帧当起点，出三到五秒的片段。
//
// 和首帧那一层一样，出片的后端是注入的。这一层管的是**计划和状态**：
// 时长换多少帧、种子怎么来、出完之后镜头是什么状态。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "gates/checks.hpp"
#include "infer/sd_image.hpp"
#include "models/character.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"
#include "stages/limits.hpp"
#include "stages/prompt_compose.hpp"

namespace changji::stages {

/// 时长换帧数。
///
/// Wan 要求帧数满足 4n+1。超过上限的时长会被截断——调用方应该先用
/// max_shot_duration_s 把时长限住，走到这里才截断的话，
/// 成片会比计划短一截而且没有任何提示。
int frames_for(double duration_s, int fps = 24);

/// 视频的种子。
///
/// **和首帧那个是不同的偏移**（7919 vs 6271），刻意的：两者用同一个偏移的话，
/// 首帧撞上一个坏种子时视频也会撞上同一个，重试也躲不开。
///
/// 和 Python 的差异同 frame_seed：那边用 `hash(shot_id)`，
/// 而 Python 的 str hash 按进程随机化。那个函数的文档字符串明写着
/// "整体又要可复现，所以不能用随机数"——意图和实现直接矛盾。
std::int64_t render_seed(const std::string& shot_id, int attempts);

/// 一个镜头的渲染计划。
struct RenderPlan {
    std::string shot_id;
    models::Tier tier = models::Tier::DRAFT;
    models::TierSpec spec;
    int frames = 0;
    PromptBundle prompts;
    std::string motion;

    /// 拼提示词要用的分隔符由它决定（动画线 ", "，写实线 "，"）。
    ///
    /// **放进计划里而不是让调用方传**，是因为原来就是传错的：
    /// 两个视频后端都写死了 REALISTIC，动画线的项目拼出来和 Python
    /// 差一个分隔符——而提示词是要逐字节对得上的。
    /// 后端那一层拿不到资产库，让它"记得传对"本身就是设计问题。
    models::StyleLine style_line = models::StyleLine::REALISTIC;

    double duration_s() const { return frames / 24.0; }
};

RenderPlan make_plan(const models::Shot& shot, const models::TierSpec& spec,
                     const PromptComposer& composer,
                     const std::string& aspect_ratio, int fps = 24);

/// 视频模型的正向提示词：画面描述加运动描述。
///
/// 拼在一起而不是只给运动描述，是因为视频模型也要知道画面里有什么——
/// 只给"镜头缓慢推近"的话它不知道推的是谁。
std::string video_positive(const RenderPlan& plan);

struct RenderOutcome {
    std::string shot_id;
    bool ok = false;
    std::string path;
    std::string error;
    double elapsed_s = 0.0;
};

/// 出一段视频，写到 dest（mp4）。
using VideoRenderer = std::function<void(
    const models::Shot&, const RenderPlan&,
    const std::optional<std::filesystem::path>& start_image,
    const std::filesystem::path& dest, pipeline::CancelToken&,
    const infer::StepCallback&)>;

/// 出片之后过闸门用的两个钩子。
///
/// **注入而不是直接调 `gates::gate_video`**，理由和渲染器一样：
/// 闸门要跑 ffmpeg，而"闸门没过怎么办"这段编排（重试几次、什么时候降级、
/// 降级之后状态是什么）全是纯逻辑，得能在不装 ffmpeg 的机器上测死。
/// 而它恰恰是最不能错的一段——判错了要么无限重跑，要么第一次失败
/// 就把镜头判死。
struct GateHooks {
    /// 过一遍闸门。**空的表示不过**，等价于 Python 的 `[gates] enabled = false`。
    std::function<gates::GateResult(const models::Shot&,
                                    const std::filesystem::path& video,
                                    const RenderPlan&)>
        check;
    /// 没过的时候决定下一步。`check` 非空时这个也必须非空。
    std::function<gates::Verdict(const gates::GateResult&, const models::Shot&)>
        decide;
    /// 一个镜头最多试几次。到了就降级——**不是停下来**，
    /// 无人值守时停下来等于整集废掉。
    int max_attempts = 3;
};

/// 渲染一批镜头。
///
/// `concurrency` 是同时在跑的镜头数。**1 就是逐镜串行**——单卡就该是 1，
/// 一张卡上并发只会让每个任务都更慢并且更容易爆显存（两个上下文同时占显存
/// 正是 6GB 卡上跑不动的原因）。多卡时取工作进程数，一进程一张卡。
///
/// 大于 1 时并行的只是**渲染**那一步：渲染器收的是 `const Shot&`，只读。
/// 写回 `video_path` / `status` / `attempts` 一律等全部收完之后
/// 在调用线程上按镜头原顺序做——存盘那份 project.json 仍然只有一个写者。
///
/// 成功的镜头按档位置成 DRAFT_DONE 或 FINAL_DONE——**这两个状态不能混**，
/// 草稿档的片子当成片发出去，用户会以为模型质量就这样。
///
/// `gate` 给了就每出一镜过一遍闸门，没过按 `decide` 的判定重试 / 退回 /
/// 降级，和 Python 的 `_render_one` 一样。**不给等于不过闸门**——
/// 那是这个参数加进来之前的行为。
std::vector<RenderOutcome> render_batch(
    std::vector<models::Shot*>& shots,
    const models::AssetLibrary& assets,
    const models::TierSpec& spec,
    const models::ProjectPaths& paths,
    const VideoRenderer& render,
    pipeline::JobProgress& progress,
    pipeline::CancelToken& tok,
    int fps = 24,
    int concurrency = 1,
    const GateHooks& gate = {},
    /// 每出完一镜调一次（写回之后）。见 pipeline::ShotCommit。
    /// **不给就是老行为**：整批跑完再统一写回。
    const pipeline::ShotCommit& commit = {});

}  // namespace changji::stages
