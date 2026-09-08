#include "media/assemble.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::media {

namespace {

std::string fmt(const char* spec, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

/// 路径转成 UTF-8 的正斜杠形式。ffmpeg 的清单和滤镜都只认正斜杠。
std::string fwd(const fs::path& p) {
    std::string s = paths::to_utf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

}  // namespace

double Timeline::total_duration_s() const {
    double total = 0.0;
    for (const auto& e : entries) total += e.duration_s;
    return total;
}

std::vector<SubtitleCue> Timeline::cues() const {
    std::vector<SubtitleCue> out;
    for (const auto& e : entries) {
        out.insert(out.end(), e.cues.begin(), e.cues.end());
    }
    return out;
}

Timeline build_timeline(const std::vector<models::Shot>& shots,
                        const models::ProjectPaths& paths,
                        const config::AssemblyConfig& /*config*/) {
    Timeline timeline;
    double cursor = 0.0;

    for (const auto& shot : shots) {
        if (!shot.video_path.has_value() || shot.video_path->empty()) {
            throw AssemblyError("镜头 " + shot.shot_id + " 还没有视频，不能装配");
        }
        const fs::path video = paths.abs(*shot.video_path);
        std::error_code ec;
        if (!fs::is_regular_file(video, ec)) {
            throw AssemblyError("镜头 " + shot.shot_id + " 的视频文件不见了：" +
                                paths::to_utf8(video));
        }

        // 溶解会让两镜重叠，起点要往回挪。硬切没有重叠。
        const double overlap = shot.transition_in != models::Transition::CUT
                                   ? shot.transition_dur_s
                                   : 0.0;
        const double start = std::max(0.0, cursor - overlap);

        TimelineEntry entry;
        entry.shot_id = shot.shot_id;
        entry.video_path = video;
        entry.start_s = start;
        entry.duration_s = shot.duration_s;
        entry.transition_in = shot.transition_in;
        entry.transition_dur_s = shot.transition_dur_s;

        double speech_cursor = start;
        for (const auto& line : shot.dialogue) {
            const double dur = line.actual_duration_s.value_or(0.0);
            if (line.audio_path.has_value() && !line.audio_path->empty()) {
                entry.audio_paths.push_back(paths.abs(*line.audio_path));
            }
            const std::string t = text::strip_ws(line.text);
            if (!t.empty() && dur > 0.0) {
                SubtitleCue cue;
                cue.start_s = speech_cursor;
                cue.end_s = speech_cursor + dur;
                cue.text = t;
                // 没有 char_id 就是旁白，用另一套样式。
                cue.style = line.char_id.has_value() ? "dialogue" : "narration";
                entry.cues.push_back(cue);
            }
            speech_cursor += dur;
        }

        cursor = start + shot.duration_s;
        timeline.entries.push_back(std::move(entry));
    }
    return timeline;
}

std::vector<std::string> normalize_args(const fs::path& src, int target_w,
                                        int target_h,
                                        const config::AssemblyConfig& config,
                                        const fs::path& dest) {
    const std::string w = std::to_string(target_w);
    const std::string h = std::to_string(target_h);
    // 先按比例缩到框内，再补边到目标尺寸。直接 scale 到目标尺寸会拉伸，
    // 而竖屏短剧里混进一个横屏镜头时，拉伸出来的人脸一眼就不对。
    const std::string vf = "scale=" + w + ":" + h +
                           ":force_original_aspect_ratio=decrease,"
                           "pad=" + w + ":" + h + ":(ow-iw)/2:(oh-ih)/2,"
                           "setsar=1,fps=" + std::to_string(config.fps);
    return {
        "-y",
        "-i", paths::to_utf8(src),
        "-vf", vf,
        "-an",   // 音频统一在后面处理
        "-c:v", config.video_codec,
        "-crf", std::to_string(config.crf),
        "-pix_fmt", config.pix_fmt,
        "-preset", "medium",
        // 时基也要统一。不统一的话 concat 会在拼接处丢帧，
        // 表现是每一镜的开头卡一下。
        "-video_track_timescale", "90000",
        paths::to_utf8(dest),
    };
}

std::string concat_listing(const std::vector<fs::path>& clips) {
    std::string out;
    for (const auto& p : clips) {
        out += "file '" + fwd(p) + "'\n";
    }
    return out;
}

std::vector<std::string> concat_args(const fs::path& listing,
                                     const fs::path& dest) {
    // -safe 0 是必需的：清单里是绝对路径，默认的安全检查会拒绝。
    return {"-y", "-f", "concat", "-safe", "0", "-i", paths::to_utf8(listing),
            "-c", "copy", paths::to_utf8(dest)};
}

std::vector<std::string> silent_audio_args(const fs::path& video,
                                           const config::AssemblyConfig& config,
                                           const fs::path& dest) {
    // 这里用 -shortest 是安全的：静音源是无限长的，截到视频长度
    // 正是想要的结果。**下面 mix_args 那条路不能用它**，理由见那边。
    return {
        "-y",
        "-i", paths::to_utf8(video),
        "-f", "lavfi", "-i",
        "anullsrc=r=" + std::to_string(config.audio_sample_rate) + ":cl=stereo",
        "-c:v", "copy",
        "-c:a", config.audio_codec,
        "-b:a", config.audio_bitrate,
        "-ar", std::to_string(config.audio_sample_rate),
        "-ac", std::to_string(config.audio_channels),
        "-shortest",
        paths::to_utf8(dest),
    };
}

std::vector<std::string> mix_args(const fs::path& video,
                                  const std::vector<AudioSegment>& segments,
                                  const config::AssemblyConfig& config,
                                  double target_lufs, double max_true_peak_db,
                                  double video_duration_s, const fs::path& dest) {
    std::vector<std::string> args = {"-y", "-i", paths::to_utf8(video)};
    for (const auto& s : segments) args.insert(args.end(), {"-i", paths::to_utf8(s.path)});

    // 每条配音延迟到自己的位置，然后混在一起。
    std::vector<std::string> filters;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const std::string n = std::to_string(i + 1);
        const std::string delay_ms =
            std::to_string(static_cast<long long>(segments[i].at_s * 1000.0));
        // adelay 要给每个声道各一个值，所以是 "ms|ms"。只给一个的话
        // 右声道不延迟，听起来是回声。
        filters.push_back("[" + n + ":a]aresample=" +
                          std::to_string(config.audio_sample_rate) +
                          ",adelay=" + delay_ms + "|" + delay_ms + "[a" + n + "]");
    }

    std::string mix_inputs;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        mix_inputs += "[a" + std::to_string(i + 1) + "]";
    }
    // loudnorm 之后必须再 aresample 一次：这个滤镜内部按 192k 工作，
    // 不收回来的话编码器会挑个 96k 之类的采样率，文件白白变大。
    filters.push_back(
        mix_inputs + "amix=inputs=" + std::to_string(segments.size()) +
        // %.12g 而不是 %g。**这里和 storyboard.cpp 的 format_g 不是一回事**：
        // 那边对的是 Python 的 f"{x:g}"（也是 6 位有效数字，两边同一套规则），
        // 这边对的是 Python 的 f"{self.target_lufs}"，也就是 str(float)，
        // 不截位。%g 默认 6 位有效数字，target_lufs 填 -16.123456 时
        // C++ 会写出 -16.1235——**响度目标真的变了**，不是显示问题。
        //
        // 剩下一处对不齐是有意留着的：整数值 Python 写 "-16.0"，
        // 这里写 "-16"。ffmpeg 两个都当 -16 解析，成片一模一样；
        // 要逐字节一样得照搬 Python 的 repr 规则（整数浮点补 ".0"），
        // 为一个解析结果相同的字符串背那套算法不值。
        ":dropout_transition=0:normalize=0,loudnorm=I=" + fmt("%.12g", target_lufs) +
        ":TP=" + fmt("%.12g", max_true_peak_db) + ":LRA=11,aresample=" +
        std::to_string(config.audio_sample_rate) + "[amixed]");
    // 音轨补静音到视频长度。
    filters.push_back("[amixed]apad[aout]");

    std::string chain;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (i) chain += ";";
        chain += filters[i];
    }

    args.insert(args.end(), {
        "-filter_complex", chain,
        "-map", "0:v", "-map", "[aout]",
        "-c:v", "copy",
        "-c:a", config.audio_codec, "-b:a", config.audio_bitrate,
        "-ar", std::to_string(config.audio_sample_rate),
        "-ac", std::to_string(config.audio_channels),
        // **以视频为准截断补出来的静音尾巴，而不是 -shortest。**
        // 配音总长几乎总是短于画面（无对白的镜头没有音频），
        // -shortest 会把成片截到配音那么长，后面的画面直接丢掉。
        "-t", fmt("%.3f", video_duration_s),
        paths::to_utf8(dest),
    });
    return args;
}

std::vector<std::string> burn_args(const fs::path& video,
                                   const fs::path& ass_path,
                                   const config::AssemblyConfig& config,
                                   const fs::path& dest) {
    return {
        "-y",
        "-i", paths::to_utf8(video),
        "-vf", "subtitles='" + escape_filter_path(ass_path) + "'",
        "-c:v", config.video_codec,
        "-crf", std::to_string(config.crf),
        "-pix_fmt", config.pix_fmt,
        "-preset", "medium",
        // 音频原样拷贝。重编码一次是白白多一次有损压缩，
        // 而烧字幕根本不碰音轨。
        "-c:a", "copy",
        paths::to_utf8(dest),
    };
}

std::string escape_filter_path(const fs::path& path) {
    std::string text = fwd(path);
    // 冒号是滤镜的参数分隔符，要转义。反斜杠上面已经换成正斜杠了。
    std::string out;
    for (const char c : text) {
        if (c == ':') out += '\\';
        out += c;
    }
    return out;
}

std::vector<std::string> subtitle_problems(const Timeline& timeline,
                                           const config::AssemblyConfig& config) {
    return validate_cues(timeline.cues(), config.subtitle_max_chars_per_line);
}

// ---- Assembler ----

Assembler::Assembler(const FFmpeg& ff, config::AssemblyConfig config,
                     models::ProjectPaths paths, double target_lufs,
                     double max_true_peak_db)
    : ff_(ff),
      config_(std::move(config)),
      paths_(std::move(paths)),
      target_lufs_(target_lufs),
      max_true_peak_db_(max_true_peak_db) {}

std::pair<int, int> Assembler::target_size(const Timeline& timeline) const {
    int bw = 0, bh = 0;
    for (const auto& e : timeline.entries) {
        try {
            const MediaInfo info = ff_.probe(e.video_path);
            if (info.width * info.height > bw * bh) {
                bw = info.width;
                bh = info.height;
            }
        } catch (const FFmpegError&) {
            // 一镜读不出来不该让整集装不了：后面还有几十镜，
            // 取最大值这件事少一个样本没关系。
            continue;
        }
    }
    if (bw == 0 && bh == 0) throw AssemblyError("所有镜头都读不出分辨率");
    // 必须是偶数，否则 yuv420p 编不了——而报错是编码器内部的，很难懂。
    return {bw - bw % 2, bh - bh % 2};
}

fs::path Assembler::assemble(const Timeline& timeline,
                             const std::string& out_name, bool burn_subtitles) {
    if (timeline.entries.empty()) {
        throw AssemblyError("时间线是空的，没有可装配的镜头");
    }

    const fs::path work = paths_.output() / ".work";
    std::error_code ec;
    fs::create_directories(work, ec);

    // 中间文件不管成功失败都要清掉：一集的中间产物是几百兆，
    // 留在 output/.work 里几次之后磁盘就满了，而用户看不到那个目录。
    struct Cleanup {
        const fs::path& dir;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(dir, e);
        }
    } cleanup{work};

    const auto [tw, th] = target_size(timeline);

    std::vector<fs::path> normalized;
    for (std::size_t i = 0; i < timeline.entries.size(); ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "norm_%04zu.mp4", i);
        const fs::path out = work / name;
        ff_.run(normalize_args(timeline.entries[i].video_path, tw, th, config_, out));
        normalized.push_back(out);
    }

    const fs::path listing = work / "concat.txt";
    {
        std::ofstream f(listing, std::ios::binary | std::ios::trunc);
        if (!f) throw AssemblyError("写不了拼接清单：" + paths::to_utf8(listing));
        f << concat_listing(normalized);
    }
    const fs::path silent = work / "joined.mp4";
    ff_.run(concat_args(listing, silent));

    // 每条配音落在时间线上的位置。
    std::vector<AudioSegment> segments;
    for (const auto& entry : timeline.entries) {
        double cursor = entry.start_s;
        for (std::size_t i = 0; i < entry.audio_paths.size(); ++i) {
            segments.push_back({entry.audio_paths[i], cursor});
            cursor += i < entry.cues.size() ? entry.cues[i].duration_s() : 0.0;
        }
    }

    const fs::path with_audio = work / "with_audio.mp4";
    if (segments.empty()) {
        ff_.run(silent_audio_args(silent, config_, with_audio));
    } else {
        const double dur = ff_.probe(silent).duration_s;
        ff_.run(mix_args(silent, segments, config_, target_lufs_,
                         max_true_peak_db_, dur, with_audio));
    }

    const fs::path final_path = paths_.output() / paths::from_utf8(out_name);
    fs::create_directories(final_path.parent_path(), ec);

    const auto cues = timeline.cues();
    if (burn_subtitles && !cues.empty()) {
        const MediaInfo info = ff_.probe(with_audio);
        AssOptions opt;
        opt.width = info.width > 0 ? info.width : 1080;
        opt.height = info.height > 0 ? info.height : 1920;
        opt.font = config_.subtitle_font;
        opt.max_chars_per_line = config_.subtitle_max_chars_per_line;
        opt.max_lines = config_.subtitle_max_lines;

        const fs::path ass =
            paths_.subtitles() / paths::from_utf8(
                paths::to_utf8(final_path.stem()) + ".ass");
        write_ass(ass, cues, opt);
        ff_.run(burn_args(with_audio, ass, config_, final_path));
    } else {
        // 没字幕就直接搬过去，不重编码一遍——那是白白多一次有损压缩。
        fs::remove(final_path, ec);
        fs::rename(with_audio, final_path, ec);
        if (ec) {
            // 跨盘 rename 会失败（工作目录和输出目录理论上同在项目里，
            // 但用户可能把 output 做成了符号链接）。退回复制。
            fs::copy_file(with_audio, final_path,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                throw AssemblyError("成片挪不到 " + paths::to_utf8(final_path) +
                                    "：" + ec.message());
            }
        }
    }
    return final_path;
}

}  // namespace changji::media
