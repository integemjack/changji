#pragma once

// 把镜头拼成一集。
//
// 移植自 src/changji/assembly/assemble.py。
//
// ---
//
// **ffmpeg 的参数拼成什么样，这里当成契约来测。**
//
// 和 sd_video 的 encode_args 是同一个理由：参数错了**不会当场报错**，
// 只会让成片在某些播放器上打不开、音画错位、或者中间丢掉一整个镜头。
// 那要等到成片出来、有人从头看一遍才发现，而一集是三分钟。
//
// 所以每一步的参数都是一个纯函数，Assembler 只负责按顺序调它们。

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "config/settings.hpp"
#include "media/ffmpeg.hpp"
#include "media/subtitles.hpp"
#include "models/project.hpp"
#include "models/shot.hpp"

namespace changji::media {

class AssemblyError : public std::runtime_error {
public:
    explicit AssemblyError(const std::string& what) : std::runtime_error(what) {}
};

/// 时间线上的一个镜头。
struct TimelineEntry {
    std::string shot_id;
    std::filesystem::path video_path;
    double start_s = 0.0;
    double duration_s = 0.0;
    models::Transition transition_in = models::Transition::CUT;
    double transition_dur_s = 0.0;
    std::vector<std::filesystem::path> audio_paths;
    std::vector<SubtitleCue> cues;
};

/// 一集的时间线。分镜表加配音时长推出来的排期。
struct Timeline {
    std::vector<TimelineEntry> entries;

    double total_duration_s() const;
    std::vector<SubtitleCue> cues() const;
};

/// 由分镜表推出时间线。
///
/// **字幕的时间戳来自配音的真实时长，不是估算。** 这是音画对齐的最后一环——
/// 用估算时长的话，一集下来字幕会越飘越远，而每一条单看都"差不多对"。
Timeline build_timeline(const std::vector<models::Shot>& shots,
                        const models::ProjectPaths& paths,
                        const config::AssemblyConfig& config);

// ---- 各步的参数。纯函数，测得死。 ----

/// 统一编码规格。
///
/// **拼接环节最容易踩的坑。** 分辨率、帧率、像素格式、采样宽高比、时基
/// 任何一项不齐都会导致花屏或丢帧，而 concat 分离器不会为此报错。
std::vector<std::string> normalize_args(const std::filesystem::path& src,
                                        int target_w, int target_h,
                                        const config::AssemblyConfig& config,
                                        const std::filesystem::path& dest);

/// concat 清单文件的内容。路径一律正斜杠。
std::string concat_listing(const std::vector<std::filesystem::path>& clips);

/// 拼接。**零重编码**——上一步已经把规格统一了。
std::vector<std::string> concat_args(const std::filesystem::path& listing,
                                     const std::filesystem::path& dest);

/// 没有配音时也要有音轨，否则有些平台会认为文件损坏。
std::vector<std::string> silent_audio_args(const std::filesystem::path& video,
                                           const config::AssemblyConfig& config,
                                           const std::filesystem::path& dest);

/// 一段配音在时间线上的位置。
struct AudioSegment {
    std::filesystem::path path;
    double at_s = 0.0;
};

/// 混音并做响度归一。
///
/// **绝不能用 -shortest**：配音总长几乎总是短于画面（无对白的镜头没有音频），
/// 用它会把成片截到配音那么长，后面的画面直接丢掉。
/// 实测一次四镜头的片子丢了整整一个镜头。
std::vector<std::string> mix_args(const std::filesystem::path& video,
                                  const std::vector<AudioSegment>& segments,
                                  const config::AssemblyConfig& config,
                                  double target_lufs, double max_true_peak_db,
                                  double video_duration_s,
                                  const std::filesystem::path& dest);

/// 烧字幕。
std::vector<std::string> burn_args(const std::filesystem::path& video,
                                   const std::filesystem::path& ass_path,
                                   const config::AssemblyConfig& config,
                                   const std::filesystem::path& dest);

/// 把路径转成 ffmpeg 滤镜能接受的形式。
///
/// Windows 上 `C:\x` 里的冒号是滤镜的参数分隔符，反斜杠是转义符，
/// 直接传进去滤镜解析失败——而报错信息是滤镜语法错误，
/// 看不出根因是路径。
std::string escape_filter_path(const std::filesystem::path& path);

/// 装配前先查一遍字幕。
std::vector<std::string> subtitle_problems(const Timeline& timeline,
                                           const config::AssemblyConfig& config);

/// 把镜头拼成一集。
class Assembler {
public:
    Assembler(const FFmpeg& ff, config::AssemblyConfig config,
              models::ProjectPaths paths, double target_lufs = -16.0,
              double max_true_peak_db = -1.5);

    /// 装配成片，返回成片路径。
    std::filesystem::path assemble(const Timeline& timeline,
                                   const std::string& out_name = "episode.mp4",
                                   bool burn_subtitles = true);

    /// 取最大的那个分辨率作为统一规格，**避免放大模糊**。
    std::pair<int, int> target_size(const Timeline& timeline) const;

private:
    const FFmpeg& ff_;
    config::AssemblyConfig config_;
    models::ProjectPaths paths_;
    /// 响度目标从闸门配置传进来。
    ///
    /// 以前这里写死 -16，界面上改了响度目标其实一点用没有，
    /// 成片还是老样子——一个只写不读的开关，和 _TIER_OVERRIDES 同一类问题。
    double target_lufs_;
    double max_true_peak_db_;
};

}  // namespace changji::media
