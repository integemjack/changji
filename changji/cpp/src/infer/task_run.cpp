#include "infer/task_run.hpp"

#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "infer/sd_video.hpp"
#include "media/ffmpeg.hpp"
#include "models/shot.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"
#include "util/paths.hpp"

namespace changji::infer {

std::string cannot_do(const Task& t, const config::Settings& s) {
    const auto ws = s.workspace_path();

    // 出图出片都要扩散模型和它的 VAE
    const std::string& which =
        t.kind == TaskKind::Video ? s.models.video : s.models.image;
    if (which.empty()) {
        return std::string(t.kind == TaskKind::Video ? "[models].video"
                                                     : "[models].image") +
               " 没配，这台干不了" +
               (t.kind == TaskKind::Video ? "出片" : "出图");
    }
    for (const auto& [key, name] :
         std::vector<std::pair<const char*, std::string>>{
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

    // **出片要 ffmpeg 把帧编成 mp4。** 就是这一条烧过一次，见头文件。
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
    const auto dir =
        std::filesystem::path(paths::from_utf8(t.dest)).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec) return "产物目录建不出来：" + paths::to_utf8(dir);
    }
    return {};
}

TaskResult run_task_locally(const Task& t, const config::Settings& s,
                            Origin origin, const StepCallback& on_step,
                            pipeline::CancelToken& tok) {
    TaskResult result;
    try {
        const std::filesystem::path dest = paths::from_utf8(t.dest);
        models::Shot shot;
        shot.shot_id = t.shot_id;

        if (t.kind == TaskKind::Frame) {
            // **种子是派活方算好的。** 这边算不了：`frame_seed` 要
            // `attempts`，而这儿拿不到那个数。
            stages::sd_renderer_with_seed(t.seed, origin)(
                shot, t.prompts, t.spec, dest, tok, on_step);
        } else {
            stages::RenderPlan plan;
            plan.shot_id = t.shot_id;
            plan.tier = t.tier;
            plan.spec = t.spec;
            plan.frames = t.frames;
            plan.prompts = t.prompts;
            plan.motion = t.motion;
            plan.style_line = t.style_line;
            std::optional<std::filesystem::path> start;
            if (t.start_image) start = paths::from_utf8(*t.start_image);
            sd_video_renderer_with_seed(s, t.seed, origin)(
                shot, plan, start, dest, tok, on_step);
        }
        result.ok = true;
        result.dest = t.dest;
    } catch (const std::exception& e) {
        result.ok = false;
        // 这句会一路变成派活方事件流里的那条 warn，所以要能直接给用户看。
        result.error = e.what();
    }
    return result;
}

}  // namespace changji::infer
