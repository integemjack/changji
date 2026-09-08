// 子进程与可执行文件查找。
//
// 体检要跑 ffmpeg -version、nvidia-smi、fc-list；后面的装配环节
// 整个都是拉起 ffmpeg。先把这层抽出来，免得每处各写一遍管道。

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace changji::proc {

struct Result {
    int exit_code = -1;
    std::string out;   ///< stdout 和 stderr 合并。ffmpeg 习惯把版本信息写到 stderr
    /// 进程有没有起来。区分「跑了但失败」和「根本没这个程序」。
    ///
    /// **靠先查一遍 PATH 得出，不是靠 popen 的返回值**——popen 只要
    /// shell 起得来就算成功，命令不存在它也返回非空。
    /// `media/ffmpeg.cpp` 靠这个标志决定说"找不到 ffmpeg"还是
    /// "ffmpeg 报错了"，两句话把人指向完全不同的方向。
    bool launched = false;

    /// 超时被杀掉了。
    ///
    /// 原来 popen 根本没法设超时——一个卡死的 ffmpeg 会把整个工作线程钉住，
    /// 而外面看到的只是"这一集一直在跑"，没有任何别的信号。
    /// 换成 CreateProcessW / fork 之后才有了这一项。
    bool timed_out = false;
};

/// 在 PATH 里找可执行文件，返回绝对路径。找不到返回 nullopt。
/// 相当于 Unix 的 which / Windows 的 where。
std::optional<std::string> which(const std::string& name);

/// 跑一个命令并抓取输出。
///
/// 参数逐个传。**不经过 shell**：Windows 上是 CreateProcessW，
/// 别处是 fork + execv。
///
/// 原来走的是 popen（也就是交给 cmd.exe / sh），换掉是因为踩了两次：
/// cmd 把 `,` `;` `=` 也当参数分隔符，ffmpeg 的滤镜串会被切碎；
/// 而且 popen 没法设超时。不经过 shell 之后这两类问题都不存在了。
///
/// timeout_ms 为 0 表示不限时；超时会杀掉子进程并把 `timed_out` 置位。
Result run(const std::string& exe,
           const std::vector<std::string>& args,
           int timeout_ms = 15000);

}  // namespace changji::proc
