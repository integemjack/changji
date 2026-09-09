// 把 RunDeps 接到真实世界上。**单独一个文件，不进单元测试目标。**
//
// 理由和 llm/client_http.cpp 一样：这里要 include 那些带网络和大模型的
// 实现，而 run.cpp 里的队列、错误汇总、状态码全是纯逻辑，
// 得能在不链那些库的情况下测。
//
// 换句话说，这个文件里没有任何值得测的判断——它只是把
// "配置说走哪条路" 翻译成 "装哪两个函数对象"。
#include <memory>

#include "llm/client.hpp"
#include "media/ffmpeg.hpp"
#include "stages/tts_backends.hpp"
#include "config/runtime.hpp"
#include "http/run.hpp"
#include "infer/sd_image.hpp"
#include "infer/sd_video.hpp"
#include "infer/worker_pool.hpp"
#include "stages/frames.hpp"

namespace changji::http {

using models::ProjectStore;

RunDeps default_run_deps() {
    RunDeps d;
    d.settings = [] { return config::runtime().snapshot(); };
    d.profile = [] { return config::runtime().profile(); };
    d.backends = [](const config::Settings& s, const ProjectStore& store) {
        pipeline::Backends b;
        // 默认这一套：进程内 sd.cpp。
        b.frame = stages::sd_renderer();
        b.video = infer::sd_video_renderer(s);
        b.frame_backend_name = "sd.cpp";

        // **配了工作进程就派出去算。** 空的话上面那两行原样生效——
        // 行为和以前一模一样，这是这一步能安全落地的前提。
        if (auto pool = infer::make_worker_pool(s.workers.endpoints)) {
            b.frame = pool->frame_renderer();
            b.video = pool->video_renderer();
            b.frame_backend_name =
                "sd.cpp（" + std::to_string(pool->size()) + " 个工作进程）";
            // **并发上限就是池的大小。** 没有池时保持 1——
            // 进程内不能并发（sd.cpp 的进度回调是全局的）。
            b.render_lanes = static_cast<int>(pool->size());
            // 池要活到渲染结束。Backends 只存 std::function，
            // 捕获一份 shared_ptr 让它跟着活。
            b.keepalive.push_back(pool);
        }

        // 装配和闸门要用。路径从配置来——用户可能把 ffmpeg 装在
        // 非 PATH 的地方，那时候 assembly.ffmpeg_path 是唯一的出路。
        b.ffmpeg = media::FFmpeg(s.assembly.ffmpeg_path, s.assembly.ffprobe_path,
                                 media::default_runner());

        // 配音后端按 [tts].backend 选。
        //
        // **任何一条路搭不起来都退回估算后端，不抛。** 配音只是五个阶段
        // 之一，为它整条流水线跑不起来不值得——而且估算后端会写出等长
        // 静音，画面那几步照样能验。真出不了声这件事在配音阶段的
        // start 事件里说清楚了，不会跑完一整集才发现。
        b.tts.reset();
        if (s.tts.backend == "http" && s.tts.base_url.has_value() &&
            !s.tts.base_url->empty()) {
            b.tts = stages::http_tts_backend(*s.tts.base_url, 300.0,
                                             llm::default_http_post(), b.ffmpeg);
        } else if (s.tts.backend == "local") {
            // 进程内配音。模型路径在 [models] 里——那一节本来就是
            // C++ 侧独有的，C++ 独有的键集中在一处。
            std::string why;
            const auto ws = s.workspace_path();
            auto local = stages::local_tts_backend(
                s.models.resolve(s.models.tts, ws),
                s.models.resolve(s.models.tts_decoder, ws),
                /*use_gpu=*/true, b.ffmpeg, why);
            if (local.has_value()) b.tts = std::move(*local);
            // 载不起来就退回估算后端，和另外两条路一样。
            //
            // **不在这里往哪儿写一行日志**：这个文件没有日志设施，
            // 为一条错误现造一个不合适。用户看得见的地方有两处，
            // 都已经覆盖：配音阶段的 start 事件里会报后端名字
            // （退回了就是 estimate），以及 /api/doctor 的"进程内配音"
            // 那一项——它查的就是这两个模型路径。
        }
        return b;
    };
    return d;
}

}  // namespace changji::http
