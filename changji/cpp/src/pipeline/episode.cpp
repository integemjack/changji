#include "pipeline/episode.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

#include "util/human_time.hpp"

namespace changji::pipeline {

using namespace changji::models;

namespace {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool wants(const RunOptions& o, Stage s) {
    if (o.only.empty()) return true;
    return std::find(o.only.begin(), o.only.end(), s) != o.only.end();
}

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
                        bool force) {
    std::vector<Shot*> all;
    all.reserve(ep.shots.size());
    for (auto& s : ep.shots) all.push_back(&s);
    // stable_sort：order 相同的镜头保持原有先后，对齐 Python 的 sorted()。
    std::stable_sort(all.begin(), all.end(),
                     [](const Shot* a, const Shot* b) { return a->order < b->order; });

    std::vector<Shot*> todo;
    for (Shot* s : all) {
        if (force || want.count(s->status)) todo.push_back(s);
    }
    return todo;
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

/// 一个档位的入口状态。
///
/// 首帧失败的镜头状态还停在 AUDIO_DONE（配音接上之前是 PLANNED）。
/// **它们不该被跳过**，而是退回纯文生视频——画面一致性差一些，
/// 但整集不会卡在这里。
std::set<ShotStatus> render_entry_states(Tier tier) {
    if (tier == Tier::FINAL) {
        return {ShotStatus::DRAFT_DONE, ShotStatus::FINAL_REJECTED};
    }
    return {ShotStatus::FRAME_DONE, ShotStatus::DRAFT_REJECTED,
            ShotStatus::AUDIO_DONE,
            // 配音还没移植（阶段 7），镜头到不了 AUDIO_DONE，
            // 所以这里也要收 PLANNED。见下面 run_episode 里的长注释。
            ShotStatus::PLANNED};
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
                      const HardwareProfile& profile, const RunOptions& opts,
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
        auto todo = pick(*ep, render_entry_states(tier), force);
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

        return stages::render_batch(todo, assets, spec, store.paths(),
                                    backends.video, progress, tok);
    };

    try {
        // ---- 首帧 ----
        if (wants(opts, Stage::Frames) && !tok.cancelled()) {
            // Python 那边的入口状态只有 AUDIO_DONE：配音跑完锁了时长
            // 才出首帧。**配音还没移植（阶段 7），所以这里也要收 PLANNED**——
            // 只认 AUDIO_DONE 的话现在一个镜头都挑不出来，
            // 而那表现为"点了开始，进度条转一下就说完成了，一张图也没有"。
            //
            // 阶段 7 接上配音之后要回头收紧到只认 AUDIO_DONE，
            // 否则配音失败的镜头会带着错的时长往下走。
            auto todo = pick(*ep, {ShotStatus::AUDIO_DONE, ShotStatus::PLANNED},
                             opts.force);
            if (todo.empty()) {
                emit(progress, "frames", "done", "首帧已完成，跳过");
            } else {
                emit(progress, "frames", "start",
                     "给 " + std::to_string(todo.size()) + " 个镜头出首帧（" +
                         backends.frame_backend_name + "）",
                     0, static_cast<int>(todo.size()));

                report.frames = stages::run_frames(
                    todo, assets, profile.tiers.at(Tier::DRAFT),
                    store.paths(), backends.frame, progress, tok);
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
        if (wants(opts, Stage::Draft) && !tok.cancelled()) {
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
            const bool force_final = opts.only.empty() ? false : opts.force;
            report.final_ = render_tier(Tier::FINAL, force_final);
            save();
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
