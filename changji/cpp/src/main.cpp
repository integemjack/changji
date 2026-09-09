// changji —— AI 短剧生产流水线，C++ 后端。
//
// 阶段 0：骨架。只有 /api/health、/api/doctor 和一个 WebSocket 端点，
// 目的是把工具链和跨平台构建先跑通，别等写了两万行才发现某个依赖
// 在树莓派上编不过。

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <iostream>
#include <string>

#include "infer/worker_server.hpp"
#include "config/settings.hpp"
#include "infer/llama_tts.hpp"
#include "doctor/doctor.hpp"
#include "http/server.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace {

/// 解析 --port 的参数。
///
/// **不用 std::atoi。** 它解不出来就返回 0，而 0 在 bind 里是合法的——
/// 意思是"操作系统随便挑一个空闲端口"。于是 `--port abc` 会静默地
/// 起在一个随机端口上，而用户以为它在自己敲的那个上面。
///
/// 更常见的是打错一个字符：`--port 8O80`（字母 O）被 atoi 解成 **8**，
/// 服务起在 8 端口，用户连 8080 连不上，而程序一声没吭。
/// 这一类"静默地理解成别的意思"是最难查的，因为症状离原因很远。
int parse_port(const std::string& raw) {
    const bool digits =
        !raw.empty() && std::all_of(raw.begin(), raw.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
    long v = digits ? std::strtol(raw.c_str(), nullptr, 10) : -1;
    if (!digits || v < 1 || v > 65535) {
        std::cerr << "--port 要一个 1 到 65535 的整数，收到的是「" << raw << "」\n";
        std::exit(2);
    }
    return static_cast<int>(v);
}

void print_usage() {
    std::cout <<
        "用法: changji [选项]\n"
        "\n"
        "  --port <n>            监听端口，默认 8080\n"
        "  --host <addr>         监听地址，默认 127.0.0.1（只有本机连得上）\n"
        "                        要让局域网连进来才填 0.0.0.0——这套接口没有\n"
        "                        鉴权，连上就能读项目、改分镜、起流水线\n"
        "  --doctor              跑一遍环境体检然后退出\n"
        "\n"
        "  当工作进程跑（多卡时一张卡起一个，见方案「多卡和多机」）：\n"
        "  --worker              以工作进程模式起，只算不发接口\n"
        "  --gpu <n>             绑第几张卡，默认 0\n"
        "  --init-config         生成一份带注释的配置模板然后退出\n"
        "  --force               配合 --init-config：已有配置也照样覆盖\n"
        "  --help                显示这段话\n"
        "\n"
        "  用进程内配音念一句然后退出（不用起服务，也不用建项目）：\n"
        "  --say <文本>          要念的话\n"
        "  --out <文件>          写到哪儿，默认 say.wav\n"
        "  --voice <音频>        参考音色，一段人声片段（wav/mp3/flac）。\n"
        "                        不给就用模型自带的\n"
        "  --tts-model <文件>    临时指定骨干，盖过配置里的 [models].tts\n"
        "  --tts-decoder <文件>  临时指定解码器，盖过 [models].tts_decoder\n"
        "\n"
        "配置优先级：环境变量 > 项目目录的 changji.toml > 用户全局配置 > 内置默认值\n";
}


/// `--say`：用进程内配音念一句，写成 wav。
///
/// **阶段 9 的实机判据就是这一条。** 那条合成路径代码写完很久了，
/// 但机器上一直没有 Qwen3-TTS 的权重，所以一次都没执行过。
/// 权重到位之后，这一条命令是最短的验证路径——不用起服务、不用建项目、
/// 不用 ffmpeg，出不出得了声一句话就知道。
///
/// 报错要说清楚是**哪一步**断的：没编进来（改构建）、没配路径（改配置）、
/// 载不起来（文件不对或显存不够）、合成失败（模型或参数）。
/// 只说一句"配音失败"的话，用户下一步该干什么全靠猜。
int run_say(const changji::config::Settings& settings, const std::string& text,
            const std::string& voice, const std::string& out,
            const std::string& model_override,
            const std::string& decoder_override) {
    using namespace changji;

    if (!infer::llama_tts_available()) {
        std::cerr << "这个二进制没编进程内配音。\n"
                     "  用 -DCHANGJI_LLAMA=ON 重新配置构建。\n";
        return 1;
    }

    const auto ws = settings.workspace_path();
    const auto backbone =
        model_override.empty() ? settings.models.resolve(settings.models.tts, ws)
                               : paths::from_utf8(model_override);
    const auto decoder =
        decoder_override.empty()
            ? settings.models.resolve(settings.models.tts_decoder, ws)
            : paths::from_utf8(decoder_override);
    if (backbone.empty() || decoder.empty()) {
        std::cerr << "配置里缺模型路径。要这两项：\n"
                     "  [models].tts          骨干（qwen-talker-*.gguf）\n"
                     "  [models].tts_decoder  解码器（qwen-tokenizer-12hz-*.gguf）\n"
                     "两份都没有的话跑一遍 download_tts_gguf.ps1。\n"
                     "也可以不改配置，直接用 --tts-model / --tts-decoder 指过来。\n";
        return 1;
    }

    std::cout << "骨干   " << paths::to_utf8(backbone) << "\n"
              << "解码器 " << paths::to_utf8(decoder) << "\n"
              << "载入中（1 GB 出头，第一次会慢）…\n";

    std::string why;
    auto engine = infer::LlamaTts::load(backbone, decoder, /*use_gpu=*/true, why);
    if (!engine) {
        std::cerr << "载不起来：" << why << "\n";
        return 1;
    }
    std::cout << "载好了，采样率 " << engine->sample_rate() << " Hz\n";

    infer::LlamaTtsRequest req;
    req.text = text;
    req.out = paths::from_utf8(out);
    if (!voice.empty()) req.speaker_ref = paths::from_utf8(voice);

    double seconds = 0;
    if (!engine->synthesize(req, seconds, why)) {
        std::cerr << "合成失败：" << why << "\n";
        return 1;
    }

    std::cout << "出声了：" << out << "，" << seconds << " 秒\n";
    // 这一条是给判据用的：静音检测那套判据（见 stages/tts_backends.hpp）
    // 认为 1.05 秒以下多半是空音频。这里只提醒，不当失败——
    // 念一个字本来就可能不到一秒。
    if (seconds < 1.05) {
        std::cout << "  ⚠️ 不到 1.05 秒。流水线里的静音检测会把这种当空音频拦下来。\n"
                     "     念一句长一点的再看看是不是真的出声了。\n";
    }
    return 0;
}

/// UTF-8 字符串占多少个终端列。
///
/// 不能按字节数算：一个汉字是 3 个字节但只占 2 列，按字节对齐会歪得很离谱。
/// 也不能按字符数算：汉字占 2 列而 ASCII 占 1 列。
/// 正确做法是跳过续字节（0b10xxxxxx），每个首字节按是否 ASCII 计 1 或 2 列。
size_t display_width(const std::string& s) {
    size_t w = 0;
    for (unsigned char ch : s) {
        if ((ch & 0xC0) == 0x80) continue;  // UTF-8 续字节，不占列
        w += (ch & 0x80) ? 2 : 1;           // 非 ASCII 一律按全角算
    }
    return w;
}

/// 命令行跑体检。首次部署时最常用的一条命令，
/// 不用先起服务再开浏览器就能知道缺什么。
int run_doctor(const changji::config::Settings& settings) {
    auto report = changji::doctor::run_checks(settings);

    size_t width = 0;
    for (const auto& c : report.checks) {
        width = std::max(width, display_width(c.name));
    }

    for (const auto& c : report.checks) {
        const char* symbol = c.level == changji::doctor::Level::OK     ? "✓"
                             : c.level == changji::doctor::Level::WARN ? "!"
                                                                       : "✗";
        std::cout << "  " << symbol << "  " << c.name;
        for (size_t i = display_width(c.name); i < width + 2; ++i) std::cout << ' ';
        // detail 也可能是多行的（"出图后端"那一项带着 sd.cpp 的
        // System Info）。不缩进的话它会顶格贴在报告中间，把对齐冲掉。
        std::cout << changji::text::indent_rest(c.detail, "        ") << "\n";
        if (!c.fix.empty()) {
            std::cout << "        "
                      << changji::text::indent_rest(c.fix, "        ") << "\n";
        }
    }
    std::cout << "\n";

    size_t failed = 0, warned = 0;
    for (const auto& c : report.checks) {
        if (c.level == changji::doctor::Level::FAIL) ++failed;
        if (c.level == changji::doctor::Level::WARN) ++warned;
    }
    if (failed) {
        std::cout << "有 " << failed << " 项必须先解决才能出片。\n";
    } else if (warned) {
        std::cout << "可以跑，但有 " << warned << " 项建议处理。\n";
    } else {
        std::cout << "一切就绪。\n";
    }
    return failed ? 1 : 0;
}

}  // namespace

namespace {

/// 真正的入口。main 只负责把异常兜住。
int run(int argc, char** argv);

}  // namespace

int main(int argc, char** argv) {
    // 顶层兜异常。没有它的话，任何漏出来的异常会走 std::terminate 到 abort，
    // 在 Windows 上表现为进程以 0xC0000409 消失、一个字都不打印，
    // 而那个错误码字面意思是"栈缓冲区溢出"，会把人往完全错误的方向带。
    // （这是实测踩到的：--doctor 在 MSVC 下就这样静默死掉。）
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "出错了：" << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "出错了：未知异常\n";
        return 1;
    }
}

namespace {

int run(int argc, char** argv) {
    changji::http::Options opts;
    bool want_doctor = false;
    bool want_worker = false;
    int worker_gpu = 0;
    bool want_force = false;
    bool want_init = false;

    // Windows 上 argv 是按 ANSI 代码页编的，中文参数直接用是乱码。
    // 现在的选项都是 ASCII 所以碰不上，但将来加 --config <路径> 就会踩——
    // 而那时候的表现是"配置文件找不到"，看不出是参数被编码毁了。
    // 对拍程序就是这么栽的一次，见 tests/compat/main.cpp。
    const std::vector<std::string> av = changji::paths::utf8_args(argc, argv);

    // --say 那一组。**这是阶段 9 唯一的实机判据**：进程内配音那条路
    // 有没有真的能出声，不跑一次是不知道的（代码写完了但一直没有权重）。
    // 做成命令行而不是接口，是因为它要在"整条流水线还跑不起来"的时候
    // 就能单独验——出一集要模型、要 ffmpeg，那些是另外的坎。
    std::string say_text, say_voice, say_model, say_decoder;
    std::string say_out = "say.wav";

    for (std::size_t i = 1; i < av.size(); ++i) {
        const std::string& a = av[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= av.size()) {
                std::cerr << a << " 后面要跟" << what << "\n";
                std::exit(2);
            }
            return av[++i];
        };
        if (a == "--help" || a == "-h") { print_usage(); return 0; }
        else if (a == "--port") opts.port = parse_port(next("端口号"));
        else if (a == "--host") opts.host = next("监听地址");
        else if (a == "--worker") want_worker = true;
        else if (a == "--gpu") worker_gpu = std::atoi(next("显卡序号").c_str());
        else if (a == "--doctor") want_doctor = true;
        else if (a == "--say") say_text = next("要念的话");
        else if (a == "--out") say_out = next("输出文件名");
        else if (a == "--voice") say_voice = next("参考音色文件");
        // **两个临时覆盖。** 没有它们的话，想拿 --say 试一份刚下好的模型
        // 就得先去改配置文件——而"改了配置去试，试完再改回来"这件事
        // 本身就容易忘记改回来。
        else if (a == "--tts-model") say_model = next("骨干模型文件");
        else if (a == "--tts-decoder") say_decoder = next("解码器文件");
        else if (a == "--init-config") want_init = true;
        else if (a == "--force") want_force = true;
        else {
            std::cerr << "不认识的选项：" << a << "\n\n";
            print_usage();
            return 2;
        }
    }

    if (want_init) {
        // **已经有配置就不覆盖。**
        //
        // write_default_config() 是无条件截断写的。一个已经配好模型路径、
        // 工作区、后端地址的用户，只是想看看模板长什么样而敲了 --init-config，
        // 结果那些全没了——而且没有任何提示，因为命令"成功"了。
        //
        // 这不是假想：写这段之前我自己就是拿它当"看一眼默认配置在哪"的
        // 探针用的，一敲就把人家的配置盖了。
        const auto existing = changji::config::user_config_path();
        std::error_code ec;
        if (!want_force && std::filesystem::exists(existing, ec)) {
            std::cerr << "配置文件已经在了：" << changji::paths::to_utf8(existing)
                      << "\n"
                         "  不覆盖。真要重新生成一份就加 --force（原来那份会没）。\n";
            return 1;
        }
        try {
            auto p = changji::config::write_default_config();
            std::cout << "配置模板已写入 " << changji::paths::to_utf8(p) << "\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }

    changji::config::Settings settings;
    try {
        settings = changji::config::load_settings();
    } catch (const std::exception& e) {
        // 配置解析失败是致命的，而且消息里带着是哪个文件第几行。
        // 用默认值硬撑会让用户以为配置生效了。
        std::cerr << e.what() << "\n";
        return 1;
    }

    auto errs = settings.validate();
    if (!errs.empty()) {
        std::cerr << "配置有问题：\n";
        for (const auto& e : errs) std::cerr << "  - " << e << "\n";
        return 1;
    }

    if (!say_text.empty()) {
        return run_say(settings, say_text, say_voice, say_out, say_model,
                       say_decoder);
    }

    // **协调者也绑卡。** 不绑的话 llama.cpp 默认把配音模型摊到所有卡上，
    // 实测比只用一张慢 39%（PCIe 上的通信开销比省下的算力多），
    // 而且会和每一个 worker 抢显存。配 [workers].gpu 就绑。
    if (!want_worker && settings.workers.gpu >= 0) {
        changji::paths::set_env("CUDA_VISIBLE_DEVICES",
                                std::to_string(settings.workers.gpu));
    }

    if (want_worker) {

        // 工作进程：只算，不发接口、不碰项目文件。

        // 见方案「多卡和多机怎么用起来」。

        changji::infer::WorkerOptions wo;

        wo.port = opts.port;

        wo.host = opts.host;

        wo.gpu = worker_gpu;

        changji::infer::run_worker(settings, wo);

        return 0;

    }


    if (want_doctor) return run_doctor(settings);

    changji::http::run(settings, opts);
    return 0;
}

}  // namespace
