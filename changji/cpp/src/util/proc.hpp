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
    bool launched = false;  ///< 进程有没有起来。区分「跑了但失败」和「根本没这个程序」
};

/// 在 PATH 里找可执行文件，返回绝对路径。找不到返回 nullopt。
/// 相当于 Unix 的 which / Windows 的 where。
std::optional<std::string> which(const std::string& name);

/// 跑一个命令并抓取输出。
///
/// 参数逐个传，不做 shell 拼接——路径里有空格或中文时，
/// 拼字符串再交给 shell 是最经典的一类 bug。
/// timeout_ms 为 0 表示不限时。
Result run(const std::string& exe,
           const std::vector<std::string>& args,
           int timeout_ms = 15000);

}  // namespace changji::proc
