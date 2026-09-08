#include "infer/sd_video.hpp"

#include <fstream>
#include <random>

#include "infer/scheduler.hpp"
#include "util/paths.hpp"
#include "util/proc.hpp"

namespace fs = std::filesystem;

namespace changji::infer {

namespace {

/// 临时的裸帧文件。放在目标旁边而不是系统临时目录：
/// 121 帧 448x768 的 RGB 是 125 MB，系统盘可能没那么多空闲，
/// 而项目目录所在的盘本来就要装成片。
fs::path raw_temp_for(const fs::path& dest) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream os;
    os << ".changji_raw_" << std::hex << rng() << ".rgb";
    return dest.parent_path() / paths::from_utf8(os.str());
}

/// 用完就删。异常路径上也要删——125 MB 的垃圾留在项目目录里，
/// 用户下次看到的是"我的项目怎么这么大"。
struct TempFile {
    fs::path path;
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

}  // namespace

std::vector<std::string> encode_args(const fs::path& raw_path, int width,
                                     int height, int fps,
                                     const config::AssemblyConfig& assembly,
                                     const fs::path& dest) {
    return {
        "-y",                       // 覆盖。重跑一镜时不该卡在"要覆盖吗"上
        "-f", "rawvideo",
        "-pixel_format", "rgb24",   // sd.cpp 吐的就是 RGB24
        "-video_size", std::to_string(width) + "x" + std::to_string(height),
        "-framerate", std::to_string(fps),
        "-i", paths::to_utf8(raw_path),
        "-an",                      // 这一步不带声音，配音是后面的阶段
        "-c:v", assembly.video_codec,
        "-crf", std::to_string(assembly.crf),
        // **像素格式必须显式给。** 不给的话 ffmpeg 会保留 rgb24，
        // 而 H.264 的 rgb24 变体一大半播放器打不开，包括浏览器里的
        // <video>——而审片就是在浏览器里做的。
        "-pix_fmt", assembly.pix_fmt,
        // 拼接环节要求各镜头规格一致。这里和成片用同一套参数，
        // 拼的时候才能直接 concat 而不是重编码——重编码是白白多一次有损压缩。
        "-r", std::to_string(fps),
        paths::to_utf8(dest),
    };
}

void encode_raw_to_mp4(const fs::path& raw_path, int width, int height, int fps,
                       const config::AssemblyConfig& assembly,
                       const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    const auto exe = proc::which(assembly.ffmpeg_path);
    if (!exe.has_value()) {
        throw SdError("找不到 ffmpeg（" + assembly.ffmpeg_path +
                      "）。装配和出片都要它，"
                      "装好之后在配置里填 assembly.ffmpeg_path");
    }

    // 不限时。一镜的编码在低配机器上可能要几十秒，
    // 而超时把它杀掉留下的是一个半截的 mp4——比慢更糟。
    const proc::Result r = proc::run(
        *exe, encode_args(raw_path, width, height, fps, assembly, dest), 0);
    if (!r.launched || r.exit_code != 0) {
        throw SdError("ffmpeg 编码失败（退出码 " + std::to_string(r.exit_code) +
                      "）：\n" + r.out);
    }
    if (!fs::is_regular_file(dest, ec)) {
        throw SdError("ffmpeg 说成功了但没有产出文件：" + paths::to_utf8(dest));
    }
}

namespace {

/// 出片那一段的公共实现。`seed_override` 有值就用它。
stages::VideoRenderer make_video_renderer(
    const config::Settings& settings,
    std::optional<std::int64_t> seed_override) {
    const config::AssemblyConfig assembly = settings.assembly;
    return [assembly, seed_override](
               const models::Shot& shot, const stages::RenderPlan& plan,
                      const std::optional<fs::path>& start_image,
                      const fs::path& dest, pipeline::CancelToken& tok,
                      const StepCallback& on_step) {
        // 每一镜借一次视频槽。跨阶段的显存回收由调度器决定。
        auto lease = scheduler().acquire(Slot::Video);
        auto ctx = current_video_context();
        if (!ctx) throw SdError("出视频上下文没准备好");

        VideoRequest req;
        req.positive = stages::video_positive(plan);
        req.negative = plan.prompts.negative;
        req.width = plan.spec.width;
        req.height = plan.spec.height;
        req.steps = plan.spec.steps;
        req.frames = plan.frames;
        req.fps = assembly.fps;
        req.seed = seed_override
                       ? *seed_override
                       : stages::render_seed(shot.shot_id, shot.attempts);
        req.start_image = start_image;

        const fs::path raw = raw_temp_for(dest);
        TempFile guard{raw};
        ctx->generate_video(req, raw, tok, on_step);
        encode_raw_to_mp4(raw, req.width, req.height, req.fps, assembly, dest);
    };
}

}  // namespace

stages::VideoRenderer sd_video_renderer(const config::Settings& settings) {
    return make_video_renderer(settings, std::nullopt);
}

stages::VideoRenderer sd_video_renderer_with_seed(
    const config::Settings& settings, std::int64_t seed) {
    return make_video_renderer(settings, seed);
}

}  // namespace changji::infer
