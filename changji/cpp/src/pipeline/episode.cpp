#include "pipeline/episode.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

#include "gates/checks.hpp"
#include "util/paths.hpp"
#include "media/assemble.hpp"
#include "stages/audio_plan.hpp"
#include "stages/storyboard.hpp"
#include "util/human_time.hpp"

namespace changji::pipeline {

using namespace changji::models;

/// 挑出这一阶段要跑的镜头。
///
/// **按 order 排序**，不是按在数组里的顺序。分镜表被手工改过之后，
/// 数组顺序和 order 可能对不上，而画面的连贯性是按 order 来的。
///
/// 返回的是**指针**，因为各阶段要就地改状态。Python 那边
/// `episode.sorted_shots()` 返回的是同一批对象的引用，
/// 而 C++ 侧那个函数返回的是拷贝——照抄名字会让所有状态改动写进临时对象，
/// 存盘时一个字段都没变。
std::vector<Shot*> pick(Episode& ep, const std::set<ShotStatus>& want,
                        bool force,
                        const std::set<std::string>& only_shots) {
    std::vector<Shot*> all;
    all.reserve(ep.shots.size());
    for (auto& s : ep.shots) all.push_back(&s);
    // stable_sort：order 相同的镜头保持原有先后，对齐 Python 的 sorted()。
    std::stable_sort(all.begin(), all.end(),
                     [](const Shot* a, const Shot* b) { return a->order < b->order; });

    std::vector<Shot*> todo;
    for (Shot* s : all) {
        // 指定了镜头就只认这几个。**先筛这一层**：不筛的话
        // force 会把整集都拉进来，而用户点的是某一镜的"重新生成"。
        if (!only_shots.empty() && only_shots.count(s->shot_id) == 0) continue;
        if (force || want.count(s->status)) todo.push_back(s);
    }
    return todo;
}

models::TierSpec frame_spec(const models::HardwareProfile& profile,
                            const config::Settings& settings) {
    const auto tier =
        settings.models.frame_tier == "draft" ? Tier::DRAFT : Tier::FINAL;
    auto it = profile.tiers.find(tier);
    // 档位表里没有这一档就退回另一档。**不抛**：档位表是推出来的，
    // 少一档也该照样出片，而不是让整集跑不起来。
    if (it == profile.tiers.end()) {
        it = profile.tiers.find(tier == Tier::DRAFT ? Tier::FINAL : Tier::DRAFT);
    }
    if (it == profile.tiers.end()) return {};

    models::TierSpec spec = it->second;
    if (settings.models.frame_steps > 0) spec.steps = settings.models.frame_steps;
    return spec;
}

void apply_project_spec(config::Settings& settings,
                        models::HardwareProfile& profile) {
    auto it = profile.tiers.find(models::Tier::FINAL);
    if (it == profile.tiers.end()) return;
    const auto eff = config::effective_spec(settings, it->second.steps);
    it->second.width = eff.width;
    it->second.height = eff.height;
    it->second.steps = eff.final_steps;
    settings.models.frame_steps = eff.frame_steps;
}

/// 一个档位的入口状态。
///
/// 首帧失败的镜头状态还停在 AUDIO_DONE（配音接上之前是 PLANNED）。
/// **它们不该被跳过**，而是退回纯文生视频——画面一致性差一些，
/// 但整集不会卡在这里。
std::set<ShotStatus> render_entry_states(Tier tier, bool skip_draft) {
    if (tier == Tier::FINAL) {
        std::set<ShotStatus> in{ShotStatus::DRAFT_DONE,
                                ShotStatus::FINAL_REJECTED};
        // **跳过草稿档时，成片档要收首帧刚做完的那一批。**
        // 不收的话它们卡在 FRAME_DONE 上，成片阶段一个镜头都挑不到，
        // 而且不报错——表现是"跑完了，什么都没出"。
        if (skip_draft) {
            in.insert(ShotStatus::FRAME_DONE);
            in.insert(ShotStatus::DRAFT_REJECTED);
            in.insert(ShotStatus::AUDIO_DONE);
        }
        return in;
    }
    return {ShotStatus::FRAME_DONE, ShotStatus::DRAFT_REJECTED,
            ShotStatus::AUDIO_DONE};
}

namespace {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool wants(const RunOptions& o, Stage s) {
    if (!o.only.has_value()) return true;   // 没给就是全跑
    return std::find(o.only->begin(), o.only->end(), s) != o.only->end();
}

void emit(JobProgress& p, const char* stage, const char* kind,
          const std::string& message, int current = 0, int total = 0) {
    Event e;
    e.stage = stage;
    e.kind = kind;
    e.message = message;
    e.current = current;
    e.total = total;
    p.report(e);
}

/// 一批镜头的 id。给 JobProgress::set_pending 用——每个阶段开工时登记
/// "这一轮还有哪几镜没落定"，界面刷新之后靠它把「排队中」重新点亮。
std::vector<std::string> ids_of(const std::vector<Shot*>& shots) {
    std::vector<std::string> out;
    out.reserve(shots.size());
    for (const Shot* s : shots) out.push_back(s->shot_id);
    return out;
}

/// 装配成片。
///
/// 单独一个函数只是为了让 run_episode 里那一段短一点——它已经有五个阶段了。
std::string run_assemble(const ProjectStore& store,
                         const config::Settings& settings, Episode& ep,
                         const media::FFmpeg& ff, JobProgress& progress) {
    // 只装配**已经出片而且过了闸门**的镜头。
    // DRAFT_DONE 也收：只跑草稿档验叙事时，那一档就是成品。
    static const std::set<ShotStatus> kUsable = {
        ShotStatus::FINAL_DONE, ShotStatus::DRAFT_DONE, ShotStatus::FALLBACK,
        ShotStatus::LOCKED};

    std::vector<Shot> shots;
    for (const auto& s : ep.sorted_shots()) {
        if (s.video_path.has_value() && !s.video_path->empty() &&
            kUsable.count(s.status)) {
            shots.push_back(s);
        }
    }
    if (shots.empty()) {
        throw std::runtime_error("没有可装配的镜头。先跑渲染阶段");
    }

    emit(progress, "assemble", "start",
         "装配 " + std::to_string(shots.size()) + " 个镜头", 0,
         static_cast<int>(shots.size()));

    // **装配前先查音画能不能装下**，装完再发现就得重做整集。
    // 查出问题只报不拦：拦下来的话一句台词长了一点整集就出不来，
    // 而那一点多半是听不出来的。
    for (const auto& shot : shots) {
        if (shot.dialogue.empty()) continue;
        const auto r = gates::gate_audio_sync(
            shot, store.paths().abs(*shot.video_path), ff, settings.gates);
        if (!r.ok()) {
            Event e;
            e.stage = "assemble";
            e.kind = "gate";
            e.shot_id = shot.shot_id;
            e.message = r.describe();
            progress.report(e);
        }
    }

    const media::Timeline timeline =
        media::build_timeline(shots, store.paths(), settings.assembly);
    media::Assembler assembler(ff, settings.assembly, store.paths(),
                               settings.gates.target_lufs,
                               settings.gates.max_true_peak_db);
    const auto output = assembler.assemble(timeline, ep.episode_id + ".mp4");

    // 成片检查同样只报不拦：片子已经出来了，人可以自己看一眼再决定。
    const auto result = gates::gate_episode(output, ff, settings.gates,
                                            timeline.total_duration_s());
    for (const auto& reason : result.reasons) {
        Event e;
        e.stage = "assemble";
        e.kind = "gate";
        e.message = "成片：" + reason;
        progress.report(e);
    }

    emit(progress, "assemble", "done", "成片已生成：" + paths::to_utf8(output),
         static_cast<int>(shots.size()), static_cast<int>(shots.size()));
    return paths::to_utf8(output);
}

}  // namespace

const char* to_string(Stage s) {
    switch (s) {
        case Stage::Audio:    return "audio";
        case Stage::Frames:   return "frames";
        case Stage::Draft:    return "draft";
        case Stage::Final:    return "final";
        case Stage::Assemble: return "assemble";
    }
    return "?";
}

bool stage_from_string(const std::string& s, Stage& out) {
    if (s == "audio")    { out = Stage::Audio;    return true; }
    if (s == "frames")   { out = Stage::Frames;   return true; }
    if (s == "draft")    { out = Stage::Draft;    return true; }
    if (s == "final")    { out = Stage::Final;    return true; }
    if (s == "assemble") { out = Stage::Assemble; return true; }
    return false;
}

RunReport run_episode(const ProjectStore& store,
                      const HardwareProfile& profile,
                      const config::Settings& settings, const RunOptions& opts,
                      const Backends& backends, JobProgress& progress,
                      CancelToken& tok) {
    RunReport report;
    report.episode_id = opts.episode_id;
    const double started = now_seconds();

    Project project = store.load_project();
    const AssetLibrary assets = store.load_assets();
    Episode* ep = project.episode_by_id(opts.episode_id);
    if (ep == nullptr) {
        throw std::runtime_error("项目里没有剧集 " + opts.episode_id);
    }
    if (ep->shots.empty()) {
        throw std::runtime_error("剧集 " + opts.episode_id + " 还没有分镜表");
    }

    progress.set_episode_id(opts.episode_id);

    // 每个阶段跑完立刻存盘。不存的话中途断电或者点了停止，
    // 前面几十分钟的产出全部作废——文件还在磁盘上，但项目文件里没记，
    // 下次跑会当成没跑过。
    const auto save = [&] { store.save_project(project); };

    // 渲染一个档位。草稿和成片只差三个东西：入口状态、档位参数、事件名。
    const auto render_tier = [&](Tier tier, bool force) {
        const char* stage_name = tier == Tier::FINAL ? "final" : "draft";
        auto todo = pick(*ep, render_entry_states(tier, opts.skip_draft), force,
                         opts.only_shots);
        progress.set_pending(ids_of(todo));
        if (todo.empty()) {
            emit(progress, stage_name, "done",
                 std::string(models::to_string(tier)) + " 档已完成，跳过");
            return std::vector<stages::RenderOutcome>{};
        }

        // 消息里报的是**表里的**分辨率，不是按画幅缩放后的。
        // Python 就是这样，而且这样才对得上设置页上显示的数字——
        // 用户在那儿填的是 640x352，看到日志里写 448x768 会以为设置没生效。
        const TierSpec& spec = profile.tiers.at(tier);
        std::string msg = std::string(models::to_string(tier)) + " 档渲染 " +
                          std::to_string(todo.size()) + " 个镜头，" +
                          std::to_string(spec.width) + "x" +
                          std::to_string(spec.height) + " " +
                          std::to_string(spec.steps) + " 步";
        if (const auto est = profile.estimate_episode(
                static_cast<int>(todo.size()), tier)) {
            msg += "，粗估 " + util::human_time(*est);
        }
        emit(progress, stage_name, "start", msg, 0,
             static_cast<int>(todo.size()));

        // ---- 闸门 ----
        //
        // 以前这儿没有。`gate_video` 和 `decide_next` 移植了、也和 Python
        // 一条不差地对过，但真二进制里从来没人调——出完片直接置
        // DRAFT_DONE。也就是 C++ 从不拦废片、从不重试、从不降级，
        // 而 Python 默认每一镜都过。整套质量控制在这边是空的。
        stages::GateHooks gate;
        // 上限**不管闸门开没开都生效**：渲染抛错的重试也数它。
        gate.max_attempts = settings.gates.max_attempts_per_shot;
        if (settings.gates.enabled) {
            if (backends.ffmpeg) {
                const media::FFmpeg& ff = *backends.ffmpeg;
                const config::GateConfig& gcfg = settings.gates;
                const int fps = settings.assembly.fps;
                // Python：f"{spec.tier.value} 档闸门"
                const std::string gate_name =
                    std::string(models::to_string(tier)) + " 档闸门";
                gate.check = [&ff, &gcfg, fps, gate_name](
                                 const Shot& shot,
                                 const std::filesystem::path& video,
                                 const stages::RenderPlan& plan) {
                    // 期望时长按帧数反推，不按 duration_s——帧数是 4n+1
                    // 截过的，真片长就是它。Python 也是这么算的。
                    const double expected =
                        static_cast<double>(
                            stages::frames_for(shot.duration_s, fps)) /
                        fps;
                    return gates::gate_video(
                        shot, video, ff, gcfg, expected,
                        std::make_pair(plan.spec.width, plan.spec.height),
                        gate_name);
                };
                gate.decide = [&gcfg](const gates::GateResult& r,
                                      const Shot& s) {
                    return gates::decide_next(r, s, gcfg);
                };
            } else {
                // Python 这时候会在 gate_video 里炸。这边选择说一声然后
                // 不过闸门——没装 ffmpeg 的机器上前几步照样能跑，
                // 而"跑到闸门才说缺 ffmpeg"最气人。装配那一步也是这么处理的。
                emit(progress, stage_name, "warn",
                     "闸门开着但没找到 ffmpeg，这一档不过闸门");
            }
        }

        // fps 走配置，不是写死的 24。Python 那边是
        // `RenderStage(..., fps=self.settings.assembly.fps)`；这儿以前
        // 漏了这个参数吃了默认值，`[assembly].fps = 30` 时两边算出来的
        // 帧数不同——**片长会不一样**，而没有任何一层会报错。
        return stages::render_batch(todo, assets, spec, store.paths(),
                                    backends.video, progress, tok,
                                    settings.assembly.fps,
                                    backends.render_lanes, gate, save);
    };

    try {
        // ---- 配音 ----
        //
        // **在生成任何画面之前跑完。** 它决定镜头时长，而时长决定帧数。
        // 顺序反过来的话，配音出来装不进已经渲好的视频里。
        if (wants(opts, Stage::Audio) && !tok.cancelled()) {
            auto todo = pick(*ep, {ShotStatus::PLANNED}, opts.force, opts.only_shots);
            progress.set_pending(ids_of(todo));
            if (todo.empty()) {
                emit(progress, "audio", "done", "配音已完成，跳过");
            } else {
                const stages::TTSBackend backend =
                    backends.tts.value_or(stages::estimate_backend());
                // **后端名字对用户没有意义，要说清楚这次到底出不出声音。**
                // 只说 "estimate" 的话，用户跑完一整集才发现成片是静音的。
                const std::string how =
                    backend.name == "estimate" ? "只算时长不出声音，成片会是静音"
                    : backend.name == "http"   ? "走独立配音服务"
                                               : backend.name;
                emit(progress, "audio", "start",
                     "给 " + std::to_string(todo.size()) + " 个镜头配音，" + how,
                     0, static_cast<int>(todo.size()));

                stages::AudioStage stage(backend, settings.tts, store.paths());
                report.audio = stage.run(todo, assets, progress, tok);
                save();

                // 台词太多装不下的镜头，在这里拆成连着的几镜。
                //
                // **拆在配音之后、出首帧之前**：音频已经有了、画面还没生成，
                // 拆开不浪费任何一次渲染。
                const std::size_t before = ep->shots.size();
                ep->shots = stages::split_overlong_shots(
                    ep->shots, stages::max_shot_duration_s(settings.assembly.fps));
                const std::size_t added = ep->shots.size() - before;
                if (added > 0) {
                    emit(progress, "audio", "warn",
                         "有 " + std::to_string(added) +
                             " 处台词一镜装不下，已拆成新的镜头。这一集现在是 " +
                             std::to_string(ep->shots.size()) + " 个镜头");
                }
                save();

                emit(progress, "audio", "done", stages::summarize(report.audio),
                     static_cast<int>(todo.size()),
                     static_cast<int>(todo.size()));
            }
        }

        // ---- 首帧 ----
        if (wants(opts, Stage::Frames) && !tok.cancelled()) {
            // **入口状态只有 AUDIO_DONE。** 配音跑完锁了时长才出首帧——
            // 时长决定帧数，帧数决定这一镜的画面，顺序反过来的话
            // 配音出来装不进已经渲好的视频里。
            //
            // 阶段 5 时这里临时放宽收了 PLANNED（那会儿配音还没移植），
            // 现在配音接上了，收回来。配音失败的镜头状态停在 PLANNED，
            // 于是自动被挡在首帧之外——那是对的，它们带着错的时长。
            auto todo = pick(*ep, {ShotStatus::AUDIO_DONE}, opts.force, opts.only_shots);
            progress.set_pending(ids_of(todo));
            if (todo.empty()) {
                emit(progress, "frames", "done", "首帧已完成，跳过");
            } else {
                emit(progress, "frames", "start",
                     "给 " + std::to_string(todo.size()) + " 个镜头出首帧（" +
                         backends.frame_backend_name + "）",
                     0, static_cast<int>(todo.size()));

                report.frames = stages::run_frames(
                    todo, assets, frame_spec(profile, settings),
                    store.paths(), backends.frame, progress, tok,
                    // 同时跑几镜。**没有池就是 1**，行为和以前一样。
                    // 有池就取池的大小——这一层不知道有几张卡，
                    // 但它知道池里有几个工作进程。
                    backends.render_lanes,
                    // 每出完一张就存一次。见 pipeline::ShotCommit——
                    // 不存的话这一批跑完之前镜头墙上一张缩略图都没有。
                    save);
                save();

                const int n = static_cast<int>(report.frames.size());
                int failed = 0;
                for (const auto& o : report.frames) {
                    if (!o.ok) ++failed;
                }
                std::string done_msg =
                    "首帧完成 " + std::to_string(n - failed) + " 个";
                if (failed > 0) {
                    done_msg += "，失败 " + std::to_string(failed) +
                                " 个（这些镜头会退回纯文生视频）";
                }
                emit(progress, "frames", "done", done_msg, n, n);
            }
        }

        // ---- 草稿档 ----
        //
        // skip_draft：两档拉不开差距时它就是白跑一遍（挂 Turbo LoRA 之后
        // 正是这个局面）。跳过之后成片档会收 FRAME_DONE 那批，
        // 见 render_entry_states。
        if (!opts.skip_draft && wants(opts, Stage::Draft) && !tok.cancelled()) {
            report.draft = render_tier(Tier::DRAFT, opts.force);
            save();
        }

        // ---- 成片档 ----
        //
        // skip_final 是为了快速验证叙事：成片档一个镜头几分钟，
        // 而叙事对不对看草稿就够了。
        if (!opts.skip_final && wants(opts, Stage::Final) && !tok.cancelled()) {
            // **跑全流程时成片档不吃 force，只有单跑这个阶段时才吃。**
            // 照抄 Python：run() 里给的是 force=False，run_stages() 里
            // 给的是 force=force。
            //
            // 差别在草稿失败的那几镜：force 重跑草稿之后它们状态没变，
            // 成片阶段跳过它们是对的——拿一个没渲出来的草稿去出成片，
            // 出来的是另一段没有首帧参考的片子，混在成片目录里最难发现。
            const bool force_final = opts.only.has_value() ? opts.force : false;
            report.final_ = render_tier(Tier::FINAL, force_final);
            save();
        }

        // ---- 装配 ----
        if (wants(opts, Stage::Assemble) && !tok.cancelled()) {
            if (!backends.ffmpeg.has_value()) {
                // 没装 ffmpeg 时前面几步照样跑完了。**跑到最后一步才说
                // 缺 ffmpeg 最气人**，所以这里说清楚缺的是什么、产物在哪。
                emit(progress, "assemble", "warn",
                     "没有 ffmpeg，跳过装配。各镜头的视频已经在 shots/ 下，"
                     "装好之后单跑 assemble 阶段即可");
            } else {
                report.output = run_assemble(store, settings, *ep,
                                             *backends.ffmpeg, progress);
            }
        }
    } catch (const std::exception& e) {
        report.errors.push_back(e.what());
        emit(progress, "assemble", "error", e.what());
    }

    // 对齐 Python 的 finally：无论成功、失败还是中途停止都存一次。
    // 上面各阶段已经存过了，这里再存一次是为了兜住"还没跑到任何一个
    // 阶段就被取消"和"阶段中间抛异常"这两种情况——那时候镜头的
    // attempts 可能已经加过了，不存的话下次重跑的重试计数是错的。
    try {
        save();
    } catch (const std::exception& e) {
        // 存不进去也要说，但不能盖掉真正的错误——所以是追加不是替换。
        report.errors.push_back(std::string("存盘失败：") + e.what());
    }

    report.elapsed_s = now_seconds() - started;
    return report;
}

}  // namespace changji::pipeline
