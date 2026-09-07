#include "util/proc.hpp"

#include <array>
#include <cstdio>
#include <filesystem>

#include "util/paths.hpp"

#ifdef _WIN32
#include <windows.h>
#define POPEN  _popen
#define PCLOSE _pclose
#else
#include <unistd.h>
#define POPEN  popen
#define PCLOSE pclose
#endif

namespace changji::proc {

namespace fs = std::filesystem;

namespace {

/// 给参数加引号。
///
/// 这里必须用引号而不是直接拼接：项目路径经常带中文和空格
/// （比如 E:\AI短剧\），不加引号会被 shell 拆成多个参数。
std::string quote(const std::string& s) {
    if (s.empty()) return "\"\"";
    bool needs = s.find_first_of(" \t\"'&|<>()") != std::string::npos;
    if (!needs) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\";
        out += c;
    }
    out += "\"";
    return out;
}

}  // namespace

std::optional<std::string> which(const std::string& name) {
    // 已经是个能直接用的路径就不用找了
    std::error_code ec;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        if (fs::exists(name, ec)) return name;
    }

    std::string path_env = paths::env("PATH");
    if (path_env.empty()) return std::nullopt;

#ifdef _WIN32
    const char sep = ';';
    // PATHEXT 决定哪些后缀算可执行。写死 .exe 会漏掉 .bat 包装的工具，
    // ffmpeg 的某些安装方式就是这样。
    std::vector<std::string> exts;
    std::string pathext = paths::env("PATHEXT");
    if (pathext.empty()) pathext = ".COM;.EXE;.BAT;.CMD";
    {
        size_t start = 0;
        while (start <= pathext.size()) {
            size_t end = pathext.find(';', start);
            if (end == std::string::npos) end = pathext.size();
            std::string e = pathext.substr(start, end - start);
            if (!e.empty()) exts.push_back(e);
            start = end + 1;
        }
    }
    exts.push_back("");  // 名字里已经带后缀的情况
#else
    const char sep = ':';
    const std::vector<std::string> exts{""};
#endif

    size_t start = 0;
    while (start <= path_env.size()) {
        size_t end = path_env.find(sep, start);
        if (end == std::string::npos) end = path_env.size();
        std::string dir = path_env.substr(start, end - start);
        start = end + 1;
        if (dir.empty()) continue;

        for (const auto& ext : exts) {
            fs::path candidate = paths::from_utf8(dir) / paths::from_utf8(name + ext);
            if (fs::is_regular_file(candidate, ec)) {
                return paths::to_utf8(candidate);
            }
        }
    }
    return std::nullopt;
}

Result run(const std::string& exe, const std::vector<std::string>& args, int timeout_ms) {
    Result r;

    std::string cmd = quote(exe);
    for (const auto& a : args) cmd += " " + quote(a);
    // stderr 并进 stdout。ffmpeg -version 和 nvidia-smi 的报错都走 stderr，
    // 只读 stdout 会得到一片空白然后误判成「程序不存在」。
    cmd += " 2>&1";

#ifdef _WIN32
    // cmd.exe 对整条命令行的解析规则：最外层再包一层引号，
    // 否则 exe 路径带空格时前半段会被当成命令、后半段当成参数。
    cmd = "\"" + cmd + "\"";
#endif

    // TODO(阶段 1): popen 没法设超时，卡死的子进程会把工作线程一起拖住。
    // 现在只用来跑 ffmpeg -version 这类秒回的命令，风险可控。
    // 装配环节接进来之前必须换成 CreateProcess / fork+waitpid 加超时。
    (void)timeout_ms;

    std::FILE* pipe = POPEN(cmd.c_str(), "r");
    if (!pipe) return r;
    r.launched = true;

    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        r.out += buf.data();
        // 输出无上限的话，遇到 ffmpeg 这种能刷几十 MB 日志的程序会吃光内存
        if (r.out.size() > 1u << 20) {
            r.out += "\n...(输出过长，已截断)\n";
            break;
        }
    }
    r.exit_code = PCLOSE(pipe);
    return r;
}

}  // namespace changji::proc
