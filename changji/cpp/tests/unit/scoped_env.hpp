#pragma once

// 测试里临时改环境变量，析构时还原。
//
// 单独一个头文件是因为已经有两处要用（配置的环境变量覆盖、`which` 找
// PATH），各写一份迟早会分家——而这种小工具分了家最难发现：
// 两份行为不一样时，出问题的是"另一个文件里的测试莫名其妙地挂了"。
//
// ⚠️ **Windows 上必须走宽字符 API，不能用 `_putenv_s`。**
//
// `_putenv_s` 是窄（ANSI）接口。而 `paths::env()` 返回的是 **UTF-8**，
// 把 UTF-8 字节喂给 ANSI 接口，只要值里有非 ASCII 就会被解坏——
// 这台机器的 PATH 里就有中文目录名。实测：存下 2266 字符的 PATH，
// 原样还原之后只剩 1448 字符。
//
// 后果不是"这条用例挂了"，而是**它之后的每条用例都在一个坏掉的 PATH 上跑**：
// where.exe 找不到、nvidia-smi 找不到，于是 test_readonly 里的显卡检测
// 变成 null 然后报"和 Python 不一致"。**报错的地方离原因隔了两个文件。**
//
// 第一版就是这么写的，上面那段是查出来之后补的。

#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

#include "util/paths.hpp"

namespace changji::test {

class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value)
        : name_(std::move(name)) {
        old_ = paths::env(name_.c_str());
        had_ = !old_.empty();
        set(value);
    }
    ~ScopedEnv() { set(had_ ? old_ : std::string()); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void set(const std::string& v) {
#ifdef _WIN32
        // 借 fs::path 做 UTF-8 → 宽字符的转换：paths::from_utf8 已经
        // 把 MSVC 上那个"窄字符串按 ANSI 解"的坑处理过了。
        const std::wstring wname = paths::from_utf8(name_).wstring();
        if (v.empty()) {
            ::SetEnvironmentVariableW(wname.c_str(), nullptr);
            return;
        }
        const std::wstring wval = paths::from_utf8(v).wstring();
        ::SetEnvironmentVariableW(wname.c_str(), wval.c_str());
#else
        if (v.empty()) {
            ::unsetenv(name_.c_str());
        } else {
            ::setenv(name_.c_str(), v.c_str(), 1);
        }
#endif
    }
    std::string name_;
    std::string old_;
    bool had_ = false;
};

}  // namespace changji::test
