// util/proc 的测试：PATH 查找和跑子进程。
//
// 这一层原来没有直接测试，而**体检和整个装配环节都压在它上面**：
// ffmpeg、ffprobe、nvidia-smi、fc-list 全靠它拉起来。
//
// 最要紧的一条是 `Result::launched`——它区分"跑了但失败"和"根本没这个
// 程序"。体检就是靠它决定报"没装 ffmpeg（去装）"还是"ffmpeg 报错了
// （看看这段输出）"。这两句话把人指向完全不同的方向，弄反了很费时间。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "scoped_env.hpp"
#include "util/paths.hpp"
#include "util/proc.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 建一个临时目录，里面放一个"可执行文件"。
/// 只验 which 找不找得到，不真跑它，所以内容无所谓。
fs::path make_fake_bin(const std::string& filename) {
    const fs::path dir = fs::temp_directory_path() / "changji_which_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::ofstream(dir / filename) << "echo hi\n";
    return dir;
}

}  // namespace

TEST_CASE("which：在 PATH 里找得到") {
#ifdef _WIN32
    const std::string file = "changji_fake_tool.bat";
#else
    const std::string file = "changji_fake_tool";
#endif
    const fs::path dir = make_fake_bin(file);
    const test::ScopedEnv guard("PATH", paths::to_utf8(dir));

#ifdef _WIN32
    // **不带后缀也要找得到。** Windows 上靠 PATHEXT 补后缀，
    // 写死 .exe 会漏掉 .bat 包装的工具——ffmpeg 的某些安装方式就是这样，
    // 那时候体检会报"没装 ffmpeg"而它其实装了。
    const test::ScopedEnv pathext("PATHEXT", ".COM;.EXE;.BAT;.CMD");
    const auto found = proc::which("changji_fake_tool");
#else
    const auto found = proc::which("changji_fake_tool");
#endif
    REQUIRE(found.has_value());
    CHECK(found->find("changji_fake_tool") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("which：找不到就是 nullopt，不是空串") {
    // 空串会被调用方当成"找到了一个名字为空的程序"，然后拿去 run。
    //
    // **PATH 指向一个空目录，而不是清空 PATH。** 清空的话同一个进程里
    // 别的用例会跟着遭殃——第一版就是这么写的，结果 test_readonly 里
    // 那条显卡检测挂了（nvidia-smi 找不到，gpu 变成 null），
    // 而报错看起来跟 proc 毫无关系。
    const fs::path empty = fs::temp_directory_path() / "changji_empty_bin";
    std::error_code ec;
    fs::create_directories(empty, ec);
    const test::ScopedEnv guard("PATH", paths::to_utf8(empty));
    CHECK_FALSE(proc::which("changji_这个程序不存在_12345").has_value());
    fs::remove_all(empty, ec);
}

TEST_CASE("which：已经是路径就直接用，不去翻 PATH") {
    const fs::path dir = make_fake_bin("tool.txt");
    const std::string full = paths::to_utf8(dir / "tool.txt");
    // PATH 指向一个空目录，证明它没去翻 PATH（理由同上一条）。
    const fs::path empty = fs::temp_directory_path() / "changji_empty_bin2";
    std::error_code ec2;
    fs::create_directories(empty, ec2);
    const test::ScopedEnv guard("PATH", paths::to_utf8(empty));
    const auto found = proc::which(full);
    REQUIRE(found.has_value());
    CHECK(*found == full);

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(empty, ec);
}

TEST_CASE("run：程序不存在时 launched 为假，而不是「跑了但退出码非零」") {
    // **这一条是这个文件里最重要的。** 体检靠 launched 决定说哪句话：
    //   launched == false → "没装，去装"
    //   launched == true 而 exit_code != 0 → "装了但报错，看这段输出"
    // 弄反了的话，用户会照着"去装"再装一遍已经装好的东西。
    const auto r = proc::run("changji_这个程序不存在_12345", {"--version"});
    CHECK_FALSE(r.launched);
    CHECK(r.exit_code != 0);
}

TEST_CASE("run：跑得起来的命令，输出和退出码都对") {
    // **挑命令要迁就 run 的实现**：它先 which(exe) 再交给 popen，
    // 所以 exe 必须是 PATH 里查得到的**真文件**。
    // Windows 上 `echo` 是 cmd 的内建命令、不是文件，which 找不到它——
    // 第一版就是这么挂的。改用 where.exe，它一定在，而且输出可预期。
#ifdef _WIN32
    const auto r = proc::run("where.exe", {"cmd"});
    CHECK(r.launched);
    CHECK(r.exit_code == 0);
    // where cmd 会打出 cmd.exe 的完整路径。
    CHECK(r.out.find("cmd") != std::string::npos);
#else
    const auto r = proc::run("echo", {"changji_proc_ok"});
    CHECK(r.launched);
    CHECK(r.exit_code == 0);
    CHECK(r.out.find("changji_proc_ok") != std::string::npos);
#endif
}

TEST_CASE("run：跑起来了但失败，launched 仍然为真") {
    // 「跑起来了但失败」和「没这个程序」是两件事——上一条测前者，
    // 「程序不存在」那条测后者。两者都靠 launched 区分。
#ifdef _WIN32
    // where 查一个不存在的名字：退出码非零，但它自己是跑起来了的。
    const auto r = proc::run("where.exe", {"changji_不存在_98765"});
#else
    const auto r = proc::run("cat", {"/changji_不存在_98765"});
#endif
    CHECK(r.launched);
    CHECK(r.exit_code != 0);
}

TEST_CASE("run：路径里有空格也跑得起来") {
    // 路径里有空格（"C:\\Program Files\\" 那种）是常态。拼命令行时不给每个部分单独
    // 加引号，前半段会被当成命令、后半段当成参数。
    //
    // **拿一个真的带空格的路径去跑**，而不是去看输出里参数有没有被切开
    // ——第一版想用 where.exe 的报错文字来判断，但它不回显搜的那个名字，
    // 那条断言其实什么都没验到。能观察到的才算数。
    const fs::path dir = fs::temp_directory_path() / "changji 带空格的目录";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
#ifdef _WIN32
    const fs::path tool = dir / "tool.bat";
    { std::ofstream f(tool); f << "@echo changji_space_ok\n"; }
#else
    const fs::path tool = dir / "tool.sh";
    { std::ofstream f(tool); f << "#!/bin/sh\necho changji_space_ok\n"; }
    fs::permissions(tool, fs::perms::owner_all, ec);
#endif

    const auto r = proc::run(paths::to_utf8(tool), {});
    CHECK(r.launched);
    CHECK_MESSAGE(r.out.find("changji_space_ok") != std::string::npos,
                  "exit=" << r.exit_code << " out=[" << r.out << "]");
    CHECK(r.exit_code == 0);

    fs::remove_all(dir, ec);
}
