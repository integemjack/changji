#include "infer/sd_upscale.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "infer/sd_backend.hpp"
#include "infer/sd_image.hpp"
#include "infer/sd_video.hpp"
#include "util/paths.hpp"
#include "util/proc.hpp"

#ifdef CHANGJI_HAVE_SD
#include "stable-diffusion.h"
#endif

namespace fs = std::filesystem;

namespace changji::infer {

namespace {

/// 用 ffprobe 问一段视频的宽高帧率。
struct Probe {
    int width = 0;
    int height = 0;
    int fps = 24;
};

Probe probe_video(const fs::path& in, const config::AssemblyConfig& assembly) {
    const auto exe = proc::which("ffprobe");
    if (!exe.has_value()) {
        throw SdError("找不到 ffprobe。超分要先问出这段视频的宽高帧率");
    }
    const proc::Result r = proc::run(
        *exe,
        {"-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height,r_frame_rate", "-of", "csv=p=0",
         paths::to_utf8(in)},
        // **单位是毫秒，不是秒。** 写 60 的话 ffprobe 在 60 毫秒里跑不完，
        // 被杀掉之后 r.out 是空的，报的错是"读不出视频信息："后面什么都没有
        // ——指向完全错的地方。
        60000);
    if (!r.launched || r.exit_code != 0) {
        throw SdError("ffprobe 读不出视频信息：" + r.out);
    }
    Probe p;
    p.fps = assembly.fps;
    // 形如 "960,544,24/1"
    int w = 0, h = 0, num = 0, den = 1;
    if (std::sscanf(r.out.c_str(), "%d,%d,%d/%d", &w, &h, &num, &den) >= 2) {
        p.width = w;
        p.height = h;
        if (num > 0 && den > 0) p.fps = num / den;
    }
    if (p.width <= 0 || p.height <= 0) {
        throw SdError("ffprobe 给的宽高不对：" + r.out);
    }
    return p;
}

/// 把视频解成一整块 rgb24 裸数据。
///
/// 一段三秒的 960×544 是 73 帧 × 1.5 MB ≈ 115 MB，整块读进来没问题；
/// 换成成片长度的片子要改成流式。
std::vector<unsigned char> decode_to_raw(const fs::path& in, const Probe& p,
                                        const config::AssemblyConfig& assembly,
                                        const fs::path& tmp) {
    const auto exe = proc::which(assembly.ffmpeg_path);
    if (!exe.has_value()) throw SdError("找不到 ffmpeg");
    const proc::Result r = proc::run(
        *exe,
        {"-y", "-i", paths::to_utf8(in), "-f", "rawvideo", "-pix_fmt", "rgb24",
         paths::to_utf8(tmp)},
        0);
    if (!r.launched || r.exit_code != 0) {
        throw SdError("ffmpeg 解码失败：" + r.out);
    }
    std::ifstream f(tmp, std::ios::binary);
    if (!f) throw SdError("读不了解出来的裸数据：" + paths::to_utf8(tmp));
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

}  // namespace

#ifdef CHANGJI_HAVE_SD

void upscale_video(const fs::path& in, const fs::path& out,
                   const fs::path& model, int out_w, int out_h,
                   const config::AssemblyConfig& assembly) {
    if (!fs::is_regular_file(in)) {
        throw SdError("要超分的视频不在：" + paths::to_utf8(in));
    }
    if (!fs::is_regular_file(model)) {
        throw SdError("超分模型不在：" + paths::to_utf8(model));
    }
    sd_log_to_stderr();

    const Probe p = probe_video(in, assembly);
    const fs::path raw_in = fs::path(paths::to_utf8(out) + ".in.raw");
    const fs::path raw_up = fs::path(paths::to_utf8(out) + ".up.raw");

    const std::vector<unsigned char> raw =
        decode_to_raw(in, p, assembly, raw_in);
    const std::size_t frame_bytes =
        static_cast<std::size_t>(p.width) * p.height * 3;
    const std::size_t frames = frame_bytes ? raw.size() / frame_bytes : 0;
    if (frames == 0) throw SdError("解出来一帧都没有");
    std::fprintf(stderr, "[超分] %d×%d %zu 帧\n", p.width, p.height, frames);

    // **超分上下文只建一次。** 每帧重建的话 67 MB 的权重要反复加载，
    // 73 帧就是 73 次——这类"每次都重来"的浪费在这套代码里栽过好几回。
    upscaler_ctx_t* ctx = ::new_upscaler_ctx(paths::to_utf8(model).c_str(),
                                             false, -1, 0, "", "");
    if (ctx == nullptr) {
        throw SdError("超分模型加载失败：" + paths::to_utf8(model) +
                      "\n看一眼上面 sd.cpp 打的日志");
    }
    const int factor = ::get_upscale_factor(ctx);
    std::fprintf(stderr, "[超分] 模型倍率 %d，先放大再压到 %d×%d\n", factor,
                 out_w, out_h);

    std::ofstream up(raw_up, std::ios::binary | std::ios::trunc);
    if (!up) throw SdError("写不了中间文件：" + paths::to_utf8(raw_up));

    for (std::size_t i = 0; i < frames; ++i) {
        sd_image_t src{};
        src.width = static_cast<std::uint32_t>(p.width);
        src.height = static_cast<std::uint32_t>(p.height);
        src.channel = 3;
        // sd.cpp 不会改输入，但接口要非 const 指针。
        src.data = const_cast<unsigned char*>(raw.data() + i * frame_bytes);

        sd_image_t* got = nullptr;
        int n = 0;
        if (!::upscale(ctx, src, static_cast<std::uint32_t>(factor), &got, &n) ||
            got == nullptr || n < 1) {
            ::free_upscaler_ctx(ctx);
            throw SdError("第 " + std::to_string(i + 1) + " 帧超分失败");
        }
        up.write(reinterpret_cast<const char*>(got->data),
                 static_cast<std::streamsize>(
                     static_cast<std::size_t>(got->width) * got->height * 3));
        const int big_w = static_cast<int>(got->width);
        const int big_h = static_cast<int>(got->height);
        ::free_sd_images(got, n);
        if (i == 0) {
            std::fprintf(stderr, "[超分] 放大后每帧 %d×%d\n", big_w, big_h);
        }
        if ((i + 1) % 10 == 0 || i + 1 == frames) {
            std::fprintf(stderr, "[超分] %zu/%zu\n", i + 1, frames);
        }
    }
    up.close();
    ::free_upscaler_ctx(ctx);

    // 放大后的尺寸按模型倍率算，再让 ffmpeg 用 lanczos 压到目标。
    const int big_w = p.width * factor;
    const int big_h = p.height * factor;
    const auto exe = proc::which(assembly.ffmpeg_path);
    if (!exe.has_value()) throw SdError("找不到 ffmpeg");
    const proc::Result r = proc::run(
        *exe,
        {"-y", "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size",
         std::to_string(big_w) + "x" + std::to_string(big_h), "-framerate",
         std::to_string(p.fps), "-i", paths::to_utf8(raw_up), "-an", "-vf",
         "scale=" + std::to_string(out_w) + ":" + std::to_string(out_h) +
             ":flags=lanczos",
         "-c:v", assembly.video_codec, "-crf", std::to_string(assembly.crf),
         "-pix_fmt", assembly.pix_fmt, "-r", std::to_string(p.fps),
         paths::to_utf8(out)},
        0);
    std::error_code ec;
    fs::remove(raw_in, ec);
    fs::remove(raw_up, ec);
    if (!r.launched || r.exit_code != 0) {
        throw SdError("ffmpeg 压回目标尺寸失败：" + r.out);
    }
}

#else

void upscale_video(const fs::path&, const fs::path&, const fs::path&, int, int,
                   const config::AssemblyConfig&) {
    throw SdError("这个二进制没编进 sd.cpp，做不了超分");
}

#endif

}  // namespace changji::infer
