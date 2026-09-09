#include "stages/render.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "gates/checks.hpp"
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

/// 把闸门给的几条理由拼成一句。分隔符照抄 gates 那边的全角分号——
/// 这句会原样进 gate_notes，人在界面上读的就是它。
std::string join_reasons(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += "；";
        out += v[i];
    }
    return out;
}

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
    p.style_line = composer.style_line();
    return p;
}

std::string video_positive(const RenderPlan& plan) {
    const std::string sep = plan.style_line == StyleLine::ANIME ? ", " : "，";
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
                                        pipeline::CancelToken& tok, int fps,
                                        int concurrency,
                                        const GateHooks& gate) {
    const PromptComposer composer(assets);
    const std::string stage_name =
        spec.tier == Tier::FINAL ? "final" : "draft";

    const int total = static_cast<int>(shots.size());

    /// 一镜跑完之后的东西。**只装数据，不碰调用方的 Shot。**
    ///
    /// `shot` 是那份本地副本，闸门循环改的就是它。收的时候整份写回去——
    /// 一次赋值，比逐个字段抄少一类"新加了字段忘了抄"的错。
    struct Done {
        bool ok = false;
        std::string error;
        std::string rel_path;
        double elapsed_s = 0.0;
        bool skipped = false;  ///< 取消了，没跑
        Shot shot;
    };
    std::vector<Done> done(shots.size());

    // 开跑前的预计来自一张按显存推的静态表，实测能差一倍。
    // 跑起来之后用真实耗时重算，等的人才知道还要等多久。
    const double stage_started = now_seconds();

    // 同时跑几镜。**上限是镜头数**——池里八个而只有三镜时，
    // 起八个线程只是白占。
    const int lanes = std::max(1, std::min(concurrency, total));

    std::atomic<int> next{0};
    std::atomic<int> finished{0};

    const auto lane = [&] {
        for (;;) {
            const int i = next.fetch_add(1);
            if (i >= total) return;
            if (tok.cancelled()) {
                done[i].skipped = true;
                continue;
            }

            Shot* shot = shots[i];
            const int index = i + 1;
            const double started = now_seconds();

            // **本地副本。** 重试要改 attempts，而 attempts 进种子——
            // 不换种子的重试就是把同一张牌再打一遍。改副本是为了让
            // 并行那一段仍然一个字节都不往 shots 里写。
            Shot local = *shot;

            {
                pipeline::Event e;
                e.stage = stage_name;
                e.kind = "progress";
                e.current = index;
                e.total = total;
                e.shot_id = local.shot_id;
                e.message = "出视频 " + local.shot_id;
                progress.report(e);
            }

            const auto say = [&](const char* kind, const std::string& msg) {
                pipeline::Event e;
                e.stage = stage_name;
                e.kind = kind;
                e.current = index;
                e.total = total;
                e.shot_id = local.shot_id;
                e.message = msg;
                progress.report(e);
            };

            // 重试超限，用能用的东西顶上。**不是停下来**——
            // 无人值守时停下来等于整集废掉。
            const auto fallback = [&](const std::string& reason) {
                local.status = ShotStatus::FALLBACK;
                local.gate_notes = {reason};
                // Python 的 _fallback 发的这条**不带 current/total**——
                // 它不在循环那几条的行列里。照抄，别顺手"补全"。
                pipeline::Event e;
                e.stage = stage_name;
                e.kind = "warn";
                e.shot_id = local.shot_id;
                e.message = local.shot_id + " 重试超限，降级处理：" + reason;
                progress.report(e);
            };

            const ShotStatus want_after = spec.tier == Tier::FINAL
                                              ? ShotStatus::FINAL_DONE
                                              : ShotStatus::DRAFT_DONE;

            for (;;) {
                if (tok.cancelled()) { done[i].skipped = true; break; }

                fs::path dest;
                RenderPlan plan;
                try {
                    plan = make_plan(local, spec, composer,
                                     assets.style.aspect_ratio, fps);

                    // 首帧是这一镜的起点。没有的话退回纯文生视频——
                    // 那样跨镜头一致性会掉一大截，但总比整条流水线卡住强。
                    std::optional<fs::path> start;
                    if (local.frame_path.has_value() &&
                        !local.frame_path->empty()) {
                        const fs::path frame = paths.abs(*local.frame_path);
                        std::error_code ec;
                        if (fs::is_regular_file(frame, ec)) {
                            start = frame;
                        } else {
                            say("warn", local.shot_id +
                                            " 记着首帧但文件不在，这一镜退回纯文生视频");
                        }
                    }

                    // 按档位分目录。草稿和成片混在一起的话，重跑成片时
                    // 分不清哪个 mp4 是哪一档的，而它们文件名只差一个后缀。
                    dest = paths.shots(stage_name) /
                           paths::from_utf8(local.shot_id + ".mp4");

                    // **并发时几镜同时报**，靠 Event 里的 shot_id 分得开；
                    // JobProgress::report 自己有锁。
                    const auto on_step = [&](int step, int steps, double,
                                             bool loading) {
                        say("progress",
                            // 不是采样的那些阶段（搬权重、VAE 分块解码、
                            // 首次载权重）分不开，所以只说"准备"，
                            // 别说"加载模型"——那会让人以为每镜都重载。
                            loading ? "出视频 " + local.shot_id + "（准备 " +
                                          std::to_string(step) + "/" +
                                          std::to_string(steps) + "）"
                                    : "出视频 " + local.shot_id + "（第 " +
                                          std::to_string(step) + "/" +
                                          std::to_string(steps) + " 步）");
                    };

                    render(local, plan, start, dest, tok, on_step);
                    local.video_path = paths.rel(dest);
                } catch (const std::exception& e) {
                    // 一镜失败不拖垮后面几镜。跑一晚上，早上发现第三镜挂了
                    // 导致后面三十镜都没动，那这一晚上就白熬了。
                    local.attempts += 1;
                    done[i].error = e.what();
                    say("warn", local.shot_id + " 渲染失败：" + done[i].error);
                    if (local.attempts >= gate.max_attempts) {
                        fallback(std::string("渲染连续失败：") + e.what());
                        break;
                    }
                    continue;
                }

                if (!gate.check) {
                    // 没配闸门。这是加这个参数之前的行为。
                    local.status = want_after;
                    done[i].ok = true;
                    done[i].rel_path = *local.video_path;
                    say("shot_done", local.shot_id + " 完成");
                    break;
                }

                const gates::GateResult res = gate.check(local, dest, plan);
                if (res.ok()) {
                    local.status = want_after;
                    local.gate_notes.clear();
                    done[i].ok = true;
                    done[i].rel_path = *local.video_path;
                    say("shot_done", local.shot_id + " 通过闸门");
                    break;
                }

                const gates::Verdict verdict = gate.decide(res, local);
                local.gate_notes = res.reasons;
                say("gate", res.describe());

                if (verdict == gates::Verdict::Retry) {
                    local.attempts += 1;
                    continue;
                }
                if (verdict == gates::Verdict::Fallback) {
                    fallback(join_reasons(res.reasons));
                    break;
                }
                // Regress：重跑没用，标记后交给人。
                local.status = spec.tier == Tier::FINAL
                                   ? ShotStatus::FINAL_REJECTED
                                   : ShotStatus::DRAFT_REJECTED;
                done[i].error = join_reasons(res.reasons);
                break;
            }

            done[i].shot = std::move(local);

            done[i].elapsed_s = now_seconds() - started;

            // 剩余时间按**已经跑完的这几镜**推，不按静态表。
            // 失败的那几镜也算进去：它们也花了时间（而且往往花得更多，
            // 失败通常发生在跑完大半之后）。
            //
            // 分母用**跑完的个数**而不是序号：并发时序号先到的未必先跑完，
            // 而 `墙上时间 / 跑完个数` 恰好就是吞吐的倒数——几路都对。
            const int did = finished.fetch_add(1) + 1;
            const int left = total - did;
            if (left > 0) {
                const double per = (now_seconds() - stage_started) / did;
                pipeline::Event e;
                e.stage = stage_name;
                e.kind = "eta";
                e.current = did;
                e.total = total;
                e.message = "还剩 " + std::to_string(left) +
                            " 个镜头，按目前速度约 " +
                            util::human_time(per * left);
                progress.report(e);
            }
        }
    };

    if (lanes == 1) {
        lane();  // 串行那条路一个线程都不起
    } else {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(lanes));
        for (int k = 0; k < lanes; ++k) pool.emplace_back(lane);
        for (auto& t : pool) t.join();
    }

    // ---- 收。**在调用线程上顺序改 Shot** ----
    //
    // 上面那一段一个字节都没往 Shot 里写。写回集中在这儿，
    // 单线程、按镜头原顺序——存盘的那份 project.json 因此仍然只有一个写者。
    std::vector<RenderOutcome> outcomes;
    for (int i = 0; i < total; ++i) {
        if (done[i].skipped) continue;
        Shot* shot = shots[i];
        RenderOutcome out;
        out.shot_id = shot->shot_id;
        out.elapsed_s = done[i].elapsed_s;
        // 整份写回。状态、attempts、gate_notes、video_path 都在副本里，
        // 闸门循环已经按 Python 的判定改好了——这儿只负责搬。
        *shot = std::move(done[i].shot);
        out.ok = done[i].ok;
        out.path = done[i].rel_path;
        out.error = done[i].error;
        outcomes.push_back(out);
    }
    return outcomes;
}

}  // namespace changji::stages
