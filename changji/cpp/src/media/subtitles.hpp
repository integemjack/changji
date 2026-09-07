#pragma once

// 中文字幕生成。
//
// **用 ASS 不用 SRT**，因为 SRT 没法控样式和位置。
//
// **断行自己算，不依赖渲染库的自动换行。** libass 对中文只按字符断不按语义断，
// 一句话会在词中间折断，观感很差。自己算好断点插换行符才对——
// 所以 ASS 头里 WrapStyle 是 2（只在显式换行符处断）。
//
// 移植自 src/changji/assembly/subtitles.py。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace changji::media {

/// 按**显示宽度**算长度。全角算一，半角算半。
///
/// 判据是 Unicode 的 east_asian_width 为 W 或 F。区间表由
/// tools/gen_eaw.py 从 Python 的 unicodedata 导出——手写必然有出入，
/// 而出入的表现不是报错，是某几句字幕断行位置和 Python 不一样，
/// 那要逐帧比对成片才看得出来。
double display_width(const std::string& utf8);

/// 一个码点是不是宽的。
bool is_wide(char32_t cp);

/// 中文断行。
///
/// 优先在标点后断，其次在宽度上限处断，但要避开标点不能在行首行尾的情况。
/// 超过行数上限时，**塞不下的部分并进最后一行**——宁可最后一行长一点
/// 也不丢字。丢字是最糟的：观众看到的是一句没说完的话。
std::vector<std::string> wrap_chinese(const std::string& text,
                                      double max_width = 15.0,
                                      int max_lines = 2);

/// 一条字幕。
struct SubtitleCue {
    double start_s = 0.0;
    double end_s = 0.0;
    std::string text;
    /// dialogue / narration / title。别的值一律当 dialogue。
    std::string style = "dialogue";

    double duration_s() const { return end_s - start_s; }
};

/// ASS 的时间格式：时:分:秒.厘秒。
std::string ass_time(double seconds);

struct AssOptions {
    int width = 1080;
    int height = 1920;
    std::string font = "Source Han Sans SC";
    int font_size = 54;
    int max_chars_per_line = 15;
    int max_lines = 2;
    /// 竖屏短剧的字幕放在下方**偏上一点**，避开平台的界面元素。
    int margin_v = 180;
};

std::string build_ass(const std::vector<SubtitleCue>& cues,
                      const AssOptions& opt = {});

/// 写文件。**带 UTF-8 BOM**——一些播放器靠它才认出中文。
std::filesystem::path write_ass(const std::filesystem::path& path,
                                const std::vector<SubtitleCue>& cues,
                                const AssOptions& opt = {});

/// 字幕自检。装配后闸门要用。返回人话，空表示没问题。
std::vector<std::string> validate_cues(const std::vector<SubtitleCue>& cues,
                                       int max_chars_per_line = 15);

}  // namespace changji::media
