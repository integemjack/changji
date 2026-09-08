// util/paths 里几个和路径解析有关的函数。
//
// 用 tools/coverage_audit.py 查出来的：expand_user / user_config_dir /
// user_data_dir 一个符号都没被测试碰过。
//
// **expand_user 尤其值得测**：`--init-config` 生成的配置模板里就写着
// `# dir = "~/models"`，用户照着填是最自然的做法。展开错了的表现是
// "模型明明在那儿，程序说找不到"——而报出来的路径看着还挺像回事。

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "scoped_env.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 当前平台上"家目录"那个环境变量的名字。
#ifdef _WIN32
constexpr const char* kHomeVar = "USERPROFILE";
#else
constexpr const char* kHomeVar = "HOME";
#endif

}  // namespace

TEST_CASE("expand_user：~ 开头的展开成家目录") {
    const fs::path fake = fs::temp_directory_path() / "changji_fake_home";
    const test::ScopedEnv guard(kHomeVar, paths::to_utf8(fake));

    CHECK(paths::expand_user("~") == fake);
    CHECK(paths::expand_user("~/models") == fake / "models");
    // 配置模板里写的就是这一行，跑一遍真的。
    CHECK(paths::expand_user("~/models") == fake / "models");
#ifdef _WIN32
    // Windows 上用户多半写反斜杠。
    CHECK(paths::expand_user("~\\models") == fake / "models");
#endif
    // 多层也要对。
    CHECK(paths::expand_user("~/models/tts/talker.gguf") ==
          fake / "models" / "tts" / "talker.gguf");
}

TEST_CASE("expand_user：不是 ~ 开头的原样给回去") {
    CHECK(paths::expand_user("models/a.gguf") == fs::path("models/a.gguf"));
    CHECK(paths::expand_user("").empty());
    // 绝对路径不能被动过。
    const std::string abs =
#ifdef _WIN32
        "C:/models/a.gguf";
#else
        "/models/a.gguf";
#endif
    CHECK(paths::expand_user(abs) == fs::path(abs));
}

TEST_CASE("expand_user：~someuser 这种不支持，原样返回而不是猜") {
    // 展开成"家目录的爹再拼一个名字"是 shell 的行为，这里没有用户数据库
    // 可查。猜错了会指到一个不存在的地方，而报错只会说"找不到文件"。
    // 原样返回至少让人看得出自己写了什么。
    CHECK(paths::expand_user("~other/models") == fs::path("~other/models"));
    CHECK(paths::expand_user("~other") == fs::path("~other"));
}

TEST_CASE("expand_user：中文路径不会被 ANSI 代码页毁掉") {
    // 这个项目的路径基本都是中文。expand_user 内部走 from_utf8，
    // 而 MSVC 上 fs::path(std::string) 是按 ANSI 解的——直接用会抛
    // "No mapping for the Unicode character"。
    const fs::path fake = fs::temp_directory_path() /
                          paths::from_utf8("changji_家目录_测试");
    const test::ScopedEnv guard(kHomeVar, paths::to_utf8(fake));
    const fs::path got = paths::expand_user("~/模型/首帧.gguf");
    CHECK(got == fake / paths::from_utf8("模型") / paths::from_utf8("首帧.gguf"));
}

TEST_CASE("user_config_dir 和 user_data_dir 带上应用名，而且不一样") {
    const fs::path cfg = paths::user_config_dir("changji");
    const fs::path data = paths::user_data_dir("changji");
    CHECK(paths::to_utf8(cfg).find("changji") != std::string::npos);
    CHECK(paths::to_utf8(data).find("changji") != std::string::npos);
    CHECK(cfg.is_absolute());
    CHECK(data.is_absolute());
}

TEST_CASE("user_config_dir 在 Windows 上是 LocalAppData 不是 Roaming") {
    // **踩过一次**：download_tts_gguf.ps1 里让用户把配置贴到
    // %APPDATA%\changji\config.toml（那是 Roaming），而程序读的是
    // LocalAppData。贴过去 changji 根本不读，症状是
    // "配置贴了还是说模型没配"。
#ifdef _WIN32
    const std::string cfg = paths::to_utf8(paths::user_config_dir("changji"));
    CAPTURE(cfg);
    CHECK(cfg.find("Local") != std::string::npos);
    CHECK(cfg.find("Roaming") == std::string::npos);
#endif
}
