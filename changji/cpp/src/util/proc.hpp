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
};

/// 在 PATH 里找可执行文件，返回绝对路径。找不到返回 nullopt。
/// 相当于 Unix 的 which / Windows 的 where。
std::optional<std::string> which(const std::string& name);

/// 跑一个命令并抓取输出。
///
/// 参数逐个传，**每个单独加引号**再拼成命令行交给 shell。
/// 调用方不用自己处理引号——路径里有空格或中文时，
/// 让调用方拼字符串是最经典的一类 bug。
///
/// ⚠️ 底下是 popen，也就是**真的经过一层 shell**。原来这里写的是
/// "不做 shell 拼接"，那是不准确的。之所以还能用，是因为每个参数都
/// 单独引过了；但要记得这一层的存在——比如 `launched` 的判断就不能
/// 靠 popen 的返回值，见下面。
///
/// timeout_ms 为 0 表示不限时。
Result run(const std::string& exe,
           const std::vector<std::string>& args,
           int timeout_ms = 15000);

}  // namespace changji::proc
