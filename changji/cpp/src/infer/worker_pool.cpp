#include "infer/worker_pool.hpp"

#include "infer/worker_roster.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <set>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "infer/blob.hpp"
#include <cstdint>
#include <cstdlib>

#include "infer/peer_auth.hpp"
#include "infer/worker_proto.hpp"
#include "util/httplib.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

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

/// 读一个文件。读不了抛——**别拿空内容接着跑**：那样传过去的是一个
/// 指纹对得上的空文件，对面照样"成功"，直到出图那步才发现参考图是空的。
std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("读不了输入文件：" + paths::to_utf8(p));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

struct WorkerPool::Impl {
    struct Worker {
        WorkerEndpoint ep;
    };

    std::vector<Worker> workers;
    /// 本机那个槽怎么跑。空 = 池里没有本机这一档。
    LocalRunner local_runner;
    /// 这部剧挑的档位。每个派出去的任务都盖上，见 WorkerPool 的构造函数。
    std::map<std::string, std::string> pick;
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
    /// 把一个输入文件送到对面去，回它的 `blob:` 记法。
    ///
    /// **先问再传。** 一集 22 镜、每镜三五张参考图，而那几张是同一批文件；
    /// 不问的话就是同一张脸传二十二遍。
    std::string ship_input(httplib::Client& cli, const std::string& prefix,
                           const std::string& url, const std::string& path) {
        const std::string bytes = read_file(paths::from_utf8(path));
        const std::string id = text::sha1_hex(bytes);

        auto probe = cli.Get(prefix + "/blob/" + id + "/probe");
        if (!probe) {
            throw Unreachable("问不到工作进程 " + url + "：" +
                              httplib::to_string(probe.error()));
        }
        bool have = false;
        if (probe->status == 200) {
            const auto j = json::parse(probe->body, nullptr, false);
            have = !j.is_discarded() && j.value("have", false);
        }
        if (!have) {
            auto res = cli.Post(prefix + "/blob/" + id, bytes,
                                "application/octet-stream");
            if (!res) {
                throw Unreachable("传不过去 " + url + "：" +
                                  httplib::to_string(res.error()));
            }
            if (res->status != 200) {
                throw std::runtime_error(
                    worker_rejected_message(res->status, res->body));
            }
        }
        return "blob:" + id;
    }

    /// 把产物取回来，落到本机的 dest。
    void pull_artifact(httplib::Client& cli, const std::string& prefix,
                       const std::string& url, const std::string& artifact_id,
                       const std::string& dest) {
        if (artifact_id.empty()) {
            throw std::runtime_error(
                "对面说跑成了，却没给产物指纹——多半是那台的版本还不认 "
                "return_artifact");
        }
        // **一段一段地取。** 跨境公网实测 20～30 KB/s，一张首帧 650 KB
        // 要二三十秒、一段 mp4 上百秒。整个一次取，对面要么被连接计时器
        // 切断，要么同步发送把它的 io_service 堵住、下一镜派不出去
        // （见 worker_server.cpp 的 /blob）。每段 256 KB，慢链路上也就
        // 十几秒；对面回的 X-Blob-Size 说总共多大。
        constexpr std::uint64_t kChunk = 256 * 1024;
        std::string body;
        std::uint64_t total = 0;
        for (std::uint64_t off = 0;;) {
            auto res = cli.Get(prefix + "/blob/" + artifact_id +
                               "?off=" + std::to_string(off) +
                               "&len=" + std::to_string(kChunk));
            if (!res) {
                throw Unreachable("取不回产物 " + url + "：" +
                                  httplib::to_string(res.error()));
            }
            if (res->status != 200) {
                throw std::runtime_error(
                    worker_rejected_message(res->status, res->body));
            }
            if (res->has_header("X-Blob-Size")) {
                total = std::strtoull(res->get_header_value("X-Blob-Size").c_str(),
                                      nullptr, 10);
            } else {
                // 老版本的工作进程不认 off/len，回的就是整份。
                total = res->body.size();
            }
            body += res->body;
            off += res->body.size();
            if (off >= total || res->body.empty()) break;
        }
        // **落地之前核一遍指纹。** 少几个字节的 png 照样能写下去，
        // 之后报的是一张半截图或者"权重读不对"，指向完全错误的方向。
        const std::string real = text::sha1_hex(body);
        if (real != artifact_id) {
            throw std::runtime_error("产物传坏了：说好的是 " + artifact_id +
                                     "，收到的是 " + real + "（" +
                                     std::to_string(body.size()) +
                                     " 字节）");
        }
        const auto out = paths::from_utf8(dest);
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("写不了产物：" + dest);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f) throw std::runtime_error("产物写坏了：" + dest);
    }

    /// 跑完回结果。**要这个返回值是为了配音**：出来多长（秒）只有跑活
    /// 那台知道（它顺手就量了），而配音先行那条线靠它反推镜头时长。
    /// `before_pull` 在对面报"跑完了"、开始取产物之前调一次：把槽先还
    /// 回去，好让下一镜马上派出去，取产物那几十秒和它的采样叠着走。
    TaskResult run_on(std::size_t idx, const Task& task,
                      pipeline::CancelToken& tok,
                      const StepCallback& on_step,
                      const std::function<void()>& before_pull) {
        // 本机那个槽：进程内跑，不发 HTTP，也不搬文件（同一个文件系统）。
        // 排队由执行位管（见 exec_queue.hpp），这儿不用再判忙不忙。
        if (workers[idx].ep.url == kLocalEndpoint) {
            if (!local_runner) {
                throw Unreachable("池里有个 local 槽，却没给怎么在本机跑");
            }
            const TaskResult r = local_runner(task, on_step, tok);
            if (!r.ok) throw std::runtime_error(r.error);
            return r;
        }

        const auto [origin, prefix] = split_url(workers[idx].ep.url);
        httplib::Client cli(origin);
        // 跨机那头要口令，本机那些听回环的不查——带上都不碍事。
        if (!workers[idx].ep.token.empty()) {
            cli.set_bearer_token_auth(workers[idx].ep.token);
        }
        cli.set_connection_timeout(10, 0);
        cli.set_read_timeout(600, 0);

        // **同机就什么都不搬。** 一个文件系统，参考图直接给路径、
        // 产物直接写过去。跨机才走 blob：那边根本没有这些目录。
        const bool remote = !endpoint_is_local(workers[idx].ep.url);
        Task t = task;
        if (remote) {
            t.return_artifact = true;
            for (auto& r : t.prompts.reference_images) {
                r = ship_input(cli, prefix, workers[idx].ep.url, r);
            }
            if (t.start_image) {
                t.start_image =
                    ship_input(cli, prefix, workers[idx].ep.url, *t.start_image);
            }
            if (t.end_image) {
                t.end_image =
                    ship_input(cli, prefix, workers[idx].ep.url, *t.end_image);
            }
        }

        auto res = cli.Post(prefix + "/task", to_json(t).dump(),
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
        std::string last_phase;
        constexpr int kPollMisses = 12;   // 12 × 5 秒 = 一分钟
        int misses = 0;
        // 预览：最多每 3 秒要一张。一张几十 KB，跨境链路 25 KB/s，每次轮询
        // 都要会把取产物的带宽吃光。只要比手里那张新的。
        int last_preview = -1;
        auto last_preview_at = std::chrono::steady_clock::now() - std::chrono::seconds(3);
        for (;;) {
            if (tok.cancelled()) {
                cli.Post(prefix + "/task/" + id + "/cancel", "",
                         "application/json");
                throw std::runtime_error("取消了");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            const bool want_preview =
                std::chrono::steady_clock::now() - last_preview_at >= std::chrono::seconds(3);
            auto st = cli.Get(prefix + "/task/" + id +
                              (want_preview ? "?preview_after=" + std::to_string(last_preview)
                                            : std::string{}));
            if (!st) {
                // **公网抖一下不算它死了。** 跨境链路实测隔几分钟就掉一次
                // 连接，而那边的活还在好好地跑（2026-09-15：一镜跑到第二
                // 段、GPU 100%，这边一句"断了"就把整镜判失败）。httplib
                // 下一次 Get 会自己重连，所以隔几秒再问；连着一分钟都问
                // 不到才算它真没了——那时候才是 Unreachable，这一镜换台
                // 机器从头跑，不记到镜头的重试次数上。
                if (++misses <= kPollMisses) {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }
                throw Unreachable("工作进程 " + workers[idx].ep.url + " 断了：" +
                                  httplib::to_string(st.error()));
            }
            misses = 0;
            const auto body = json::parse(st->body, nullptr, false);
            if (body.is_discarded()) {
                throw std::runtime_error("工作进程回的进度不是 JSON");
            }
            const auto p = task_progress_from_json(body);
            if (!p.preview.empty() && p.preview_step > last_preview) {
                // 和进程内采样走同一个口子，镜头墙上的小图就不用管活在哪台跑
                last_preview = p.preview_step;
                last_preview_at = std::chrono::steady_clock::now();
                publish_preview(task.shot_id, p.preview_step, p.preview);
            }
            const bool changed = p.step != last_step || p.steps != last_steps ||
                                 p.phase != last_phase;
            if (p.steps > 0 && on_step && changed) {
                on_step(p.step, p.steps, 0.0, phase_from(p.phase));
                last_step = p.step;
                last_steps = p.steps;
                last_phase = p.phase;
            }
            if (p.state == "done" || p.state == "failed") {
                if (!p.result) throw std::runtime_error("跑完了却没有结果");
                if (!p.result->ok) throw std::runtime_error(p.result->error);
                if (remote) {
                    // **对面已经空了，先把槽还回去再取产物。** 取一张
                    // 650 KB 的首帧在跨境公网上要二三十秒（30 KB/s），
                    // 而那台的显卡这期间闲着；先还槽，下一镜的采样就和
                    // 这一镜的传输叠在一起——L20 上一镜 47 秒变 24 秒。
                    // 取砸了照样抛，只是那时槽已经在别人手里，不影响。
                    before_pull();
                    pull_artifact(cli, prefix, workers[idx].ep.url,
                                  p.result->artifact_id, task.dest);
                }
                return *p.result;
            }
        }
    }

    /// 把一个任务跑完。**同步**——阶段那一层本来就在自己的线程里等。
    ///
    /// 连不上就换一个再试。**这不是镜头的重试**：镜头的 attempts 管的是
    /// "这一镜的画面不行，换个种子再来"，而一台机器崩了跟画面没关系。
    /// 8×L20 上真发生过：一个工作进程 OOM 崩了、systemd 正在重启它，
    /// 十一个镜头连着挑中它，每个 attempts 加到 3 直接降级——
    /// 而池子里另外七个好好的，一个都没被试过。
    TaskResult run_task(const Task& raw, pipeline::CancelToken& tok,
                        const StepCallback& on_step) {
        // **在这一个口子上盖档位**，不在三个 renderer 里各盖一遍：漏掉
        // 一个的表现是"出图用对了模型、出片用错了"，而两者出来都是能看
        // 的东西，没有哪一层会去比。
        Task task = raw;
        task.pick = pick;
        std::set<std::size_t> tried;
        std::string last_error;
        for (;;) {
            const auto got = take(tried);
            if (!got) {
                throw PoolUnreachable(
                    "池里每一个工作进程都连不上（试过 " +
                    std::to_string(tried.size()) + " 个）：" + last_error);
            }
            const std::size_t idx = *got;
            // 还槽只还一次：正常路上在取产物前就还了（见 run_on），
            // 抛出来的路上靠析构兜底。
            struct Release {
                Impl* self;
                std::size_t i;
                bool done = false;
                void now() {
                    if (done) return;
                    done = true;
                    self->give_back(i);
                }
                ~Release() { now(); }
            } release{this, idx};

            try {
                const TaskResult r =
                    run_on(idx, task, tok, on_step, [&] { release.now(); });
                mark_ok(idx);
                return r;
            } catch (const Unreachable& e) {
                mark_bad(idx);
                tried.insert(idx);
                last_error = e.what();
                // 换一个再来。**不往上抛**——抛上去就成了镜头的一次失败。
            }
        }
    }
};

WorkerPool::WorkerPool(std::vector<WorkerEndpoint> endpoints,
                       LocalRunner local_runner,
                       std::map<std::string, std::string> pick)
    : impl_(std::make_unique<Impl>()) {
    impl_->local_runner = std::move(local_runner);
    impl_->pick = std::move(pick);
    for (auto& e : endpoints) impl_->workers.push_back({std::move(e)});
    impl_->roster = std::make_unique<WorkerRoster>(impl_->workers.size());
}

WorkerPool::~WorkerPool() = default;

std::size_t WorkerPool::size() const { return impl_->workers.size(); }

std::size_t WorkerPool::lanes() const {
    std::size_t remote = 0;
    for (const auto& w : impl_->workers) {
        if (!endpoint_is_local(w.ep.url)) ++remote;
    }
    return pool_lanes(impl_->workers.size(), remote);
}

std::size_t WorkerPool::alive() const {
    std::size_t n = 0;
    for (const auto& w : impl_->workers) {
        // 本机那个槽不用 ping：它要么在，要么这个进程自己也没了。
        if (w.ep.url == kLocalEndpoint) {
            if (impl_->local_runner) ++n;
            continue;
        }
        const auto [origin, prefix] = split_url(w.ep.url);
        httplib::Client cli(origin);
        if (!w.ep.token.empty()) cli.set_bearer_token_auth(w.ep.token);
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
        // 发起方定死的（定妆图）优先，见 PromptBundle::seed_override。
        t.seed = prompts.seed_override
                     ? *prompts.seed_override
                     : stages::frame_seed(shot.shot_id, shot.attempts);
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
        if (plan.end_image) t.end_image = paths::to_utf8(*plan.end_image);
        t.keep_ambient = plan.keep_ambient;
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

stages::Synthesizer WorkerPool::tts_synthesizer() {
    Impl* impl = impl_.get();
    return [impl](const std::string& text, const std::filesystem::path& dest,
                  const std::optional<std::string>& voice_id,
                  const std::string& emotion, double intensity) {
        Task t;
        t.kind = TaskKind::Tts;
        // shot_id 在配音这条路上没有意义，但沙箱和日志都靠它认人，
        // 给一个固定的比空着强。
        t.shot_id = "tts";
        t.text = text;
        t.voice_id = voice_id.value_or("");
        t.emotion = emotion;
        t.intensity = intensity;
        t.dest = paths::to_utf8(dest);

        // **配音这条路上没有取消令牌。** Synthesizer 的签名里就没有——
        // 一句话十几秒，点了停止最多多等这么久，不值得为它把整条
        // 配音链路的签名都改一遍。
        pipeline::CancelToken tok;
        const TaskResult r = impl->run_task(t, tok, {});

        stages::SynthesisResult out;
        out.duration_s = r.duration_s;
        // 产物已经落到 dest 了（同机直接写，跨机拉回来）。
        out.audio_path = dest;
        return out;
    };
}

std::shared_ptr<WorkerPool> make_worker_pool(
    const std::vector<std::string>& endpoints, const std::string& token,
    LocalRunner local_runner, std::map<std::string, std::string> pick) {
    if (endpoints.empty()) return nullptr;
    std::vector<WorkerEndpoint> eps;
    eps.reserve(endpoints.size());
    for (const auto& u : endpoints) eps.push_back(WorkerEndpoint{u, token});
    return std::make_shared<WorkerPool>(std::move(eps), std::move(local_runner),
                                        std::move(pick));
}

std::shared_ptr<WorkerPool> make_worker_pool(
    std::vector<WorkerEndpoint> endpoints, LocalRunner local_runner,
    std::map<std::string, std::string> pick) {
    if (endpoints.empty()) return nullptr;
    return std::make_shared<WorkerPool>(
        std::move(endpoints), std::move(local_runner), std::move(pick));
}

}  // namespace changji::infer
