// 装配的测试。
//
// **ffmpeg 的参数当契约来测。** 参数错了不会当场报错，只会让成片在
// 某些播放器上打不开、音画错位、或者中间丢掉一整个镜头——那要等到
// 成片出来、有人从头看一遍才发现，而一集是三分钟。
//
// 时间线那部分测的是**字幕时间戳来自配音的真实时长**。用估算时长的话，
// 一集下来字幕会越飘越远，而每一条单看都"差不多对"。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "media/assemble.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 找一个参数的值。找不到返回空串。
std::string arg_value(const std::vector<std::string>& args,
                      const std::string& flag) {
    const auto it = std::find(args.begin(), args.end(), flag);
    if (it == args.end() || it + 1 == args.end()) return {};
    return *(it + 1);
}

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_装配_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

/// 造一个带视频文件的项目。
models::ProjectPaths make_paths(const std::string& tag) {
    const fs::path root = temp_root(tag);
    models::ProjectPaths p(root);
    p.ensure();
    return p;
}

void touch(const fs::path& p, std::size_t bytes = 2048) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << std::string(bytes, 'x');
}

models::Shot make_shot(const std::string& id, double dur,
                       const std::string& video_rel) {
    models::Shot s;
    s.shot_id = id;
    s.duration_s = dur;
    s.video_path = video_rel;
    return s;
}

models::DialogueLine line(const std::string& text, double dur,
                          std::optional<std::string> char_id,
                          const std::string& audio_rel = "") {
    models::DialogueLine l;
    l.text = text;
    l.actual_duration_s = dur;
    l.char_id = std::move(char_id);
    if (!audio_rel.empty()) l.audio_path = audio_rel;
    return l;
}

}  // namespace

TEST_CASE("时间线：字幕时间戳来自配音的真实时长") {
    // 用估算时长的话，一集下来字幕会越飘越远，而每一条单看都"差不多对"。
    const auto paths = make_paths("时间线");
    touch(paths.shots("draft") / "a.mp4");
    touch(paths.shots("draft") / "b.mp4");

    auto s1 = make_shot("sh001", 5.0, "shots/draft/a.mp4");
    s1.dialogue.push_back(line("我等了你三年。", 2.5, "c_lin"));
    s1.dialogue.push_back(line("你却什么都没说。", 1.75, "c_lin"));
    auto s2 = make_shot("sh002", 4.0, "shots/draft/b.mp4");
    s2.dialogue.push_back(line("那一夜之后，她再没回来过。", 3.0, std::nullopt));

    const auto tl = media::build_timeline({s1, s2}, paths,
                                          config::AssemblyConfig{});
    REQUIRE(tl.entries.size() == 2);
    CHECK(tl.entries[0].start_s == doctest::Approx(0.0));
    CHECK(tl.entries[1].start_s == doctest::Approx(5.0));
    CHECK(tl.total_duration_s() == doctest::Approx(9.0));

    const auto cues = tl.cues();
    REQUIRE(cues.size() == 3);
    // 第一句从 0 到 2.5，第二句紧接着——**不是按镜头时长均分**
    CHECK(cues[0].start_s == doctest::Approx(0.0));
    CHECK(cues[0].end_s == doctest::Approx(2.5));
    CHECK(cues[1].start_s == doctest::Approx(2.5));
    CHECK(cues[1].end_s == doctest::Approx(4.25));
    // 第二镜的字幕从这一镜的起点算，不是接着上一句
    CHECK(cues[2].start_s == doctest::Approx(5.0));

    SUBCASE("没有 char_id 的是旁白，用另一套样式") {
        CHECK(cues[0].style == "dialogue");
        CHECK(cues[2].style == "narration");
    }

    SUBCASE("没有配音时长的台词不出字幕") {
        // 时长是 0 说明配音没跑或者跑失败了。硬出一条 0 秒的字幕，
        // 播放器要么闪一下要么直接不显示，两种都比不出更糟——
        // 因为看起来"有字幕"。
        auto s = make_shot("sh003", 3.0, "shots/draft/a.mp4");
        s.dialogue.push_back(line("没配音的一句", 0.0, "c_lin"));
        const auto t = media::build_timeline({s}, paths, config::AssemblyConfig{});
        CHECK(t.cues().empty());
    }
}

TEST_CASE("溶解让两镜重叠，起点往回挪") {
    const auto paths = make_paths("溶解");
    touch(paths.shots("draft") / "a.mp4");

    auto s1 = make_shot("sh001", 5.0, "shots/draft/a.mp4");
    auto s2 = make_shot("sh002", 4.0, "shots/draft/a.mp4");
    s2.transition_in = models::Transition::DISSOLVE;
    s2.transition_dur_s = 0.4;

    const auto tl = media::build_timeline({s1, s2}, paths,
                                          config::AssemblyConfig{});
    CHECK(tl.entries[1].start_s == doctest::Approx(4.6));

    SUBCASE("硬切不重叠") {
        auto s3 = make_shot("sh003", 4.0, "shots/draft/a.mp4");
        s3.transition_in = models::Transition::CUT;
        s3.transition_dur_s = 0.4;   // 硬切时这个值该被忽略
        const auto t = media::build_timeline({s1, s3}, paths,
                                             config::AssemblyConfig{});
        CHECK(t.entries[1].start_s == doctest::Approx(5.0));
    }
}

TEST_CASE("缺视频时说清是哪一镜") {
    const auto paths = make_paths("缺视频");

    SUBCASE("根本没记路径") {
        models::Shot s;
        s.shot_id = "sh001";
        s.duration_s = 5.0;
        try {
            media::build_timeline({s}, paths, config::AssemblyConfig{});
            FAIL("该抛");
        } catch (const media::AssemblyError& e) {
            CHECK(std::string(e.what()).find("sh001") != std::string::npos);
        }
    }

    SUBCASE("记了路径但文件不在") {
        // 这两种要分开说：前者是流水线没跑到，后者是文件被删了或者
        // 项目挪过位置。下一步不一样。
        const auto s = make_shot("sh002", 5.0, "shots/draft/没有这个.mp4");
        try {
            media::build_timeline({s}, paths, config::AssemblyConfig{});
            FAIL("该抛");
        } catch (const media::AssemblyError& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("不见了") != std::string::npos);
        }
    }
}

TEST_CASE("统一规格：先按比例缩到框内再补边") {
    // 直接 scale 到目标尺寸会拉伸，而竖屏短剧里混进一个横屏镜头时，
    // 拉伸出来的人脸一眼就不对。
    config::AssemblyConfig cfg;
    cfg.fps = 24;
    const auto args = media::normalize_args("in.mp4", 1080, 1920, cfg, "out.mp4");
    const std::string vf = arg_value(args, "-vf");
    CAPTURE(vf);

    CHECK(vf.find("force_original_aspect_ratio=decrease") != std::string::npos);
    CHECK(vf.find("pad=1080:1920") != std::string::npos);
    // 采样宽高比也要统一，否则拼接处画面会横向拉伸
    CHECK(vf.find("setsar=1") != std::string::npos);
    CHECK(vf.find("fps=24") != std::string::npos);

    SUBCASE("时基统一到 90000") {
        // 不统一的话 concat 会在拼接处丢帧，表现是每一镜的开头卡一下。
        CHECK(arg_value(args, "-video_track_timescale") == "90000");
    }

    SUBCASE("这一步不带音频") {
        // 音频统一在后面处理。带上的话每一镜的音频会各自重编码一次。
        CHECK(has_flag(args, "-an"));
    }

    SUBCASE("编码参数按配置来") {
        config::AssemblyConfig c2;
        c2.video_codec = "libx265";
        c2.crf = 28;
        c2.pix_fmt = "yuv444p";
        const auto a = media::normalize_args("in.mp4", 640, 352, c2, "out.mp4");
        CHECK(arg_value(a, "-c:v") == "libx265");
        CHECK(arg_value(a, "-crf") == "28");
        CHECK(arg_value(a, "-pix_fmt") == "yuv444p");
    }
}

TEST_CASE("拼接清单用正斜杠，拼接本身零重编码") {
    const std::vector<fs::path> clips = {
        paths::from_utf8("C:/项目 库/雨夜天台/output/.work/norm_0000.mp4"),
        paths::from_utf8("C:/项目 库/雨夜天台/output/.work/norm_0001.mp4"),
    };
    const std::string listing = media::concat_listing(clips);
    CAPTURE(listing);
    CHECK(listing.find('\\') == std::string::npos);
    CHECK(listing.find("file 'C:/项目 库/") != std::string::npos);
    // 每行一个，末尾要有换行——最后一行没换行的话 ffmpeg 会忽略它
    CHECK(listing.back() == '\n');

    const auto args = media::concat_args("list.txt", "joined.mp4");
    CHECK(arg_value(args, "-c") == "copy");   // 零重编码
    // -safe 0 是必需的：清单里是绝对路径，默认的安全检查会拒绝
    CHECK(arg_value(args, "-safe") == "0");
}

TEST_CASE("混音绝不能用 -shortest") {
    // **实测一次四镜头的片子丢了整整一个镜头。**
    // 配音总长几乎总是短于画面（无对白的镜头没有音频），
    // -shortest 会把成片截到配音那么长，后面的画面直接丢掉。
    config::AssemblyConfig cfg;
    const std::vector<media::AudioSegment> segs = {
        {"a1.wav", 0.0}, {"a2.wav", 2.5}, {"a3.wav", 5.0}};
    const auto args =
        media::mix_args("joined.mp4", segs, cfg, -16.0, -1.5, 9.0, "out.mp4");

    CHECK_FALSE(has_flag(args, "-shortest"));
    // 以视频为准截断补出来的静音尾巴
    CHECK(arg_value(args, "-t") == "9.000");

    const std::string chain = arg_value(args, "-filter_complex");
    CAPTURE(chain);

    SUBCASE("音轨补静音到视频长度") {
        CHECK(chain.find("apad") != std::string::npos);
    }

    SUBCASE("每条配音延迟到自己的位置，两个声道各给一个值") {
        // 只给一个值的话右声道不延迟，听起来是回声。
        CHECK(chain.find("adelay=0|0") != std::string::npos);
        CHECK(chain.find("adelay=2500|2500") != std::string::npos);
        CHECK(chain.find("adelay=5000|5000") != std::string::npos);
    }

    SUBCASE("loudnorm 之后要收回采样率") {
        // 这个滤镜内部按 192k 工作，不收回来的话编码器会挑个 96k
        // 之类的采样率，文件白白变大。
        const std::size_t ln = chain.find("loudnorm");
        REQUIRE(ln != std::string::npos);
        CHECK(chain.find("aresample=48000", ln) != std::string::npos);
    }

    SUBCASE("响度目标从参数来，不是写死的") {
        // 以前这里写死 -16，界面上改了响度目标其实一点用没有。
        const auto a = media::mix_args("j.mp4", segs, cfg, -14.0, -2.0, 9.0,
                                       "o.mp4");
        const std::string c = arg_value(a, "-filter_complex");
        CHECK(c.find("loudnorm=I=-14:TP=-2") != std::string::npos);
    }

    SUBCASE("响度目标不许被格式化截掉位数") {
        // **这里原来是 %g，会截到 6 位有效数字。** target_lufs 是配置里
        // 一个没有小数位限制的浮点数，填 -16.123456 的话 C++ 写出的是
        // -16.1235——**响度目标真的变了**，而 Python 那边写的是
        // f"{self.target_lufs}"（str(float)，不截位）。
        //
        // 不是显示问题：ffmpeg 照着 -16.1235 归一，成片响度和 Python 的
        // 不一样，而两边的命令行都"看着对"。
        const auto a = media::mix_args("j.mp4", segs, cfg, -16.123456, -1.234567,
                                       9.0, "o.mp4");
        const std::string c = arg_value(a, "-filter_complex");
        CAPTURE(c);
        CHECK(c.find("loudnorm=I=-16.123456:TP=-1.234567") != std::string::npos);
    }

    SUBCASE("整数值这里写 -16，Python 写 -16.0——有意留着的") {
        // ffmpeg 两个都当 -16 解析，成片一模一样。要逐字节一样得照搬
        // Python 的 repr 规则（整数浮点补 ".0"），为一个解析结果相同的
        // 字符串背那套算法不值。
        //
        // 钉住是因为**下一个人看见这处不一致，得知道它是被想过的**，
        // 而不是抄漏了——尤其是删掉 Python 之后再也没法比对的时候。
        const auto a = media::mix_args("j.mp4", segs, cfg, -16.0, -1.5, 9.0,
                                       "o.mp4");
        const std::string c = arg_value(a, "-filter_complex");
        CHECK(c.find("loudnorm=I=-16:TP=-1.5") != std::string::npos);
        CHECK(c.find("I=-16.0") == std::string::npos);
    }

    SUBCASE("视频原样拷贝") {
        // 混音不碰画面，重编码一次是白白多一次有损压缩。
        CHECK(arg_value(args, "-c:v") == "copy");
    }
}

TEST_CASE("没有配音时也要有音轨") {
    // 没音轨的话有些平台会认为文件损坏。
    config::AssemblyConfig cfg;
    const auto args = media::silent_audio_args("joined.mp4", cfg, "out.mp4");
    const std::string lavfi = arg_value(args, "-f");
    CHECK(lavfi == "lavfi");
    bool found = false;
    for (const auto& a : args) {
        if (a.find("anullsrc") != std::string::npos) found = true;
    }
    CHECK(found);
    // **这条路用 -shortest 是安全的**：静音源是无限长的，
    // 截到视频长度正是想要的结果。
    CHECK(has_flag(args, "-shortest"));
    CHECK(arg_value(args, "-c:v") == "copy");
}

TEST_CASE("烧字幕时路径里的冒号要转义") {
    // Windows 上 C:\x 里的冒号是滤镜的参数分隔符，反斜杠是转义符，
    // 直接传进去滤镜解析失败——而报错是滤镜语法错误，看不出根因是路径。
    const std::string e = media::escape_filter_path(
        paths::from_utf8("C:/项目 库/雨夜天台/subtitles/第一集.ass"));
    CAPTURE(e);
    CHECK(e.find("C\\:/") == 0);
    CHECK(e.find('\\') != std::string::npos);
    // 反斜杠已经换成正斜杠了，不该再有别的反斜杠
    CHECK(e.find("\\\\") == std::string::npos);

    const auto args = media::burn_args("in.mp4", paths::from_utf8("C:/a/b.ass"),
                                       config::AssemblyConfig{}, "out.mp4");
    const std::string vf = arg_value(args, "-vf");
    CAPTURE(vf);
    CHECK(vf.rfind("subtitles='", 0) == 0);
    CHECK(vf.back() == '\'');

    SUBCASE("烧字幕时音频原样拷贝") {
        CHECK(arg_value(args, "-c:a") == "copy");
    }
}

TEST_CASE("每条命令都带 -y") {
    // 不带的话 ffmpeg 遇到已存在的输出文件会问"覆盖吗"，
    // 而这个进程的 stdin 是关着的，它会一直等到超时。
    config::AssemblyConfig cfg;
    CHECK(has_flag(media::normalize_args("i.mp4", 640, 352, cfg, "o.mp4"), "-y"));
    CHECK(has_flag(media::concat_args("l.txt", "o.mp4"), "-y"));
    CHECK(has_flag(media::silent_audio_args("i.mp4", cfg, "o.mp4"), "-y"));
    CHECK(has_flag(media::mix_args("i.mp4", {{"a.wav", 0.0}}, cfg, -16.0, -1.5,
                                   5.0, "o.mp4"), "-y"));
    CHECK(has_flag(media::burn_args("i.mp4", "s.ass", cfg, "o.mp4"), "-y"));
}

TEST_CASE("装配前的字幕自检") {
    const auto paths = make_paths("自检");
    touch(paths.shots("draft") / "a.mp4");
    auto s = make_shot("sh001", 5.0, "shots/draft/a.mp4");
    s.dialogue.push_back(line("正常的一句", 2.0, "c_lin"));
    s.dialogue.push_back(line("太快了", 0.2, "c_lin"));

    const auto tl = media::build_timeline({s}, paths, config::AssemblyConfig{});
    const auto problems =
        media::subtitle_problems(tl, config::AssemblyConfig{});
    REQUIRE_FALSE(problems.empty());
    bool said = false;
    for (const auto& p : problems) {
        if (p.find("看不清") != std::string::npos) said = true;
    }
    CHECK(said);
}

TEST_CASE("路径里有单引号会破掉滤镜——已知缺陷，两边一样") {
    // `-vf subtitles='<路径>'` 是用单引号括起来的，路径里再出现一个单引号
    // 就会提前收尾，后面的字变成滤镜的其它参数，ffmpeg 报一句语法错误。
    //
    // **Python 侧是一样的**（assembly/assemble.py 的 _escape_filter_path
    // 只做了 `\` → `/` 和 `:` → `\:`），所以这不是移植漏了，
    // 是两边共有的一个洞。契约标准要求和 Python 一致，所以先不单方面改——
    // 单方面改了对拍会多一条不同，而那条"不同"其实是我们更对。
    //
    // 这条用例把现状钉住：**哪天有人只改一侧，它会立刻红**，
    // 提醒去看另一侧。真要修就两边一起修（阶段 8 删掉 Python 之后就只剩一侧）。
    //
    // 实际风险不高：项目名和集名基本是中文，中文里没有 ASCII 单引号。
    const std::string e =
        media::escape_filter_path(paths::from_utf8("C:/Bob's drama/a.ass"));
    CAPTURE(e);
    // 冒号转了，反斜杠换成了正斜杠，**单引号原样留着**——这就是那个洞。
    CHECK(e.find("C\\:/") == 0);
    CHECK(e.find('\'') != std::string::npos);
}
