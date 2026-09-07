#include "stages/render.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "util/human_time.hpp"
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

/// Python 的 round()：银行家舍入。
long py_round(double v) { return static_cast<long>(std::nearbyint(v)); }

}  // namespace

int frames_for(double duration_s, int fps) {
    const long raw = py_round(duration_s * fps);
    // 4n+1 是 Wan 的硬要求。给别的数它会自己截，而截的位置不告诉你。
    const long n = std::max(1L, py_round(static_cast<double>(raw - 1) / 4.0));
    const long frames = 4 * n + 1;
    return static_cast<int>(std::min<long>(frames, kMaxFrames));
}

std::int64_t render_seed(const std::string& shot_id, int attempts) {
    // 注意这里**没有前缀**，对齐 Python 的 hash(shot.shot_id)。
    // 首帧那个是 hash("frame:" + shot_id)——两者不同正是为了让
    // 同一个镜头的首帧和视频落在不同的种子上。
    const std::string hex = text::sha1_hex(shot_id);
    const std::uint32_t base =
        static_cast<std::uint32_t>(std::stoul(hex.substr(0, 8), nullptr, 16));
    const std::int64_t b = static_cast<std::int64_t>(base % 2147483648u);
    return (b + static_cast<std::int64_t>(attempts) * 7919) % 2147483648LL;
}

RenderPlan make_plan(const Shot& shot, const TierSpec& spec,
                     const PromptComposer& composer,
                     const std::string& aspect_ratio, int fps) {
    RenderPlan p;
    p.shot_id = shot.shot_id;
    p.tier = spec.tier;
    p.spec = spec.scaled_to(aspect_ratio);
    p.frames = frames_for(shot.duration_s, fps);
    p.prompts = composer.compose(shot);
    p.motion = composer.motion_prompt(shot);
    return p;
}

std::string video_positive(const RenderPlan& plan, StyleLine style_line) {
    const std::string sep = style_line == StyleLine::ANIME ? ", " : "，";
    if (plan.motion.empty()) return plan.prompts.positive;
    if (plan.prompts.positive.empty()) return plan.motion;
    return plan.prompts.positive + sep + plan.motion;
}

std::vector<RenderOutcome> render_batch(std::vector<Shot*>& shots,
                                        const AssetLibrary& assets,
                                        const TierSpec& spec,
                                        const ProjectPaths& paths,
                                        const VideoRenderer& render,
                                        pipeline::JobProgress& progress,
                                        pipeline::CancelToken& tok, int fps) {
    const PromptComposer composer(assets);
    const std::string stage_name =
        spec.tier == Tier::FINAL ? "final" : "draft";

    std::vector<RenderOutcome> outcomes;
    const int total = static_cast<int>(shots.size());
    int index = 0;

    // 开跑前的预计来自一张按显存推的静态表，实测能差一倍。
    // 跑起来之后用真实耗时重算，等的人才知道还要等多久。
    const double stage_started = now_seconds();

    for (Shot* shot : shots) {
        ++index;
        if (tok.cancelled()) break;

        const double started = now_seconds();
        RenderOutcome out;
        out.shot_id = shot->shot_id;

        {
            pipeline::Event e;
            e.stage = stage_name;
            e.kind = "progress";
            e.current = index;
            e.total = total;
            e.shot_id = shot->shot_id;
            e.message = "出视频 " + shot->shot_id;
            progress.report(e);
        }

        try {
            const RenderPlan plan =
                make_plan(*shot, spec, composer, assets.style.aspect_ratio, fps);

            // 首帧是这一镜的起点。没有的话退回纯文生视频——
            // 那样跨镜头一致性会掉一大截，但总比整条流水线卡住强。
            std::optional<fs::path> start;
            if (shot->frame_path.has_value() && !shot->frame_path->empty()) {
                const fs::path frame = paths.abs(*shot->frame_path);
                std::error_code ec;
                if (fs::is_regular_file(frame, ec)) {
                    start = frame;
                } else {
                    pipeline::Event e;
                    e.stage = stage_name;
                    e.kind = "warn";
                    e.shot_id = shot->shot_id;
                    e.message = shot->shot_id +
                                " 记着首帧但文件不在，这一镜退回纯文生视频";
                    progress.report(e);
                }
            }

            // 按档位分目录。草稿和成片混在一起的话，重跑成片时
            // 分不清哪个 mp4 是哪一档的，而它们文件名只差一个后缀。
            const fs::path dest = paths.shots(stage_name) /
                                  paths::from_utf8(shot->shot_id + ".mp4");

            const auto on_step = [&](int step, int steps, double) {
                pipeline::Event e;
                e.stage = stage_name;
                e.kind = "progress";
                e.current = index;
                e.total = total;
                e.shot_id = shot->shot_id;
                e.message = "出视频 " + shot->shot_id + "（第 " +
                            std::to_string(step) + "/" +
                            std::to_string(steps) + " 步）";
                progress.report(e);
            };

            render(*shot, plan, start, dest, tok, on_step);

            shot->video_path = paths.rel(dest);
            // **两个状态不能混。** 草稿档的片子当成片发出去，
            // 用户会以为模型质量就这样。
            shot->status = spec.tier == Tier::FINAL ? ShotStatus::FINAL_DONE
                                                    : ShotStatus::DRAFT_DONE;
            out.ok = true;
            out.path = *shot->video_path;
        } catch (const std::exception& e) {
            shot->attempts += 1;
            out.ok = false;
            out.error = e.what();

            pipeline::Event ev;
            ev.stage = stage_name;
            ev.kind = "warn";
            ev.shot_id = shot->shot_id;
            ev.message = shot->shot_id + " 出视频失败：" + out.error;
            progress.report(ev);
        }

        out.elapsed_s = now_seconds() - started;
        outcomes.push_back(out);

        // 剩余时间按**已经跑过的这几镜**的平均值推，不按静态表。
        // 失败的那几镜也算进去：它们也花了时间（而且往往花得更多，
        // 失败通常发生在跑完大半之后）。
        const int left = total - index;
        if (left > 0) {
            const double per = (now_seconds() - stage_started) / index;
            pipeline::Event e;
            e.stage = stage_name;
            e.kind = "eta";
            e.current = index;
            e.total = total;
            e.message = "还剩 " + std::to_string(left) + " 个镜头，按目前速度约 " +
                        util::human_time(per * left);
            progress.report(e);
        }
    }
    return outcomes;
}

}  // namespace changji::stages
