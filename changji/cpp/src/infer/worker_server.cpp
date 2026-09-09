#include "infer/worker_server.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <crow.h>

#include "infer/scheduler.hpp"
#include "infer/sd_image.hpp"
#include "infer/sd_video.hpp"
#include "infer/worker_proto.hpp"
#include "media/ffmpeg.hpp"
#include "pipeline/jobs.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"
#include "util/paths.hpp"

namespace changji::infer {

namespace {

using nlohmann::json;

/// 一个任务的活动状态。
///
/// **不存 std::thread。** 存了就得保证析构前 join 或 detach，而这里有好几条
/// 路走不到那一步：没人来查状态、进程收到 SIGTERM、任务还在跑时被顶掉。
/// 析构一个还 joinable 的 thread 会直接 `terminate called without an active
/// exception`——实机上就是这么崩的，日志里只有那一行，看不出和线程有关系。
///
/// 改成建完就 detach，靠 lambda 捕获的 shared_ptr<Live> 保证对象活到线程结束。
struct Live {
    TaskProgress progress;
    pipeline::CancelToken tok;
};

struct State {
    std::mutex mu;
    /// **一次只有一个。** 多了就 409，不排队——排队会让协调者那边的
    /// 并发上限失效：它以为派出去的都在跑，实际有几个在这儿排着。
    std::shared_ptr<Live> current;
    std::string current_id;
    std::atomic<std::uint64_t> next_id{1};
};

/// 接任务之前先看这活干不干得成。干不成就当场说，别跑到一半才发现。
///
/// **这条是实机烧出来的**：第一次跑出片，扩散 8 步全跑完，到最后编码那一步
/// 才报"找不到 ffmpeg"。本机上 doctor 会在起跑前拦，但直接给 worker 派任务
/// 绕过了那道检查。在 8 卡机器上，"跑几十秒再失败"乘以八就是几分钟白烧。
///
/// 回空串表示能干。
std::string cannot_do(const Task& t, const config::Settings& s) {
    const auto ws = s.workspace_path();

    // 出图出片都要扩散模型和它的文本编码器
    const std::string& which = t.kind == TaskKind::Video ? s.models.video
                                                        : s.models.image;
    if (which.empty()) {
        return std::string(t.kind == TaskKind::Video ? "[models].video"
                                                     : "[models].image") +
               " 没配，这个 worker 干不了" +
               (t.kind == TaskKind::Video ? "出片" : "出图");
    }
    for (const auto& [key, name] : std::vector<std::pair<const char*, std::string>>{
             {"扩散模型", which},
             {"VAE", t.kind == TaskKind::Video
                         ? s.models.video_vae
                         : (s.models.image_vae.empty() ? s.models.video_vae
                                                       : s.models.image_vae)},
         }) {
        if (name.empty()) continue;
        std::error_code ec;
        const auto p = s.models.resolve(name, ws);
        if (!std::filesystem::is_regular_file(p, ec)) {
            return std::string(key) + " 找不到：" + paths::to_utf8(p);
        }
    }

    // **出片要 ffmpeg 把帧编成 mp4。** 就是这一条烧过一次。
    if (t.kind == TaskKind::Video) {
        // check() 是 void，缺了就抛。这里把异常翻成一句话回给调用方。
        try {
            const media::FFmpeg ff(s.assembly.ffmpeg_path,
                                   s.assembly.ffprobe_path,
                                   media::default_runner());
            ff.check();
        } catch (const std::exception& e) {
            return e.what();
        }
    }

    // 产物目录得写得进去
    std::error_code ec;
    const auto dir = std::filesystem::path(paths::from_utf8(t.dest)).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec) return "产物目录建不出来：" + paths::to_utf8(dir);
    }
    return {};
}

crow::response json_res(const json& body, int code = 200) {
    crow::response res(code, body.dump());
    res.set_header("Content-Type", "application/json; charset=utf-8");
    return res;
}

}  // namespace

void run_worker(const config::Settings& settings, const WorkerOptions& opts) {
    // **绑卡靠 CUDA_VISIBLE_DEVICES。** 在建任何 ggml 上下文之前设，
    // 之后再设没用——后端初始化的时候就把设备列表读走了。
    //
    // ⚠️ 这一条在只有一张卡的机器上验不了。要是它对 ggml 的 CUDA 后端
    // 不生效，表现是**八个进程全挤在卡 0 上，看着在并行实际在排队，
    // 而且一声不吭**。上多卡机器第一件事就是验它。
    paths::set_env("CUDA_VISIBLE_DEVICES", std::to_string(opts.gpu));

    // 和主进程一样注册两个槽。**每个工作进程一份**——
    // 进程边界把"每卡一份预算"白送了，Scheduler 一行没改。
    register_sd_slots(settings, models::HardwareProfile::detect(
                                    settings.vram_gb_override));

    auto state = std::make_shared<State>();
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);

    CROW_ROUTE(app, "/health")([opts, state] {
        std::lock_guard lg(state->mu);
        return json_res({{"ok", true},
                         {"gpu", opts.gpu},
                         {"busy", state->current != nullptr}});
    });

    CROW_ROUTE(app, "/task").methods(crow::HTTPMethod::POST)(
        [state, settings](const crow::request& req) {
            Task task;
            try {
                task = task_from_json(json::parse(req.body));
            } catch (const std::exception& e) {
                return json_res({{"detail", std::string("任务读不懂：") + e.what()}},
                                400);
            }

            // **先自检再排队。** 干不成就当场说——这一条是烧过一次换来的。
            if (const auto why = cannot_do(task, settings); !why.empty()) {
                return json_res({{"detail", why}, {"shot_id", task.shot_id}}, 400);
            }

            std::lock_guard lg(state->mu);
            if (state->current) {
                // **不排队。** 见文件头。
                return json_res({{"detail", "正忙"}, {"busy", true}}, 409);
            }

            const std::string id =
                std::to_string(state->next_id.fetch_add(1));
            auto live = std::make_shared<Live>();
            live->progress.state = "running";

            // **建完就 detach**，见 Live 的注释。live 是 shared_ptr，
            // 被 lambda 捕获一份，线程跑多久它就活多久。
            std::thread([state, live, task, settings] {
                const auto on_step = [state, live](int step, int steps,
                                                  double, bool loading) {
                    std::lock_guard lg(state->mu);
                    live->progress.step = step;
                    live->progress.steps = steps;
                    live->progress.loading = loading;
                };
                TaskResult result;
                try {
                    const std::filesystem::path dest =
                        paths::from_utf8(task.dest);
                    if (task.kind == TaskKind::Frame) {
                        models::Shot shot;
                        shot.shot_id = task.shot_id;
                        stages::sd_renderer_with_seed(task.seed)(
                            shot, task.prompts, task.spec, dest, live->tok,
                            on_step);
                    } else {
                        models::Shot shot;
                        shot.shot_id = task.shot_id;
                        stages::RenderPlan plan;
                        plan.shot_id = task.shot_id;
                        plan.tier = task.tier;
                        plan.spec = task.spec;
                        plan.frames = task.frames;
                        plan.prompts = task.prompts;
                        plan.motion = task.motion;
                        plan.style_line = task.style_line;
                        std::optional<std::filesystem::path> start;
                        if (task.start_image) {
                            start = paths::from_utf8(*task.start_image);
                        }
                        sd_video_renderer_with_seed(settings, task.seed)(
                            shot, plan, start, dest, live->tok, on_step);
                    }
                    result.ok = true;
                    result.dest = task.dest;
                } catch (const std::exception& e) {
                    result.ok = false;
                    // 这句会一路变成协调者事件流里的那条 warn，
                    // 所以要能直接给用户看。
                    result.error = e.what();
                }
                std::lock_guard lg(state->mu);
                live->progress.state = result.ok ? "done" : "failed";
                live->progress.result = result;
            }).detach();

            state->current = live;
            state->current_id = id;
            return json_res({{"id", id}}, 202);
        });

    CROW_ROUTE(app, "/task/<string>")([state](const std::string& id) {
        std::lock_guard lg(state->mu);
        if (!state->current || state->current_id != id) {
            return json_res({{"detail", "没有这个任务"}}, 404);
        }
        const auto p = state->current->progress;
        if (p.state == "done" || p.state == "failed") {
            // 收完就放，好接下一个
            state->current.reset();
            state->current_id.clear();
        }
        return json_res(to_json(p));
    });

    CROW_ROUTE(app, "/task/<string>/cancel")
        .methods(crow::HTTPMethod::POST)([state](const std::string& id) {
            std::lock_guard lg(state->mu);
            if (!state->current || state->current_id != id) {
                return json_res({{"detail", "没有这个任务"}}, 404);
            }
            state->current->tok.request();
            return json_res({{"ok", true}});
        });

    CROW_LOG_INFO << "工作进程 gpu=" << opts.gpu << " 听 " << opts.host << ":"
                  << opts.port;
    app.bindaddr(opts.host).port(static_cast<std::uint16_t>(opts.port)).run();

    // ---- 收到信号，run() 返回了 ----
    //
    // **别让它走到静态析构。** 出片那个线程是 detach 的，可能还在 sd.cpp 里；
    // 调度器里的 SdContext（带 CUDA 上下文）在它脚下被析构，最后是
    // std::terminate——systemctl restart 时 journal 里那条
    // "code=dumped, status=6/ABRT" 就是它。
    //
    // 先取消当前任务，等它自己退出来（取消令牌在采样回调里查，一两步就停），
    // 最多等 30 秒，然后 _Exit：跳过所有析构。工作进程没有任何值得析构的
    // 东西——它的全部状态是内存里的模型缓存，进程一没就没了。
    {
        std::shared_ptr<Live> running;
        {
            std::lock_guard lg(state->mu);
            running = state->current;
        }
        if (running) {
            running->tok.request();
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (;;) {
                {
                    std::lock_guard lg(state->mu);
                    const auto& st = running->progress.state;
                    if (st == "done" || st == "failed") break;
                }
                if (std::chrono::steady_clock::now() > deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    std::_Exit(0);
}

}  // namespace changji::infer
