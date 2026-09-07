// changji —— AI 短剧生产流水线，C++ 后端。
//
// 阶段 0：骨架。只有 /api/health、/api/doctor 和一个 WebSocket 端点，
// 目的是把工具链和跨平台构建先跑通，别等写了两万行才发现某个依赖
// 在树莓派上编不过。

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include "config/settings.hpp"
#include "doctor/doctor.hpp"
#include "http/server.hpp"
#include "util/paths.hpp"

namespace {

void print_usage() {
    std::cout <<
        "用法: changji [选项]\n"
        "\n"
        "  --port <n>       监听端口，默认 8080\n"
        "  --host <addr>    监听地址，默认 0.0.0.0\n"
        "  --doctor         在命令行跑一遍环境体检然后退出\n"
        "  --init-config    生成一份带注释的配置模板然后退出\n"
        "  --help           显示这段话\n"
        "\n"
        "配置优先级：环境变量 > 项目目录的 changji.toml > 用户全局配置 > 内置默认值\n";
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
        std::cout << c.detail << "\n";
        if (!c.fix.empty()) {
            size_t pos = 0;
            while (pos <= c.fix.size()) {
                size_t nl = c.fix.find('\n', pos);
                if (nl == std::string::npos) nl = c.fix.size();
                std::cout << "        " << c.fix.substr(pos, nl - pos) << "\n";
                pos = nl + 1;
            }
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
    bool want_init = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " 后面要跟" << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { print_usage(); return 0; }
        else if (a == "--port") opts.port = std::atoi(next("端口号").c_str());
        else if (a == "--host") opts.host = next("监听地址");
        else if (a == "--doctor") want_doctor = true;
        else if (a == "--init-config") want_init = true;
        else {
            std::cerr << "不认识的选项：" << a << "\n\n";
            print_usage();
            return 2;
        }
    }

    if (want_init) {
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

    if (want_doctor) return run_doctor(settings);

    changji::http::run(settings, opts);
    return 0;
}

}  // namespace
