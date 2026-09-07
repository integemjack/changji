#include "util/paths.hpp"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
// CommandLineToArgvW 在这里，不在 windows.h
#include <shellapi.h>
#include <vector>
#endif

namespace changji::paths {

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

/// 宽字符转 UTF-8。
///
/// Windows 上不能直接用 getenv：它走 ANSI 代码页，中文路径会被
/// 转成问号。必须 GetEnvironmentVariableW 拿宽字符再自己转 UTF-8。
std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                  static_cast<int>(wide.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace
#endif

std::vector<std::string> utf8_args(int argc, char** argv) {
#ifdef _WIN32
    // argv 是按 ANSI 代码页编的。中文路径从命令行传进来，直接当 UTF-8 用
    // 会得到一串乱码字节——拼进 URL 是 400，交给 fs::path 是异常。
    // 而这一切都不报错，只表现为"这个项目打不开"。
    int wide_argc = 0;
    LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &wide_argc);
    if (wide != nullptr) {
        std::vector<std::string> out;
        out.reserve(static_cast<std::size_t>(wide_argc));
        for (int i = 0; i < wide_argc; ++i) out.push_back(to_utf8(std::wstring(wide[i])));
        ::LocalFree(wide);
        return out;
    }
    // 取不到宽版本时退回 argv。乱码总比一个参数都没有强。
#endif
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

std::filesystem::path from_utf8(const std::string& s) {
    // C++17 的 u8path 就是为这件事存在的（C++20 起被标记为弃用，
    // 替代品是 char8_t 那一套，但本项目全局停在 C++17，用它最直接）。
    return std::filesystem::u8path(s);
}

std::string to_utf8(const std::filesystem::path& p) {
#ifdef _WIN32
    return to_utf8(p.wstring());
#else
    return p.string();
#endif
}

std::string env(const char* name) {
#ifdef _WIN32
    std::wstring wname;
    for (const char* p = name; *p; ++p) wname.push_back(static_cast<wchar_t>(*p));
    DWORD need = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (need == 0) return {};
    std::vector<wchar_t> buf(need);
    DWORD got = ::GetEnvironmentVariableW(wname.c_str(), buf.data(), need);
    if (got == 0 || got >= need) return {};
    return to_utf8(std::wstring(buf.data(), got));
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

namespace {

fs::path home_dir() {
#ifdef _WIN32
    std::string p = env("USERPROFILE");
    if (!p.empty()) return from_utf8(p);
    return from_utf8(env("HOMEDRIVE") + env("HOMEPATH"));
#else
    std::string p = env("HOME");
    return p.empty() ? fs::path("/tmp") : from_utf8(p);
#endif
}

}  // namespace

fs::path user_config_dir(const std::string& app_name) {
#if defined(_WIN32)
    // platformdirs 的 user_config_dir 默认 roaming=False，落在 Local 而不是 Roaming
    std::string base = env("LOCALAPPDATA");
    if (base.empty()) return home_dir() / "AppData" / "Local" / app_name;
    return from_utf8(base) / app_name;
#elif defined(__APPLE__)
    return home_dir() / "Library" / "Application Support" / app_name;
#else
    std::string xdg = env("XDG_CONFIG_HOME");
    if (!xdg.empty()) return from_utf8(xdg) / app_name;
    return home_dir() / ".config" / app_name;
#endif
}

fs::path user_data_dir(const std::string& app_name) {
#if defined(_WIN32)
    std::string base = env("LOCALAPPDATA");
    if (base.empty()) return home_dir() / "AppData" / "Local" / app_name;
    return from_utf8(base) / app_name;
#elif defined(__APPLE__)
    return home_dir() / "Library" / "Application Support" / app_name;
#else
    std::string xdg = env("XDG_DATA_HOME");
    if (!xdg.empty()) return from_utf8(xdg) / app_name;
    return home_dir() / ".local" / "share" / app_name;
#endif
}

fs::path expand_user(const std::string& raw) {
    if (raw.empty()) return {};
    if (raw[0] != '~') return from_utf8(raw);
    if (raw.size() == 1) return home_dir();
    if (raw[1] == '/' || raw[1] == '\\') return home_dir() / from_utf8(raw.substr(2));
    return from_utf8(raw);  // ~someuser 这种形式不支持，原样返回
}

}  // namespace changji::paths
