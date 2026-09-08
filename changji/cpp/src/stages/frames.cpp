#include "stages/frames.hpp"

#include <chrono>
#include <cstdlib>

#include "infer/scheduler.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::stages {

using namespace changji::models;

namespace {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

std::int64_t frame_seed(const std::string& shot_id, int attempts) {
    // SHA-1 前 4 字节当种子基数。用摘要而不是 std::hash：
    // 后者的实现由标准库决定，换个编译器甚至换个版本就变——
    // 那和 Python 那边随机化的效果一样糟，只是变得慢一点。
    const std::string hex = text::sha1_hex("frame:" + shot_id);
    const std::uint32_t base =
        static_cast<std::uint32_t>(std::stoul(hex.substr(0, 8), nullptr, 16));
    // 对齐 Python 的 % (2**31)，保证落在正数区间
    const std::int64_t b = static_cast<std::int64_t>(base % 2147483648u);
    // 每次重试换一个种子。不换的话重试等于把同一张图再算一遍，
    // 而闸门拒它正是因为那张图不行。
    return (b + static_cast<std::int64_t>(attempts) * 6271) % 2147483648LL;
}

FrameRenderer sd_renderer() {
    return [](const Shot& shot, const PromptBundle& prompts,
              const TierSpec& spec, const fs::path& dest,
              pipeline::CancelToken& tok, const infer::StepCallback& on_step) {
        // 每次借一下。**不在外面借一次拿着不放**——那样跑首帧期间
        // 别的槽（比如视频模型）永远腾不出地方，
        // 而按阶段分批的整个意义就是让它们轮流占显存。
        auto lease = infer::scheduler().acquire(infer::Slot::Image);
        auto ctx = infer::current_image_context();
        if (!ctx) throw infer::SdError("出图上下文没准备好");

        infer::ImageRequest req;
        req.positive = prompts.positive;
        req.negative = prompts.negative;
        req.width = spec.width;
        req.height = spec.height;
        req.steps = spec.steps;
        req.seed = frame_seed(shot.shot_id, shot.attempts);
        for (const auto& r : prompts.reference_images) {
            req.reference_images.push_back(paths::from_utf8(r));
        }
        ctx->generate(req, dest, tok, on_step);
    };
}

std::vector<FrameOutcome> run_frames(std::vector<Shot*>& shots,
                                     const AssetLibrary& assets,
                                     const TierSpec& spec,
                                     const ProjectPaths& paths,
                                     const FrameRenderer& render,
                                     pipeline::JobProgress& progress,
                                     pipeline::CancelToken& tok) {
    const PromptComposer composer(assets);
    // 按画幅缩放。分辨率必须是 32 的倍数，否则潜空间对不齐。
    const TierSpec scaled = spec.scaled_to(assets.style.aspect_ratio);

    std::vector<FrameOutcome> outcomes;
    const int total = static_cast<int>(shots.size());
    int index = 0;

    for (Shot* shot : shots) {
        ++index;
        if (tok.cancelled()) break;

        const double started = now_seconds();
        FrameOutcome out;
        out.shot_id = shot->shot_id;

        {
            pipeline::Event e;
            e.stage = "frames";
            e.kind = "progress";
            e.current = index;
            e.total = total;
            e.shot_id = shot->shot_id;
            e.message = "出首帧 " + shot->shot_id;
            progress.report(e);
        }

        try {
            const PromptBundle prompts = composer.compose(*shot);
            const fs::path dest =
                paths.frames() / paths::from_utf8(shot->shot_id + ".png");

            // 逐步进度。采样一步在低配机器上要好几秒，不报的话界面上
            // 就是一条几分钟不动的进度条，用户分不清是在跑还是卡死了。
            const auto on_step = [&](int step, int steps, double,
                                     bool loading) {
                pipeline::Event e;
                e.stage = "frames";
                e.kind = "progress";
                e.current = index;
                e.total = total;
                e.shot_id = shot->shot_id;
                e.message =
                    loading ? "加载出图模型 " + std::to_string(step) + "/" +
                                  std::to_string(steps)
                            : "出首帧 " + shot->shot_id + "（第 " +
                                  std::to_string(step) + "/" +
                                  std::to_string(steps) + " 步）";
                progress.report(e);
            };

            render(*shot, prompts, scaled, dest, tok, on_step);

            shot->frame_path = paths.rel(dest);
            shot->status = ShotStatus::FRAME_DONE;
            out.ok = true;
            out.path = *shot->frame_path;
        } catch (const std::exception& e) {
            // 一镜失败不拖垮后面几镜。跑一晚上，早上发现第三镜挂了
            // 导致后面三十镜都没动，那这一晚上就白熬了。
            //
            // attempts 加一是给闸门的重试计数用的：超限之后流水线会
            // 降级成静帧加运镜，保证整集能出片。
            shot->attempts += 1;
            out.ok = false;
            out.error = e.what();

            pipeline::Event ev;
            ev.stage = "frames";
            ev.kind = "warn";
            ev.shot_id = shot->shot_id;
            ev.message = shot->shot_id + " 出首帧失败：" + out.error;
            progress.report(ev);
        }

        out.elapsed_s = now_seconds() - started;
        outcomes.push_back(out);
    }
    return outcomes;
}

}  // namespace changji::stages
