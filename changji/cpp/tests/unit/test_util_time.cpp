// util/human_time 和 util/fs_time 的测试。
//
// 这两个文件加起来 57 行，原来一条测试都没有——**而它们错了都不报错**，
// 只是显示出来的数字不对。

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "util/fs_time.hpp"
#include "util/human_time.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json load_golden() {
    const fs::path p = fs::path(CHANGJI_GOLDEN_DIR) / "human_time.json";
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到 " << paths::to_utf8(p));
    return json::parse(in);
}

}  // namespace

TEST_CASE("human_time 逐条对上 Python 的输出") {
    // **期望值是真的调了一遍 Python 录下来的**（tools/gen_human_time_golden.py），
    // 不是手写的。手写的话最容易写成小学四舍五入，而 Python 的 f-string
    // `:.0f` 用的是 IEEE 754 的就近取偶：
    //   0.5 → "0 秒"、1.5 → "2 秒"、2.5 → "2 秒"
    //
    // C++ 这边靠 snprintf("%.0f") 拿到同样的行为。这件事**只有在恰好落在
    // .5 上才看得出来**，随便挑几个数是测不到的——所以语料里专门挑了那几个点。
    for (const auto& c : load_golden().at("cases")) {
        const double seconds = c.at("seconds").get<double>();
        const std::string want = c.at("text").get<std::string>();
        CAPTURE(seconds);
        CHECK(util::human_time(seconds) == want);
    }
}

TEST_CASE("human_time 负数当零，不出现「-3 秒」") {
    CHECK(util::human_time(-1e9) == "0 秒");
}

TEST_CASE("file_mtime_unix 返回的是 Unix 纪元的秒，不是 1601") {
    // **这一条盯的是一个差 369 年的 bug。**
    //
    // MSVC 上 `file_time_type` 的纪元是 1601-01-01，直接拿
    // `time_since_epoch()` 会比 Unix 时间大 11644473600 秒。
    // 那个数不会让任何东西崩，只会让"这个文件是什么时候改的"全错，
    // 而且所有文件一起错，**排序看起来还是对的**——最难发现的那种。
    const fs::path dir = fs::temp_directory_path() / "changji_fs_time_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path f = dir / "a.txt";
    { std::ofstream(f) << "x"; }

    const double now = static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const double got = util::file_mtime_unix(f);

    // 刚写完的文件，时间应该就在现在附近。给 5 分钟的余量：
    // 够宽松到不受时钟精度和文件系统粒度影响，又远小于 369 年。
    CHECK(got > now - 300);
    CHECK(got < now + 300);

    fs::remove_all(dir, ec);
}

TEST_CASE("file_mtime_unix 对不存在的文件返回 0，不抛") {
    // 调用方拿它排序和显示，为一个缺文件抛异常会把整个列表打断。
    CHECK(util::file_mtime_unix("这个文件不存在_98765.txt") == 0.0);
}
