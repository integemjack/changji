#include "stages/frames.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <optional>

#include <chrono>
#include <cstdlib>

#include "infer/local_exec.hpp"
#include "infer/scheduler.hpp"
#include "pipeline/activity.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::stages {
namespace {
/// 把腾显存的结论包成"（……）"挂在进度文案后面；没有结论就什么都不加。
std::string room_note_suffix(infer::Slot slot) {
    const std::string note = infer::scheduler().room_note(slot);
    return note.empty() ? std::string{} : "（" + note + "）";
}
}  // namespace

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

namespace {

/// 出图那一段的公共实现。`seed_override` 有值就用它，没有就按镜头算。
// 合并时两边各加了一个参数，都要：`settings` 是采样旋钮那一份（见下面
// knobs），`origin` 是"这活谁派的"（见下面 local_exec().enter）。
FrameRenderer make_sd_renderer(const config::Settings& settings,
                               std::optional<std::int64_t> seed_override,
                               infer::Origin origin) {
    const infer::SamplingKnobs knobs =
        infer::sampling_knobs_for(settings, infer::ModelRole::Image);
    return [seed_override, origin, knobs](const Shot& shot,
              const PromptBundle& prompts,
              const TierSpec& spec, const fs::path& dest,
              pipeline::CancelToken& tok, const infer::StepCallback& on_step) {
        // **先拿本机的执行位，再向调度器借显存槽。** 反过来的话会撞上
        // 「出片在显存那儿干等五分钟」——理由写在 local_exec.hpp 里。
        //
        // origin 现在一律是 Local：派给别的机器算的活走的是工作进程池
        // 那条路，根本不经过这儿。接上对等互联之后，外来的活在工作进程
        // 那一侧标 Peer，本机自己的一集就排在它前面。
        auto hold = infer::local_exec().enter(origin, pipeline::note_queued,
                                              &tok);
        // 每次借一下。**不在外面借一次拿着不放**——那样跑首帧期间
        // 别的槽（比如视频模型）永远腾不出地方，
        // 而按阶段分批的整个意义就是让它们轮流占显存。
        // **借之前先说一句。** acquire 是阻塞的：它可能要先把大模型卸掉
        // 腾地方，再从磁盘把出图模型读进来（16~20 GB，几十秒起）。
        // 这一整段里 sd.cpp 还没开始跑，它那个进度回调一次都不会触发——
        // 界面上就是点了出片之后一动不动。用户看到的是"卡死了"，
        // 而实际上正是他要的"点击出片清理掉大模型"在发生。
        //
        // steps 传 0 表示"还没进入采样，没有步数可报"，文案由上层按这个分支写。
        if (!infer::scheduler().loaded(infer::Slot::Image)) {
            on_step(0, 0, 0.0, /*loading=*/true);
        }
        // **借之前先说这一镜多大。** 以前量到的显存只在"量过的活不小于
        // 这次要干的活"时才算数——在 720p 量到的数不能拿去给 2K 背书。
        // 见 Scheduler::record_measured_vram。
        // 借不到就排队等：这一镜等几十秒，比整镜报错强得多。
        infer::Scheduler::AcquireOptions opt;
        opt.work = static_cast<std::size_t>(spec.width) * spec.height;
        opt.wait = infer::kAcquireWait;
        opt.on_queued = pipeline::note_queued;
        auto lease = infer::scheduler().acquire(infer::Slot::Image, opt);
        auto ctx = infer::current_image_context();
        if (!ctx) throw infer::SdError("出图上下文没准备好");

        infer::ImageRequest req;
        req.positive = prompts.positive;
        req.negative = prompts.negative;
        req.width = spec.width;
        req.height = spec.height;
        req.steps = spec.steps;
        // 预览要挂到墙上哪一格，靠这个。见 ImageRequest::tag。
        req.tag = shot.shot_id;
        req.knobs = knobs;
        req.seed = seed_override ? *seed_override
                                 : frame_seed(shot.shot_id, shot.attempts);
        for (const auto& r : prompts.reference_images) {
            req.reference_images.push_back(paths::from_utf8(r));
        }
        ctx->generate(req, dest, tok, on_step);
    };
}

}  // namespace

FrameRenderer sd_renderer(const config::Settings& settings) {
    return make_sd_renderer(settings, std::nullopt, infer::Origin::Local);
}

FrameRenderer sd_renderer_with_seed(const config::Settings& settings,
                                    std::int64_t seed, infer::Origin origin) {
    return make_sd_renderer(settings, seed, origin);
}

std::vector<FrameOutcome> run_frames(std::vector<Shot*>& shots,
                                     const AssetLibrary& assets,
                                     const TierSpec& spec,
                                     const ProjectPaths& paths,
                                     const FrameRenderer& render,
                                     pipeline::JobProgress& progress,
                                     pipeline::CancelToken& tok,
                                     int concurrency,
                                     const pipeline::ShotCommit& commit) {
    const PromptComposer composer(assets);
    // 按画幅缩放。分辨率必须是 32 的倍数，否则潜空间对不齐。
    const TierSpec scaled = spec.scaled_to(assets.style.aspect_ratio);

    const int total = static_cast<int>(shots.size());

    /// 一镜的渲染结果。**只装数据，不碰 Shot**——
    /// 写回统一放到后面在调用线程上做。
    struct Done {
        bool ok = false;
        std::string error;
        std::string rel_path;
        /// 尾帧（`last_frame_prompt` 出的那张）的相对路径，没有就空。
        std::string end_rel;
        double elapsed_s = 0.0;
        bool skipped = false;     ///< 取消了，没跑
        bool committed = false;   ///< 已经写回 Shot 了，收尾时别再写一遍
    };
    std::vector<Done> done(shots.size());

    // 写回一镜。**收尾和逐镜落盘共用这一份**——两份的话，
    // `attempts += 1` 这种不幂等的操作迟早在一边漏掉或者做两遍，
    // 而做两遍的表现是"这一镜莫名其妙被判定重试超限"。
    const auto apply = [&](int i) {
        Shot* shot = shots[i];
        if (done[i].ok) {
            shot->frame_path = done[i].rel_path;
            shot->status = ShotStatus::FRAME_DONE;
            // 尾帧每次重出首帧都跟着重算：没填 last_frame_prompt 就清掉，
            // 别让一张旧的尾帧把新首帧拽回老构图。
            if (done[i].end_rel.empty()) {
                shot->end_frame_path.reset();
            } else {
                shot->end_frame_path = done[i].end_rel;
            }
        } else {
            // attempts 加一是给闸门的重试计数用的：超限之后流水线会
            // 留着最后那一版接着往下走，保证整集能出片。
            shot->attempts += 1;
        }
        done[i].committed = true;
    };
    std::mutex commit_mu;

    // 取多少并发。**上限是镜头数**——池里有八个而只有三镜时，
    // 起八个线程只是白占。
    const int lanes = std::max(1, std::min(concurrency, total));

    std::atomic<int> next{0};
    const auto worker = [&] {
        for (;;) {
            const int i = next.fetch_add(1);
            if (i >= total) return;
            if (tok.cancelled()) { done[i].skipped = true; continue; }

            Shot* shot = shots[i];
            const double started = now_seconds();
            const int index = i + 1;

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
                PromptBundle prompts = composer.compose(*shot);
                // **参考图的路径要还原成绝对的。**
                //
                // 资产库里存的是相对项目根的路径（`refs/c_xxx_front.png`），
                // 那是有意的——项目目录整个拷到别的机器上还能读。但底下
                // `load_image` 是直接拿它开文件的，于是解析到**引擎进程的
                // 当前目录**去了（服务器上是 /root），必然打不开。
                //
                // 2026-09-13 实测撞到：报的是「读不了参考图
                // refs/c_li_hao_ran_front.png」，而那个文件明明在项目里躺着。
                // 后果是**只要角色出过参考图，这一镜的首帧就必然失败**——
                // 而参考图恰恰是跨镜头一致性的全部依靠。之前没露出来，
                // 是因为那几个验过的项目都还没出过参考图。
                //
                // 消息里那个相对路径本身就是线索：这一层的报错一律该是
                // 绝对路径，看见相对的就说明哪儿漏了还原。
                for (std::string& r : prompts.reference_images) {
                    r = paths::to_utf8(paths.abs(r));
                }
                const fs::path dest =
                    paths.frames() / paths::from_utf8(shot->shot_id + ".png");

                // 逐步进度。采样一步在低配机器上要好几秒，不报的话界面上
                // 就是一条几分钟不动的进度条，用户分不清是在跑还是卡死了。
                //
                // **并发时几镜同时报**，靠 Event 里的 shot_id 分得开；
                // JobProgress::report 自己有锁。
                const auto on_step = [&](int step, int steps, double,
                                         bool loading) {
                    pipeline::Event e;
                    e.stage = "frames";
                    e.kind = "progress";
                    e.current = index;
                    e.total = total;
                    e.shot_id = shot->shot_id;
                    // 这一张自己的进度。current/total 是整集第几镜，
                    // 镜头墙上那条进度条要的是这个。见 Event::shot_steps。
                    e.shot_step = step;
                    e.shot_steps = steps;
                    e.shot_prep = loading;
                    e.message =
                        // 同 render.cpp：这一支不只是"加载模型"，
                        // 也可能是搬权重或 VAE 分块解码，分不开。
                        // steps == 0：还没开始采样，正在腾显存 / 装模型。
                        // 这一支没有步数可报，写"准备 0/0"只会让人以为出错了。
                        (loading && steps == 0)
                            ? "出首帧 " + shot->shot_id + "（正在准备模型，可能要先腾出显存）"
                        : loading ? "出首帧 " + shot->shot_id + "（准备 " +
                                      std::to_string(step) + "/" +
                                      std::to_string(steps) + "）"
                                : "出首帧 " + shot->shot_id + "（第 " +
                                      std::to_string(step) + "/" +
                                      std::to_string(steps) + " 步）" +
                                      // **第一步上把腾显存的结论带出来。**
                                      // 用户点完出片盯的是进度条，而"卸没卸
                                      // 大模型"的结论只在设置页上。挂在第一
                                      // 步是因为那时候刚借完槽，结论是新的；
                                      // 每一步都挂只是重复刷屏。
                                      //
                                      // 几镜并发时这条可能是同一个槽上兄弟镜
                                      // 头留下的判断——同一个槽、同一时刻的
                                      // 状态，内容一样，不会说错。
                                      (step == 1 ? room_note_suffix(
                                                       infer::Slot::Image)
                                                 : std::string{});
                    progress.report(e);
                };

                render(*shot, prompts, scaled, dest, tok, on_step);
                done[i].ok = true;
                done[i].rel_path = paths.rel(dest);

                // ---- 尾帧 ----
                //
                // 分镜填了 last_frame_prompt 的镜头再出一张，出片时当
                // end_image 走首尾帧。出不来不算这一镜失败：首帧在，
                // 退回单帧图生视频，说一声。
                if (shot->last_frame_prompt.has_value() &&
                    !text::strip_ws(*shot->last_frame_prompt).empty()) {
                    try {
                        PromptBundle end_prompts = composer.compose_end(*shot);
                        for (std::string& r : end_prompts.reference_images) {
                            r = paths::to_utf8(paths.abs(r));
                        }
                        const fs::path edest =
                            paths.frames() /
                            paths::from_utf8(shot->shot_id + "_end.png");
                        // id 加后缀：种子和首帧那张错开，预览也不会盖掉
                        // 墙上首帧那一格。
                        Shot end_shot = *shot;
                        end_shot.shot_id = shot->shot_id + "_end";
                        render(end_shot, end_prompts, scaled, edest, tok, on_step);
                        done[i].end_rel = paths.rel(edest);
                    } catch (const std::exception& e) {
                        pipeline::Event ev;
                        ev.stage = "frames";
                        ev.kind = "warn";
                        ev.shot_id = shot->shot_id;
                        ev.message = shot->shot_id + " 的尾帧没出来，这一镜走单帧：" +
                                     e.what();
                        progress.report(ev);
                    }
                }
            } catch (const std::exception& e) {
                // 一镜失败不拖垮后面几镜。跑一晚上，早上发现第三镜挂了
                // 导致后面三十镜都没动，那这一晚上就白熬了。
                done[i].ok = false;
                done[i].error = e.what();

                pipeline::Event ev;
                ev.stage = "frames";
                ev.kind = "warn";
                ev.shot_id = shot->shot_id;
                ev.message = shot->shot_id + " 出首帧失败：" + done[i].error;
                progress.report(ev);
            }
            done[i].elapsed_s = now_seconds() - started;

            // **出完一镜就落一次盘。** 不落的话这一批（一集二十二镜）
            // 跑完之前，镜头墙问到的永远是开跑那一刻的样子——
            // 一张缩略图都没有。
            if (commit) {
                std::lock_guard<std::mutex> lg(commit_mu);
                apply(i);
                commit();
            }

            // **这一张完了要说一声。** 同 audio.cpp 那处：界面把"带 shot_id
            // 的 progress"当成"这一镜正在跑"，靠一条非 progress 的事件把它
            // 移出去。只报 progress 的话，跑过的每一镜都永远挂在"正在出首帧"
            // 上，而墙上那些其实只是在等的镜头也跟着显示同一句。
            {
                // 变量别叫 done：外面那个数组就叫 done，遮蔽之后
                // `done[i]` 到底指谁要靠"声明点之前还是之后"来判断。
                pipeline::Event fin;
                fin.stage = "frames";
                fin.kind = "shot_done";
                fin.current = index;
                fin.total = total;
                fin.shot_id = shot->shot_id;
                fin.message =
                    shot->shot_id + (done[i].ok ? " 首帧完成" : " 首帧失败");
                progress.report(fin);
            }
        }
    };

    if (lanes == 1) {
        worker();               // 串行那条路一个线程都不起
    } else {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(lanes));
        for (int k = 0; k < lanes; ++k) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }

    // ---- 收。**在调用线程上顺序改 Shot** ----
    //
    // 上面那一段一个字节都没往 Shot 里写。写回集中在这儿，
    // 单线程、按镜头原顺序——存盘的那份 project.json 因此仍然只有一个写者。
    std::vector<FrameOutcome> outcomes;
    for (int i = 0; i < total; ++i) {
        if (done[i].skipped) continue;
        Shot* shot = shots[i];
        FrameOutcome out;
        out.shot_id = shot->shot_id;
        out.elapsed_s = done[i].elapsed_s;
        if (!done[i].committed) apply(i);
        if (done[i].ok) {
            out.ok = true;
            out.path = done[i].rel_path;
        } else {
            out.ok = false;
            out.error = done[i].error;
        }
        outcomes.push_back(out);
    }
    return outcomes;
}

}  // namespace changji::stages
