#include "infer/worker_pool.hpp"

#include "infer/worker_roster.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <set>
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
    };

    std::vector<Worker> workers;
    /// 谁忙着、谁坏了。策略在 worker_roster.hpp，那份不含网络代码、能测。
    std::unique_ptr<WorkerRoster> roster;
    std::mutex mu;
    std::condition_variable cv;

    /// 借一个能用的。没有就等——**不是失败**：
    /// 阶段那一层的并发上限就是靠这个卡住的。
    ///
    /// 返回空表示**每一个都试过了、都连不上**，那才轮到调用方把这一镜判失败。
    std::optional<std::size_t> take(const std::set<std::size_t>& skip = {}) {
        std::unique_lock lk(mu);
        if (skip.size() >= workers.size()) return std::nullopt;
        cv.wait(lk, [&] { return roster->worth_waiting(skip); });
        return roster->take(skip, std::chrono::steady_clock::now());
    }

    void mark_bad(std::size_t i) {
        std::lock_guard lg(mu);
        roster->mark_bad(i, std::chrono::steady_clock::now());
    }

    void mark_ok(std::size_t i) {
        std::lock_guard lg(mu);
        roster->mark_ok(i);
    }

    void give_back(std::size_t i) {
        {
            std::lock_guard lg(mu);
            roster->give_back(i);
        }
        cv.notify_one();
    }

    /// 把一个任务跑完。**同步**——阶段那一层本来就在自己的线程里等。
    /// 这个工作进程连不上。**和"这一镜渲染失败"是两回事**：
    /// 前者换台机器立刻就好，后者换台机器还是一样。
    struct Unreachable : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    /// 在指定的工作进程上把任务跑完。连不上抛 Unreachable，其余照旧。
    void run_on(std::size_t idx, const Task& task, pipeline::CancelToken& tok,
                const StepCallback& on_step) {
        const auto [origin, prefix] = split_url(workers[idx].ep.url);
        httplib::Client cli(origin);
        cli.set_connection_timeout(10, 0);
        cli.set_read_timeout(600, 0);

        auto res = cli.Post(prefix + "/task", to_json(task).dump(),
                            "application/json");
        if (!res) {
            throw Unreachable("连不上工作进程 " + workers[idx].ep.url + "：" +
                              httplib::to_string(res.error()));
        }
        if (res->status == 409) {
            throw std::runtime_error("工作进程 " + workers[idx].ep.url +
                                     " 正忙。**这不该发生**——"
                                     "池这边已经不派给忙着的了，"
                                     "多半是有别人也在用同一个工作进程");
        }
        if (res->status != 202 && res->status != 200) {
            // 这句话会进事件流再序列化成 JSON，body 得按字符截，
            // 拼法在 worker_proto 里（那儿能测）。
            throw std::runtime_error(
                worker_rejected_message(res->status, res->body));
        }
        const auto accepted = json::parse(res->body, nullptr, false);
        if (accepted.is_discarded() || !accepted.contains("id")) {
            throw std::runtime_error(worker_bad_accept_message(res->body));
        }
        const std::string id = accepted["id"].get<std::string>();

        // 轮询。**间隔别太短**：出一张图是几十秒到几分钟，
        // 每 100 毫秒问一次纯粹是给对面添乱。
        //
        // **进度没变就不往上报。** 以前每问一次就报一次，同一步在事件里
        // 出现三四遍：快照只留最后 200 条，几镜并行时全被这种重复填满，
        // 真正的 warn / gate 被挤出去；WebSocket 那头每秒收十几条一样的。
        int last_step = -1, last_steps = -1;
        bool last_loading = false;
        for (;;) {
            if (tok.cancelled()) {
                cli.Post(prefix + "/task/" + id + "/cancel", "",
                         "application/json");
                throw std::runtime_error("取消了");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            auto st = cli.Get(prefix + "/task/" + id);
            if (!st) {
                // 跑到一半断的。**也是 Unreachable**：这一镜在别的机器上
                // 从头跑一遍就行，不该记到镜头的重试次数上。
                throw Unreachable("工作进程 " + workers[idx].ep.url + " 断了：" +
                                  httplib::to_string(st.error()));
            }
            const auto body = json::parse(st->body, nullptr, false);
            if (body.is_discarded()) {
                throw std::runtime_error("工作进程回的进度不是 JSON");
            }
            const auto p = task_progress_from_json(body);
            const bool changed = p.step != last_step || p.steps != last_steps ||
                                 p.loading != last_loading;
            if (p.steps > 0 && on_step && changed) {
                on_step(p.step, p.steps, 0.0, p.loading);
                last_step = p.step;
                last_steps = p.steps;
                last_loading = p.loading;
            }
            if (p.state == "done" || p.state == "failed") {
                if (!p.result) throw std::runtime_error("跑完了却没有结果");
                if (!p.result->ok) throw std::runtime_error(p.result->error);
                return;
            }
        }
    }

    /// 把一个任务跑完。**同步**——阶段那一层本来就在自己的线程里等。
    ///
    /// 连不上就换一个再试。**这不是镜头的重试**：镜头的 attempts 管的是
    /// "这一镜的画面不行，换个种子再来"，而一台机器崩了跟画面没关系。
    /// 8×L20 上真发生过：一个工作进程 OOM 崩了、systemd 正在重启它，
    /// 十一个镜头连着挑中它，每个 attempts 加到 3 直接降级成静帧——
    /// 而池子里另外七个好好的，一个都没被试过。
    void run_task(const Task& task, pipeline::CancelToken& tok,
                  const StepCallback& on_step) {
        std::set<std::size_t> tried;
        std::string last_error;
        for (;;) {
            const auto got = take(tried);
            if (!got) {
                throw std::runtime_error(
                    "池里每一个工作进程都连不上（试过 " +
                    std::to_string(tried.size()) + " 个）：" + last_error);
            }
            const std::size_t idx = *got;
            struct Release {
                Impl* self;
                std::size_t i;
                ~Release() { self->give_back(i); }
            } release{this, idx};

            try {
                run_on(idx, task, tok, on_step);
                mark_ok(idx);
                return;
            } catch (const Unreachable& e) {
                mark_bad(idx);
                tried.insert(idx);
                last_error = e.what();
                // 换一个再来。**不往上抛**——抛上去就成了镜头的一次失败。
            }
        }
    }
};

WorkerPool::WorkerPool(std::vector<WorkerEndpoint> endpoints)
    : impl_(std::make_unique<Impl>()) {
    for (auto& e : endpoints) impl_->workers.push_back({std::move(e)});
    impl_->roster = std::make_unique<WorkerRoster>(impl_->workers.size());
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
