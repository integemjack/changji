#include "infer/worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "infer/worker_proto.hpp"
#include "util/httplib.hpp"
#include "util/paths.hpp"

namespace changji::infer {

namespace {

using nlohmann::json;

/// 把 `http://host:port` 拆成 httplib 要的样子。
std::pair<std::string, std::string> split_url(const std::string& url) {
    const auto pos = url.find("://");
    const std::string rest =
        pos == std::string::npos ? url : url.substr(pos + 3);
    const auto slash = rest.find('/');
    if (slash == std::string::npos) {
        return {url, ""};
    }
    return {url.substr(0, pos == std::string::npos ? slash : pos + 3 + slash),
            rest.substr(slash)};
}

}  // namespace

struct WorkerPool::Impl {
    struct Worker {
        WorkerEndpoint ep;
        /// 这个工作进程正忙着。**一个进程一次只跑一个任务**——
        /// 它那边也会拒（409），这里只是不去撞而已。
        bool busy = false;
    };

    std::vector<Worker> workers;
    std::mutex mu;
    std::condition_variable cv;

    /// 借一个空闲的。没有就等——**不是失败**：
    /// 阶段那一层的并发上限就是靠这个卡住的。
    std::size_t take() {
        std::unique_lock lk(mu);
        cv.wait(lk, [&] {
            for (const auto& w : workers) {
                if (!w.busy) return true;
            }
            return false;
        });
        for (std::size_t i = 0; i < workers.size(); ++i) {
            if (!workers[i].busy) {
                workers[i].busy = true;
                return i;
            }
        }
        throw std::runtime_error("借工作进程时状态乱了");  // 到不了
    }

    void give_back(std::size_t i) {
        {
            std::lock_guard lg(mu);
            workers[i].busy = false;
        }
        cv.notify_one();
    }

    /// 把一个任务跑完。**同步**——阶段那一层本来就在自己的线程里等。
    void run_task(const Task& task, pipeline::CancelToken& tok,
                  const StepCallback& on_step) {
        const std::size_t idx = take();
        struct Release {
            Impl* self;
            std::size_t i;
            ~Release() { self->give_back(i); }
        } release{this, idx};

        const auto [origin, prefix] = split_url(workers[idx].ep.url);
        httplib::Client cli(origin);
        cli.set_connection_timeout(10, 0);
        cli.set_read_timeout(600, 0);

        auto res = cli.Post(prefix + "/task", to_json(task).dump(),
                            "application/json");
        if (!res) {
            throw std::runtime_error("连不上工作进程 " + workers[idx].ep.url +
                                     "：" + httplib::to_string(res.error()));
        }
        if (res->status == 409) {
            throw std::runtime_error("工作进程 " + workers[idx].ep.url +
                                     " 正忙。**这不该发生**——"
                                     "池这边已经不派给忙着的了，"
                                     "多半是有别人也在用同一个工作进程");
        }
        if (res->status != 202 && res->status != 200) {
            throw std::runtime_error("工作进程拒了这个任务（" +
                                     std::to_string(res->status) + "）：" +
                                     res->body.substr(0, 200));
        }
        const auto accepted = json::parse(res->body, nullptr, false);
        if (accepted.is_discarded() || !accepted.contains("id")) {
            throw std::runtime_error("工作进程回的不是 {\"id\":...}：" +
                                     res->body.substr(0, 200));
        }
        const std::string id = accepted["id"].get<std::string>();

        // 轮询。**间隔别太短**：出一张图是几十秒到几分钟，
        // 每 100 毫秒问一次纯粹是给对面添乱。
        for (;;) {
            if (tok.cancelled()) {
                cli.Post(prefix + "/task/" + id + "/cancel", "",
                         "application/json");
                throw std::runtime_error("取消了");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            auto st = cli.Get(prefix + "/task/" + id);
            if (!st) {
                throw std::runtime_error(
                    "工作进程 " + workers[idx].ep.url +
                    " 断了。**这一镜算失败，整条不停**——"
                    "崩溃隔离就是这么来的：" +
                    httplib::to_string(st.error()));
            }
            const auto body = json::parse(st->body, nullptr, false);
            if (body.is_discarded()) {
                throw std::runtime_error("工作进程回的进度不是 JSON");
            }
            const auto p = task_progress_from_json(body);
            if (p.steps > 0 && on_step) {
                on_step(p.step, p.steps, 0.0, p.loading);
            }
            if (p.state == "done" || p.state == "failed") {
                if (!p.result) throw std::runtime_error("跑完了却没有结果");
                if (!p.result->ok) throw std::runtime_error(p.result->error);
                return;
            }
        }
    }
};

WorkerPool::WorkerPool(std::vector<WorkerEndpoint> endpoints)
    : impl_(std::make_unique<Impl>()) {
    for (auto& e : endpoints) impl_->workers.push_back({std::move(e), false});
}

WorkerPool::~WorkerPool() = default;

std::size_t WorkerPool::size() const { return impl_->workers.size(); }

std::size_t WorkerPool::alive() const {
    std::size_t n = 0;
    for (const auto& w : impl_->workers) {
        const auto [origin, prefix] = split_url(w.ep.url);
        httplib::Client cli(origin);
        cli.set_connection_timeout(3, 0);
        auto res = cli.Get(prefix + "/health");
        if (res && res->status == 200) ++n;
    }
    return n;
}

stages::FrameRenderer WorkerPool::frame_renderer() {
    Impl* impl = impl_.get();
    return [impl](const models::Shot& shot, const stages::PromptBundle& prompts,
                  const models::TierSpec& spec,
                  const std::filesystem::path& dest, pipeline::CancelToken& tok,
                  const StepCallback& on_step) {
        Task t;
        t.kind = TaskKind::Frame;
        t.shot_id = shot.shot_id;
        t.prompts = prompts;
        t.spec = spec;
        t.dest = paths::to_utf8(dest);
        // **种子在这儿算，不让工作进程算**：它不知道 attempts。
        t.seed = stages::frame_seed(shot.shot_id, shot.attempts);
        impl->run_task(t, tok, on_step);
    };
}

stages::VideoRenderer WorkerPool::video_renderer() {
    Impl* impl = impl_.get();
    return [impl](const models::Shot& shot, const stages::RenderPlan& plan,
                  const std::optional<std::filesystem::path>& start_image,
                  const std::filesystem::path& dest, pipeline::CancelToken& tok,
                  const StepCallback& on_step) {
        Task t;
        t.kind = TaskKind::Video;
        t.shot_id = shot.shot_id;
        t.prompts = plan.prompts;
        t.spec = plan.spec;
        t.frames = plan.frames;
        t.motion = plan.motion;
        t.style_line = plan.style_line;
        t.tier = plan.tier;
        if (start_image) t.start_image = paths::to_utf8(*start_image);
        t.dest = paths::to_utf8(dest);
        // **出片用 render_seed，不是 frame_seed。** 两个是不同的函数，
        // 算出来的种子不同、画面就不同。写错了不会报错、不会变慢，
        // 只是走池出来的片和进程内的不一样——而没有哪一层会去比这个。
        // 是"产物逐字节比"抓出来的：同一条路跑两遍字节相同（GPU 是确定的），
        // 池和进程内比就不同，差别就在这一行。
        t.seed = stages::render_seed(shot.shot_id, shot.attempts);
        impl->run_task(t, tok, on_step);
    };
}

std::shared_ptr<WorkerPool> make_worker_pool(
    const std::vector<std::string>& endpoints) {
    if (endpoints.empty()) return nullptr;
    std::vector<WorkerEndpoint> eps;
    eps.reserve(endpoints.size());
    for (const auto& u : endpoints) eps.push_back(WorkerEndpoint{u});
    return std::make_shared<WorkerPool>(std::move(eps));
}

}  // namespace changji::infer
