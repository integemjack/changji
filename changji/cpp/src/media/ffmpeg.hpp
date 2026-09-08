#pragma once

// ffmpeg 与 ffprobe 的封装。
//
// 装配、闸门、抽帧都要用。移植自 src/changji/assembly/ffmpeg.py。
//
// ---
//
// **解析和执行是分开的。** 跑子进程要真有 ffmpeg 才行，而这一层真正容易
// 错的是**解析**：ffprobe 的 JSON 里帧率写成 "24000/1001" 这种分数，
// signalstats 的输出是一堆 key=value 的行，loudnorm 把 JSON 打在 stderr 里
// 混在日志中间。这几个解析器全是纯函数，喂真实输出的样本就能测死。
//
// 解析错了的表现都很隐蔽：帧率算成 0 会让时长校验永远通过；
// 展布取错字段会让所有画面都被判成纯色，于是整集重跑还是全被拒。

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace changji::media {

class FFmpegError : public std::runtime_error {
public:
    explicit FFmpegError(const std::string& what) : std::runtime_error(what) {}
};

/// 找不到 ffmpeg。装配环节的硬依赖。
class FFmpegMissing : public FFmpegError {
public:
    explicit FFmpegMissing(const std::string& what) : FFmpegError(what) {}
};

/// 一个媒体文件的关键信息。
struct MediaInfo {
    std::filesystem::path path;
    double duration_s = 0.0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int frames = 0;
    bool has_video = false;
    bool has_audio = false;
    std::string pix_fmt;
    std::string sar;
};

/// 一帧的亮度统计。用来判断画面是不是纯色或全黑。
///
/// **展布用 YHIGH 减 YLOW**，也就是第 90 和第 10 百分位之差，不是极差。
/// 极差会被单个亮点或暗点带偏：一张几乎全黑但有一个高光点的废图，
/// 极差能到 250，看起来很正常。百分位之差不吃这一套。
struct PixelStats {
    double mean = 0.0;
    double spread = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double low = 0.0;
    double high = 0.0;

    /// 近乎纯色。生成失败最常见的表现。
    ///
    /// 阈值取 8。实测正常画面的展布在 40 以上，纯色的在个位数。
    bool looks_blank() const { return spread < 8.0; }

    /// 整体过曝或全黑。
    bool looks_clipped() const { return mean < 6.0 || mean > 249.0; }
};

// ---- 纯函数。喂真实输出就能测。 ----

/// 解析成浮点。解析不了返回 0，不抛——ffprobe 对某些容器会给
/// "N/A" 或者干脆不给这个字段，那不是错误。
double to_double(const std::string& v);

/// 帧率。ffprobe 给的是 "24/1"、"24000/1001" 这种分数。
///
/// **不能直接 stod**："24000/1001" 会被解析成 24000，也就是把
/// NTSC 的 23.976 当成 24000 fps。那之后所有按帧率算的时长全错。
double parse_fps(const std::string& rate);

/// 取样位置（占总时长的比例）。**避开首尾各 10%**，那里常有编码伪影。
std::vector<double> sample_points(int samples);

/// 解析 ffprobe 的 -show_format -show_streams JSON。
MediaInfo parse_probe(const std::string& json_text,
                      const std::filesystem::path& path);

/// 解析 signalstats 的输出。
///
/// **signalstats 不输出标准差。** YDIF 是相邻帧差异，单帧模式下根本不产生，
/// 拿它当标准差会让所有画面都被判成纯色——于是整集重跑，还是全被拒。
PixelStats parse_signalstats(const std::string& text);

/// 从 loudnorm 的输出里抠出那段 JSON。
///
/// ffmpeg 把它打在 **stderr** 上，混在一堆日志中间，所以要从最后一个
/// 大括号对里取。只留 input_/output_/target_ 开头的键。
std::map<std::string, double> parse_loudnorm(const std::string& stderr_text);

// ---- 执行 ----

/// 跑一个子进程。
///
/// **stdout 和 stderr 是合并的**，这不是偷懒：ffmpeg 把有用的东西
/// 两边都放——signalstats 走 stdout，loudnorm 的 JSON 走 stderr，
/// 版本信息也在 stderr。分开读要为每种调用记住"该看哪边"，
/// 而记错的表现是"解析不到结果"，看不出是读错了流。
struct ProcResult {
    int exit_code = 0;
    std::string out;
    /// 进程有没有起来。区分"跑了但失败"和"根本没这个程序"——
    /// 后者要说"去装 ffmpeg"，前者要把 ffmpeg 自己的报错贴出来。
    bool launched = false;

    /// 超时被杀掉了。
    ///
    /// 从 2026-09-08 起 `proc::run` 真的会超时杀进程（原来 popen 没这个
    /// 能力，超时参数是摆设）。**能力一旦生效，就得能报出来**——
    /// 不然一次跑了半小时被杀掉的编码，看到的是"ffmpeg 退出码 1"
    /// 加一段被截断的日志，人会以为是编码参数不对。
    bool timed_out = false;
};

using Runner = std::function<ProcResult(const std::string& exe,
                                        const std::vector<std::string>& args,
                                        double timeout_s)>;

/// 真正起子进程的那个。实现走 util/proc。
Runner default_runner();

class FFmpeg {
public:
    FFmpeg(std::string ffmpeg_exe, std::string ffprobe_exe, Runner runner);

    /// 缺了要在跑之前就说清楚，而不是跑到装配才炸。
    void check() const;
    bool available() const;

    MediaInfo probe(const std::filesystem::path& p) const;

    /// 取一帧的亮度统计。
    ///
    /// 用 ffmpeg 的 signalstats 滤镜算，不在进程里解码——
    /// 省掉一个图像库依赖，而交叉编译到树莓派时那是一笔实在的成本。
    PixelStats pixel_stats(const std::filesystem::path& p,
                           std::optional<double> at_second = std::nullopt) const;

    /// 在片段的几个位置取样。**只看一帧会漏掉中途崩坏的镜头。**
    std::vector<PixelStats> sample_pixel_stats(const std::filesystem::path& p,
                                               int samples = 3) const;

    /// 测响度。两遍归一法的第一遍。
    std::map<std::string, double> measure_loudness(
        const std::filesystem::path& p) const;

    /// 抽一帧存成图片。首尾帧比对和"视频模型出单帧"那条退路都要用。
    std::filesystem::path extract_frame(const std::filesystem::path& p,
                                        const std::filesystem::path& dest,
                                        double at_second = 0.0) const;

    /// 直接跑一串参数。装配那边要拼很长的滤镜链，包不完。
    std::string run(const std::vector<std::string>& args,
                    double timeout_s = 1800.0) const;

private:
    std::string run_exe(const std::string& exe,
                        const std::vector<std::string>& args,
                        double timeout_s) const;

    std::string ffmpeg_;
    std::string ffprobe_;
    Runner runner_;
};

}  // namespace changji::media
