#include "util/proc.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

#include "util/paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace changji::proc {

namespace fs = std::filesystem;

namespace {

/// 给参数加引号。
///
/// 这里必须用引号而不是直接拼接：项目路径经常带中文和空格
/// （比如 E:\AI短剧\），不加引号会被 shell 拆成多个参数。
///
/// ⚠️ **`,` `;` `=` 也是 cmd.exe 的参数分隔符**，不是只有空格。
/// 原来的判断里没有它们，后果是 ffmpeg 的滤镜串**每一条都会被切碎**：
///
///     scale=640:-2,setsar=1,fps=24
///
/// 这一串不带空格、不带引号，原来会原样拼进命令行，然后 cmd 在 `=` 和
/// `,` 上切开，ffmpeg 收到的是七八个碎片。`media/assemble.cpp` 拼的
/// 每一个 `-vf` / `-filter_complex` 参数都是这个形状。
///
/// 这是拿 `@echo [%~1]` 做回显、真跑一遍才看出来的——第一版用的是 `%*`
/// （回显整条参数串），那验不出"被切成几个"，因为拼回去的字还是那些。
///
/// 还没处理的一个：**`%`**。cmd 就算在引号里也会展开 `%VAR%`。
/// 现在的路径和滤镜里都不会出现成对的 `%`，先记在这儿。
std::string quote(const std::string& s) {
    if (s.empty()) return "\"\"";
    bool needs = s.find_first_of(" \t\"'&|<>(),;=^") != std::string::npos;
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

    // **先确认这个程序存在。**
    //
    // 下面走的是 popen，而 popen 是把命令交给 shell 跑的——**只要 shell
    // 起得来它就成功**，哪怕命令根本不存在。所以不先查一下的话，
    // `launched` 永远是 true，而头文件里说它"区分「跑了但失败」和
    // 「根本没这个程序」"。
    //
    // 这不是纸面问题：`media/ffmpeg.cpp` 就是靠 `!launched` 抛
    // FFmpegMissing（"找不到 ffmpeg。在配置里填 assembly.ffmpeg_path"）的。
    // 少了这一步，没装 ffmpeg 的人拿到的是"ffmpeg 报错了"外加一句
    // shell 的"不是内部或外部命令"——指不到该去装或者去填路径。
    if (!which(exe).has_value()) return r;  // launched 保持 false

    std::string cmd = quote(exe);
    for (const auto& a : args) cmd += " " + quote(a);
    // stderr 并进 stdout。ffmpeg -version 和 nvidia-smi 的报错都走 stderr，
    // 只读 stdout 会得到一片空白然后误判成「程序不存在」。
    cmd += " 2>&1";

    // TODO(阶段 1): popen 没法设超时，卡死的子进程会把工作线程一起拖住。
    // 现在只用来跑 ffmpeg -version 这类秒回的命令，风险可控。
    // 装配环节接进来之前必须换成 CreateProcess / fork+waitpid 加超时。
    (void)timeout_ms;

#ifdef _WIN32
    // **必须走 _wpopen（宽字符），不能用 _popen。**
    //
    // _popen 是窄接口，而我们手上的 cmd 是 UTF-8。把 UTF-8 字节喂给它，
    // 只要路径里有非 ASCII 就会被按当前 ANSI 代码页重新解释——
    // 这个项目的项目名和模型目录**基本都是中文**。
    // 实测症状：`'"C:\...\changji 鐢妇鈹栭弽鑲╂畱...\tool.bat"' is not
    // recognized as an internal or external command`。
    //
    // 原来这里还在最外层多包了一层引号，注释说是为了处理 exe 路径带空格。
    // **那一层反而是坏的**：cmd.exe 见到开头两个连续引号会把程序名解析成
    // 空的，于是带空格的路径一个都跑不起来。每个部分已经单独引过了，
    // 不需要再包。两个问题都是拿一个真的带空格、带中文的路径去跑才露出来的。
    const std::wstring wcmd = paths::from_utf8(cmd).wstring();
    std::FILE* pipe = _wpopen(wcmd.c_str(), L"r");
#else
    std::FILE* pipe = popen(cmd.c_str(), "r");
#endif
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
#ifdef _WIN32
    r.exit_code = _pclose(pipe);
#else
    r.exit_code = pclose(pipe);
#endif
    return r;
}

}  // namespace changji::proc
