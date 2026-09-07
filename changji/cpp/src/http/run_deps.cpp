// 把 RunDeps 接到真实世界上。**单独一个文件，不进单元测试目标。**
//
// 理由和 llm/client_http.cpp 一样：这里要 include comfy/client_http 那条路
// （httplib + asio），而 run.cpp 里的队列、错误汇总、状态码全是纯逻辑，
// 得能在不链网络库的情况下测。
//
// 换句话说，这个文件里没有任何值得测的判断——它只是把
// "配置说走哪条路" 翻译成 "装哪两个函数对象"。

#include <memory>

#include "comfy/loader.hpp"
#include "llm/client.hpp"
#include "media/ffmpeg.hpp"
#include "stages/tts_backends.hpp"
#include "comfy/renderers.hpp"
#include "config/runtime.hpp"
#include "http/run.hpp"
#include "infer/sd_image.hpp"
#include "infer/sd_video.hpp"
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
        } else if (s.tts.backend == "comfy") {
            try {
                auto tts_client = std::make_shared<comfy::Client>(
                    [] { return config::runtime().snapshot().comfy; },
                    comfy::default_transport(
                        [] { return config::runtime().snapshot().comfy; }));
                if (const auto wf =
                        comfy::load_workflow(*tts_client, store, "tts", false)) {
                    b.tts = stages::comfy_tts_backend(tts_client, *wf,
                                                      store.paths(), b.ffmpeg);
                }
            } catch (const std::exception&) {
                // 连不上 ComfyUI 或者工作流读不了。退回估算后端。
            }
        }
        if (s.models.engine != "comfy") return b;

        // ---- 走 ComfyUI ----
        //
        // 客户端用 shared_ptr：下面这两个函数对象会被存进 job 里，
        // 在工作线程上跑几十分钟，比这次调用活得久得多。
        auto client = std::make_shared<comfy::Client>(
            [] { return config::runtime().snapshot().comfy; },
            comfy::default_transport(
                [] { return config::runtime().snapshot().comfy; }));

        const auto wfs = comfy::load_all(*client, store);
        const auto video = wfs.find("video");
        if (video == wfs.end() || !video->second.has_value()) {
            // load_all 里 video 是必需的，走不到这儿；真走到了说明
            // 上面的约定变了，那时候宁可响亮地失败。
            throw ApiError(400, "配置选了 comfy 引擎，但没有可用的视频工作流");
        }
        b.video = comfy::video_renderer(client, *video->second);

        // **首帧只在项目里有 image.json 时才走 ComfyUI。**
        // 图像工作流是用户提供的（内置只有 video 和 tts），没有就退回
        // sd.cpp——Python 那边的退路是"用视频模型出单帧再抽帧"，
        // 那条路要 ffmpeg 抽帧，等阶段 7 装配那部分一起做。
        const auto image = wfs.find("image");
        if (image != wfs.end() && image->second.has_value()) {
            b.frame = comfy::image_frame_renderer(client, *image->second,
                                                  store.paths());
            b.frame_backend_name = "comfy image_model";
        }
        return b;
    };
    return d;
}

}  // namespace changji::http
