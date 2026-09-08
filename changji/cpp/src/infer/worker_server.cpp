#include "infer/worker_server.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <crow.h>

#include "infer/scheduler.hpp"
#include "infer/sd_image.hpp"
#include "infer/sd_video.hpp"
#include "infer/worker_proto.hpp"
#include "pipeline/jobs.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"
#include "util/paths.hpp"

namespace changji::infer {

namespace {

using nlohmann::json;

/// 一个任务的活动状态。
struct Live {
    TaskProgress progress;
    pipeline::CancelToken tok;
    std::thread worker;
};

struct State {
    std::mutex mu;
    /// **一次只有一个。** 多了就 409，不排队——排队会让协调者那边的
    /// 并发上限失效：它以为派出去的都在跑，实际有几个在这儿排着。
    std::shared_ptr<Live> current;
    std::string current_id;
    std::atomic<std::uint64_t> next_id{1};
};

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

            std::lock_guard lg(state->mu);
            if (state->current) {
                // **不排队。** 见文件头。
                return json_res({{"detail", "正忙"}, {"busy", true}}, 409);
            }

            const std::string id =
                std::to_string(state->next_id.fetch_add(1));
            auto live = std::make_shared<Live>();
            live->progress.state = "running";

            live->worker = std::thread([state, live, task, settings] {
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
            });

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
            auto done = state->current;
            state->current.reset();
            state->current_id.clear();
            if (done->worker.joinable()) done->worker.detach();
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
}

}  // namespace changji::infer
