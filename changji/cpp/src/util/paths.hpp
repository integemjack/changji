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

}  // namespace changji::paths
