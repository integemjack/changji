// 跨平台的标准目录。
//
// 这是 Python 侧 platformdirs 的等价物，必须逐字节对齐它的结果——
// C++ 后端要能读到 Python 后端写的同一个 config.toml，否则迁移期间
// 用户会看到两套后端各有一份配置，改了这边那边不认。
//
// 对齐目标是 platformdirs 的 user_config_dir(appname, appauthor=False)
// 和 user_data_dir(appname, appauthor=False)。

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace changji::paths {

/// 用户全局配置目录。
/// Windows: %LOCALAPPDATA%\changji
/// macOS:   ~/Library/Application Support/changji
/// Linux:   $XDG_CONFIG_HOME/changji 或 ~/.config/changji
std::filesystem::path user_config_dir(const std::string& app_name);

/// 用户数据目录。
/// Windows: %LOCALAPPDATA%\changji
/// macOS:   ~/Library/Application Support/changji
/// Linux:   $XDG_DATA_HOME/changji 或 ~/.local/share/changji
std::filesystem::path user_data_dir(const std::string& app_name);

/// 展开开头的 ~。配置里的路径允许用户写 ~/短剧项目 这种形式。
std::filesystem::path expand_user(const std::string& raw);

/// 读环境变量。没有或为空返回空字符串。
/// Windows 上走宽字符再转回 UTF-8，否则中文路径会变成问号。
std::string env(const char* name);

/// 路径转 UTF-8 字符串。**任何时候都不要用 path.string() 代替它。**
///
/// 这是实测踩出来的：MSVC 上 fs::path 内部存宽字符，.string() 会用当前
/// ANSI 代码页做转换，遇到该代码页表示不了的字符就抛 std::system_error
/// （"No mapping for the Unicode character exists in the target multi-byte
/// code page"）。这台机器的代码页是 936（GBK），--doctor 就这样静默死掉——
/// 异常一路穿到 std::terminate，进程以 0xC0000409 消失，而那个错误码字面
/// 意思是"栈缓冲区溢出"，会把人往完全错误的方向带。
///
/// 项目目录本来就允许是 E:\AI短剧\ 这种路径，用户名也可能是中文，
/// 所以这不是边缘情况。这个函数走 wstring → UTF-8，不经过 ANSI 代码页，
/// 永远不会失败。
std::string to_utf8(const std::filesystem::path& p);

/// UTF-8 字符串转路径。**任何时候都不要用 fs::path(str) 代替它。**
///
/// 是 to_utf8 的反方向，坑也是对称的：MSVC 上 fs::path(std::string) 把窄字符串
/// 按当前 ANSI 代码页解释，而本项目里的 std::string 一律是 UTF-8。
/// UTF-8 的中文字节序列在 GBK 里往往是非法的，转换会抛 std::system_error。
///
/// 实测踩到的位置：proc::which() 里 `fs::path(dir)`，dir 来自 PATH 环境变量
/// （已经是 UTF-8）。只要 PATH 里有一个目录名带非 ASCII 字符，--doctor 就整个崩掉。
std::filesystem::path from_utf8(const std::string& s);

/// 命令行参数，**UTF-8 的**。
///
/// Windows 上 `char** argv` 是按当前 ANSI 代码页编的。一个中文路径
/// 从命令行传进来，直接当 UTF-8 用会得到一串乱码字节——
/// 拼进 URL 是 400，交给 fs::path 是异常，写进日志是问号。
/// 而这一切都不报错，只表现为"这个项目打不开"。
///
/// 这里从 GetCommandLineW 重新取一份宽字符的再转 UTF-8。
/// 其它平台 argv 本来就是 UTF-8，原样拷贝。
///
/// **每个 main() 的第一件事都该是调它。**
std::vector<std::string> utf8_args(int argc, char** argv);

}  // namespace changji::paths
