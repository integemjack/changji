#include "infer/worker_server.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <optional>
#include <map>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <crow.h>

#include "config/runtime.hpp"
#include "http/setup_api.hpp"
#include "infer/blob.hpp"
#include "infer/node_status.hpp"
#include "infer/peer_auth.hpp"
#include "setup/downloader.hpp"
#include "infer/task_run.hpp"
#include "infer/scheduler.hpp"
#include "infer/sd_backend.hpp"
#include "infer/sd_image.hpp"
#include "infer/worker_proto.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

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

crow::response json_res(const json& body, int code = 200) {
    // dump 用 replace 不用默认的 strict，理由同 http/server.cpp 的
    // `json_response`：这里回的 body 里带着 sd.cpp 抛上来的那句错误原文，
    // 而那是一个原生库拼出来的字符串——夹一个非法字节进去，strict 就在这
    // 一行抛，整条 /task 变成一个没有 body 的 500，主进程那头只看得到
    // 「这一镜失败了」，真正的原因反而丢了。合法输入逐字节不变。
    crow::response res(code, body.dump(-1, ' ', false,
                                       json::error_handler_t::replace));
    res.set_header("Content-Type", "application/json; charset=utf-8");
    return res;
}

}  // namespace

bool run_worker(const config::Settings& settings, const WorkerOptions& opts) {
    // **先看这个地址开不开得起。** 对外监听而没设口令的话当场拒绝——
    // 那种情况下谁都能派活过来烧这张卡、读走这台有哪些模型。
    // 理由和判据在 peer_auth.hpp。
    if (const auto why = refuse_to_listen(opts.host, settings.peer.token);
        !why.empty()) {
        std::cerr << why << std::endl;
        return false;
    }

    // **绑卡靠 CUDA_VISIBLE_DEVICES。** 在建任何 ggml 上下文之前设，
    // 之后再设没用——后端初始化的时候就把设备列表读走了。
    //
    // ⚠️ 这一条在只有一张卡的机器上验不了。要是它对 ggml 的 CUDA 后端
    // 不生效，表现是**八个进程全挤在卡 0 上，看着在并行实际在排队，
    // 而且一声不吭**。上多卡机器第一件事就是验它。
    paths::set_env("CUDA_VISIBLE_DEVICES", std::to_string(opts.gpu));

    // 和主进程一样注册两个槽。**每个工作进程一份**——
    // 进程边界把"每卡一份预算"白送了，Scheduler 一行没改。
    // **把 sd.cpp 的日志接到 stderr。** 不接的话一条都不会落地，而出图失败
    // 时抛的是"看一眼上面 sd.cpp 打的日志"——上面什么都没有。工作进程是
    // 独立进程，它的 stderr 就是排查出图问题唯一的地方。
    sd_log_to_stderr();

    // **只探一次。** detect 会跑 nvidia-smi，一百毫秒上下；
    // /status 每次重探的话，别的机器轮询一下就是白白拖慢这台。
    const auto profile =
        models::HardwareProfile::detect(settings.vram_gb_override);

    // **把启动时这一份装进 runtime，工作进程里原来没人做这件事。**
    //
    // `config::runtime()` 是个单例，完整服务在 server.cpp 起来时会
    // `replace(settings)`，工作进程这条路一处都没有——于是它一直是默认
    // 构造的空配置。后果有两层，都指向同一个症状：
    //
    // 一，从界面把模型下到这台机器上，下完那一下 `on_item_done` 会调
    //     setup_api 的 `persist()`。那个函数写文件之外还要
    //     `runtime().snapshot()` → 打补丁 → `replace()`，而它拿到的是空配置，
    //     于是内存里那份变成"默认值 + 这一组"，别的键全丢了。
    // 二，下面 `/status` 和出图槽读的都是**启动那一刻**的拷贝，配置文件
    //     后来写对了也看不见。
    //
    // 实测（2026-09-15）：Qwen-Image-Edit 20 GB 下完、config.toml 里
    // `image` 也写上了、文件就在盘上，这台仍然一直报「[models].image
    // 没配，或者文件不在」，派活那头永远不会把首帧派过来——除非重启它。
    // 而整个「在界面上给远程机器装模型」就是为了不用去碰那台机器。
    config::runtime().replace(settings);
    // 上一轮被杀时孤儿 curl 下全的 `.part` 收编进来，见 adopt_finished_parts。
    setup::adopt_finished_parts(settings.models.dir_path(settings.workspace_path()));

    // 槽也读活的那一份，理由同上：下完模型不重启就该能用。
    register_sd_slots([] { return config::runtime().snapshot(); }, profile);

    auto state = std::make_shared<State>();
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    // 连接计时器放宽到 60 秒：一段 blob 在 10 KB/s 的链路上也要二十多秒，
    // 默认 5 秒会把它切断（见上面 /blob 那条）。
    app.timeout(60);
    // 几个 io_service：一条慢连接（往外发 blob、收 blob）只占住一个，
    // 别的连接上的 /task、轮询照常有人接。
    app.concurrency(4);

    // **这台的自我介绍。** 别的机器靠它决定派不派活过来：能力齐不齐、
    // 卡多大、模型目录还剩多少。拼的地方只有一处（node_status.cpp），
    // 界面上那张表和 --doctor 末尾那句用的是同一份。
    // 对外监听时，除了 /health 都要口令。
    //
    // **/health 故意不要**：它只回 ok/gpu/busy，探活的那一头（可能是
    // 负载均衡、可能是脚本）不该为了 ping 一下就拿到口令。
    const auto gate = [settings, opts](const crow::request& req)
        -> std::optional<crow::response> {
        if (!is_public_bind(opts.host)) return std::nullopt;
        if (token_ok(req.get_header_value("Authorization"),
                     settings.peer.token)) {
            return std::nullopt;
        }
        return json_res({{"detail",
                          "口令不对或者没带。要 Authorization: Bearer "
                          "<对面 [peer].token 那个值>"}},
                        401);
    };

    // **读活的那一份，不是启动时捕获的拷贝。** 见上面 runtime().replace
    // 那段：这台自己下完模型之后，这份自我介绍必须跟着变，否则派活那头
    // 看到的永远是"干不了"。
    CROW_ROUTE(app, "/status")([profile, gate](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        return json_res(
            node_status_json(config::runtime().snapshot(), profile));
    });

    CROW_ROUTE(app, "/health")([opts, state] {
        std::lock_guard lg(state->mu);
        return json_res({{"ok", true},
                         {"gpu", opts.gpu},
                         {"busy", state->current != nullptr}});
    });

    // ---- 装模型：让派活那头能指挥这台去补齐 ----
    //
    // **转调初始化页那套**（http/setup_api），不另写一份：清单、推荐档、
    // aria2/curl、断点续传、按真实字节数判完成，全在那儿了。这台机器
    // 自己打开界面点下载，和别的机器指挥它下载，走的必须是同一条路——
    // 两份的话，"下完了没有"的判据迟早只改一边。
    const auto api_res = [](const http::ApiResult& r) {
        return json_res(r.body, r.status);
    };

    CROW_ROUTE(app, "/setup/state")(
        [gate, settings, profile, api_res](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        return api_res(http::get_setup_state(settings, profile));
    });

    CROW_ROUTE(app, "/setup/download").methods(crow::HTTPMethod::POST)(
        [gate, settings, api_res](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            return json_res({{"detail", "请求体不是 JSON"}}, 400);
        }
        try {
            return api_res(http::post_setup_download(settings, body));
        } catch (const http::ApiError& e) {
            return json_res({{"detail", e.detail()}}, e.status());
        }
    });

    CROW_ROUTE(app, "/setup/progress")(
        [gate, api_res](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        return api_res(http::get_setup_progress());
    });

    CROW_ROUTE(app, "/setup/cancel").methods(crow::HTTPMethod::POST)(
        [gate, api_res](const crow::request& req) {
        if (auto deny = gate(req)) return std::move(*deny);
        return api_res(http::post_setup_cancel());
    });

    // ---- blob：跨机时输入和产物都走这三条 ----
    //
    // **为什么不把文件塞进任务的 JSON 里。** 一张参考图几 MB，base64 之后
    // 还要涨三分之一，而一集里那几张图是同一批文件——塞进去就是同一张脸
    // 传二十二遍。分开之后，第二镜起 probe 一问就跳过了。
    const auto cache = infer::cache_root_of(settings.workspace_path());

    CROW_ROUTE(app, "/blob/<string>/probe")(
        [gate, cache](const crow::request& req, const std::string& id) {
        if (auto deny = gate(req)) return std::move(*deny);
        // 派活那头靠这一句决定传不传。**不合法的指纹回 have:false 就够**
        // ——它本来也不可能存在，而当成错误会让派活方以为链路坏了。
        return json_res({{"have", blob_present(cache, id)}});
    });

    CROW_ROUTE(app, "/blob/<string>").methods(crow::HTTPMethod::POST)(
        [gate, cache](const crow::request& req, const std::string& id) {
        if (auto deny = gate(req)) return std::move(*deny);
        // 先核指纹再落地，对不上不写——截断的那份是最阴的故障，
        // 见 blob.hpp。
        if (const auto why = blob_store(cache, id, req.body); !why.empty()) {
            return json_res({{"detail", why}}, 400);
        }
        return json_res({{"ok", true}});
    });

    CROW_ROUTE(app, "/blob/<string>")(
        [gate, cache](const crow::request& req, const std::string& id) {
        if (auto deny = gate(req)) return std::move(*deny);
        const auto p = blob_path(cache, id);
        std::error_code ec;
        if (p.empty() || !std::filesystem::is_regular_file(p, ec)) {
            return json_res({{"detail", "没有这个 blob：" + id}}, 404);
        }
        // **按段给，不一口气发整个文件。** 跨境公网实测 20～30 KB/s：
        //   · 一次发整个：Crow 对 1 MB 以下的 body 是异步写完就开连接
        //     计时器（默认 5 秒），字节没发完计时器先到，对面收到半截
        //     （curl 每次都在 33 万字节处断，2026-09-15 实撞）；
        //   · 走静态文件那条路是**同步**分块写，一条连接把整个
        //     io_service 堵 30 秒，落在同一个 io_service 上的 /task、
        //     轮询全等着——下一镜派不出去，显卡白闲 25 秒。
        // 派活方按 off/len 一段一段地取（见 worker_pool.cpp 的
        // pull_artifact），每段几秒钟就完，走异步写、不堵别人；
        // 不带 off/len 的老客户端照样拿整份。
        std::error_code ec2;
        const auto total = static_cast<std::uint64_t>(std::filesystem::file_size(p, ec2));
        std::uint64_t off = 0;
        std::uint64_t len = total;
        if (const char* o = req.url_params.get("off")) off = std::strtoull(o, nullptr, 10);
        if (const char* l = req.url_params.get("len")) len = std::strtoull(l, nullptr, 10);
        if (off > total) return json_res({{"detail", "off 超出文件"}}, 416);
        len = std::min(len, total - off);
        std::ifstream in(p, std::ios::binary);
        if (!in) return json_res({{"detail", "读不了：" + id}}, 500);
        std::string body(static_cast<std::size_t>(len), '\0');
        in.seekg(static_cast<std::streamoff>(off));
        in.read(body.data(), static_cast<std::streamsize>(len));
        body.resize(static_cast<std::size_t>(in.gcount()));
        crow::response res(200, std::move(body));
        res.set_header("Content-Type", "application/octet-stream");
        res.set_header("X-Blob-Size", std::to_string(total));
        return res;
    });

    CROW_ROUTE(app, "/task").methods(crow::HTTPMethod::POST)(
        [state, settings, gate](const crow::request& req) {
            if (auto deny = gate(req)) return std::move(*deny);
            // **先看这串字节是不是合法 UTF-8。** 不是的话下面每一条
            // 路都会炸在同一个地方：nlohmann 解析时照单全收，而把出错
            // 位置附近的原始字节拼进 {"detail": …} 再 dump，就在报错的
            // 路上又抛一次——第二次没人接，派活方拿到一个空白的 500。
            // 2026-09-12 实撞，日志里只有一行 invalid UTF-8 byte。
            if (!text::is_valid_utf8(req.body)) {
                return json_res(
                    {{"detail",
                      "请求体不是合法的 UTF-8。派活那头多半没按 UTF-8 编码"
                      "（Windows 上直接发 GBK 的中文就会这样）"}},
                    400);
            }
            Task task;
            try {
                task = task_from_json(json::parse(req.body));
            } catch (const std::exception& e) {
                // e.what() 里可能带着原始字节，洗一遍再放进 JSON
                return json_res({{"detail", std::string("任务读不懂：") +
                                                text::sanitize_utf8(e.what())}},
                                400);
            }

            // **先自检再排队。** 干不成就当场说——这一条是烧过一次换来的。
            // 判据在 task_run.cpp，两条路（工作进程、对等互联）共用一份。
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
            // id 也捕一份：跨机时沙箱按它起名（<cache>/tasks/<id>）。
            std::thread([state, live, task, settings, id] {
                const auto on_step = [state, live](int step, int steps,
                                                  double, Phase phase) {
                    std::lock_guard lg(state->mu);
                    live->progress.step = step;
                    live->progress.steps = steps;
                    live->progress.phase = phase_name(phase);
                };
                // 采样中途的预览存进进度里，派活方轮询时按需带走
                // （见 TaskProgress::preview）。只认这件活自己的 tag。
                const PreviewSinkHandle preview_sink(
                    [state, live, tag = task.shot_id](const std::string& t, int step,
                                                      std::string url) {
                        if (t != tag) return;
                        std::lock_guard lg(state->mu);
                        live->progress.preview_step = step;
                        live->progress.preview = std::move(url);
                    });
                // 怎么跑在 task_run.cpp 里，那一层不碰网络。
                // **origin 是 Local**：这些工作进程是本机自己按显卡数
                // 拉起来的（见 worker_farm.hpp），它们干的就是本机的活。
                // 别的机器派来的活走对等互联那条路，那边传 Peer。
                //
                // （合并时这儿原来是一整段就地跑的代码，包括那句
                //  `sd_renderer_with_seed(settings, task.seed)`——采样旋钮
                //  要跟着这一集的 settings 走。搬进 run_task_locally 之后
                //  那个参数还在，见 task_run.cpp 里那一行。）
                const TaskResult result = run_task_locally(
                    task, settings, Origin::Local, id, on_step, live->tok);
                std::lock_guard lg(state->mu);
                live->progress.state = result.ok ? "done" : "failed";
                live->progress.result = result;
            }).detach();

            state->current = live;
            state->current_id = id;
            return json_res({{"id", id}}, 202);
        });

    CROW_ROUTE(app, "/task/<string>")(
        [state, gate](const crow::request& req, const std::string& id) {
        if (auto deny = gate(req)) return std::move(*deny);
        std::lock_guard lg(state->mu);
        if (!state->current || state->current_id != id) {
            return json_res({{"detail", "没有这个任务"}}, 404);
        }
        auto p = state->current->progress;
        if (p.state == "done" || p.state == "failed") {
            // 收完就放，好接下一个
            state->current.reset();
            state->current_id.clear();
        }
        // 预览只在问了、而且比它手里那张新时才带（几十 KB 一张，见
        // TaskProgress::preview）。没问的轮询一个字节都不多。
        const char* after = req.url_params.get("preview_after");
        if (!after || p.preview_step <= std::atoi(after)) p.preview.clear();
        return json_res(to_json(p));
    });

    CROW_ROUTE(app, "/task/<string>/cancel")
        .methods(crow::HTTPMethod::POST)(
            [state, gate](const crow::request& req, const std::string& id) {
            if (auto deny = gate(req)) return std::move(*deny);
            std::lock_guard lg(state->mu);
            if (!state->current || state->current_id != id) {
                return json_res({{"detail", "没有这个任务"}}, 404);
            }
            state->current->tok.request();
            return json_res({{"ok", true}});
        });

    CROW_LOG_INFO << "工作进程 gpu=" << opts.gpu << " 听 " << opts.host << ":"
                  << opts.port;
    // **端口还被上一个进程占着就等它，别当场退出。** 重启工作进程时上一个
    // 可能正在出片、几秒才退干净；这时 bind 报 Address already in use，新的
    // 一退，机器上就没有工作进程了，派活方那头整台变灰（2026-09-16 实撞）。
    // 等最多一分钟，每两秒试一次；别的错照样抛。
    for (int attempt = 1;; ++attempt) {
        try {
            app.bindaddr(opts.host).port(static_cast<std::uint16_t>(opts.port)).run();
            break;
        } catch (const std::exception& e) {
            const std::string what = e.what();
            if (what.find("Address already in use") == std::string::npos || attempt >= 30) {
                throw;
            }
            CROW_LOG_WARNING << "端口 " << opts.port << " 还被占着（多半是上一个"
                             << "工作进程还没退干净），两秒后再试（" << attempt << "/30）";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

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
    return true;   // 到不了，但签名要它
}

}  // namespace changji::infer
