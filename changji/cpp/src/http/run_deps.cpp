// 把 RunDeps 接到真实世界上。**单独一个文件，不进单元测试目标。**
//
// 理由和 llm/client_http.cpp 一样：这里要 include 那些带网络和大模型的
// 实现，而 run.cpp 里的队列、错误汇总、状态码全是纯逻辑，
// 得能在不链那些库的情况下测。
//
// 换句话说，这个文件里没有任何值得测的判断——它只是把
// "配置说走哪条路" 翻译成 "装哪两个函数对象"。
#include <algorithm>
#include <chrono>
#include <thread>
#include <memory>

#include "llm/client.hpp"
#include "media/ffmpeg.hpp"
#include "stages/tts_backends.hpp"
#include "config/runtime.hpp"
#include "http/run.hpp"
#include "infer/sd_image.hpp"
#include "infer/sd_video.hpp"
#include "infer/node_pick.hpp"
#include "infer/node_registry.hpp"
#include "infer/task_run.hpp"
#include "infer/worker_farm.hpp"
#include "infer/worker_pool.hpp"
#include "stages/frames.hpp"

namespace changji::http {

using models::ProjectStore;

RunDeps default_run_deps() {
    RunDeps d;
    d.settings = [] { return config::runtime().snapshot(); };
    d.profile = [] { return config::runtime().profile(); };
    // 第二个形参（项目目录）**故意不接名字**：RunDeps 的签名要求它在，而
    // 这一套后端一个字都没用到——`s` 已经是这一集自己的设置了（出片那条路
    // 每跑一集都 load_settings(项目目录) 重读）。接了名字不用，-Wall 每次
    // 编译报一条 unused-parameter。
    d.backends = [](const config::Settings& s, const ProjectStore&) {
        pipeline::Backends b;
        // 默认这一套：进程内 sd.cpp。
        // 两边都拿 `s`：这是**这一集**的设置（出片那条路每跑一集
        // 都 load_settings(项目目录) 重读），采样旋钮要跟着它走。
        b.frame = stages::sd_renderer(s);
        b.video = infer::sd_video_renderer(s);
        b.frame_backend_name = "sd.cpp";

        // **本机多卡：自己把每张卡的工作进程拉起来。**
        // 用户启动的仍然是一个命令，多卡编排由它自己做。只在 endpoints
        // 为空、探到多于一张卡时才动手（见 WorkerFarm::start）。
        //
        // farm 是静态的：拉起来的子进程要活到进程结束，每次建 Backends
        // 都拉一遍的话，跑第二集时会再起 N 个、端口还撞上。
        static std::shared_ptr<infer::WorkerFarm> farm =
            infer::WorkerFarm::start(
                s, config::runtime().profile(),
                [](const std::string& base, int seconds) {
                    // 工作进程的 /health 对 GET 和 POST 都答，
                    // 为一次探活单独引一条 HTTP 路径不值得。
                    auto post = llm::default_http_post();
                    const auto deadline =
                        std::chrono::steady_clock::now() +
                        std::chrono::seconds(seconds);
                    while (std::chrono::steady_clock::now() < deadline) {
                        if (post(base + "/health", "", {}, 2.0).status == 200) {
                            return true;
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(400));
                    }
                    return false;
                });
        const std::vector<std::string> farm_eps =
            farm ? farm->endpoints() : s.workers.endpoints;

        // 别的机器：按能力挑。**出图和出片要分开挑**——一台只装了出图
        // 模型的机器该参与首帧、不该参与出片，而以前那个池是"所有
        // 工作进程都能干所有活"。
        const auto nodes = infer::node_registry().snapshot(s);

        // 本机进程内跑一个任务。**做成回调注入**，池那一层不该知道
        // 配置长什么样（同 WorkerFarm::HealthProbe）。
        const infer::LocalRunner local_runner =
            [s](const infer::Task& t, const infer::StepCallback& on_step,
                pipeline::CancelToken& tok) {
                return infer::run_task_locally(t, s, infer::Origin::Local,
                                               infer::kLocalEndpoint, on_step,
                                               tok);
            };

        const auto eps_for = [&](infer::Capability cap) {
            std::vector<std::string> out;
            // 本机这一档：多卡时是自己拉起的那几个子进程，单卡时是
            // 进程内那个槽。**两者不叠加**——多卡时进程内不该再跑，
            // 那几个子进程已经把卡占满了。
            if (!farm_eps.empty()) {
                out = farm_eps;
            } else {
                for (const auto* n : infer::candidates_for(nodes, cap)) {
                    if (n->url == infer::kLocalEndpoint) {
                        out.push_back(n->url);
                        break;
                    }
                }
            }
            // 别的机器
            for (const auto* n : infer::candidates_for(nodes, cap)) {
                if (n->url != infer::kLocalEndpoint) out.push_back(n->url);
            }
            return out;
        };

        // **只有本机一个槽就别绕池了。** 那种情况下走池是纯粹多一层
        // 间接：一样的种子、一样的进程内 sd.cpp，只是中间过一遍任务的
        // 序列化和路径往返。单卡单机是最常见的用法，那条路上的行为
        // 应该和以前逐字节一样，不给自己留一个"绕了一圈才发现哪儿不同"
        // 的机会。
        const auto worth_pooling = [](const std::vector<std::string>& eps) {
            if (eps.empty()) return false;
            if (eps.size() == 1 && eps.front() == infer::kLocalEndpoint) {
                return false;
            }
            return true;
        };

        // **一个池只管一个能力。** 口令带上：本机自己拉起的那些听回环、
        // 不查，跨机那头要。
        const auto frame_eps = eps_for(infer::Capability::Frame);
        const auto video_eps = eps_for(infer::Capability::Video);
        auto frame_pool =
            worth_pooling(frame_eps)
                ? infer::make_worker_pool(frame_eps, s.peer.token, local_runner)
                : nullptr;
        auto video_pool =
            worth_pooling(video_eps)
                ? infer::make_worker_pool(video_eps, s.peer.token, local_runner)
                : nullptr;

        if (frame_pool) {
            b.frame = frame_pool->frame_renderer();
            b.keepalive.push_back(frame_pool);
        }
        if (video_pool) {
            b.video = video_pool->video_renderer();
            b.keepalive.push_back(video_pool);
        }
        if (frame_pool || video_pool) {
            const std::size_t n =
                std::max(frame_pool ? frame_pool->size() : 0,
                         video_pool ? video_pool->size() : 0);
            b.frame_backend_name = "sd.cpp（" + std::to_string(n) + " 处算力）";
            // **并发上限取大的那个。** 两个池不一样大时，小的那一阶段
            // 靠池自己挡住（借不到就等），而把上限压到小的那个会让
            // 大的那一阶段白白少跑几路。
            b.render_lanes = static_cast<int>(n);
            if (farm) b.keepalive.push_back(farm);
        }

        // 装配和闸门要用。路径从配置来——用户可能把 ffmpeg 装在
        // 非 PATH 的地方，那时候 assembly.ffmpeg_path 是唯一的出路。
        b.ffmpeg = media::FFmpeg(s.assembly.ffmpeg_path, s.assembly.ffprobe_path,
                                 media::default_runner());

        // 配音后端。**搭法只有一份**（infer::make_tts_backend），因为别的
        // 机器派配音任务过来时走的也是它——两份的话，"这台配音到底走哪条
        // 路"迟早在两边不一样，表现是同一集里前半段有声、后半段静音。
        //
        // 任何一条路搭不起来都退回估算后端（留空即是），不抛：配音只是
        // 五个阶段之一，为它整条流水线跑不起来不值当，而估算后端会写出
        // 等长静音，画面那几步照样能验。真出不了声这件事在配音阶段的
        // start 事件里说清楚了，不会跑完一整集才发现。
        b.tts = infer::make_tts_backend(s, b.ffmpeg);

        // 配音也能派给别的机器：那张表上它是一列，能勾就得能派。
        //
        // **本机自己配得了就不派**：配一句才十几秒，为它跨机搬一趟音频
        // 不划算。本机配不了（没模型、或者这份二进制没编 llama.cpp）时
        // 才去找别人——而那正是以前只能改配置指到一个固定地址的情况。
        if (!b.tts.has_value()) {
            std::vector<std::string> remote_tts;
            for (const auto& u : eps_for(infer::Capability::Tts)) {
                if (u != infer::kLocalEndpoint) remote_tts.push_back(u);
            }
            if (auto tts_pool = infer::make_worker_pool(
                    remote_tts, s.peer.token, local_runner)) {
                b.tts = stages::TTSBackend{"peer", tts_pool->tts_synthesizer(),
                                           {}};
                b.keepalive.push_back(tts_pool);
            }
        }
        return b;
    };
    return d;
}

}  // namespace changji::http
