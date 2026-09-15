// `[models.pick]`：这部剧要哪一档模型。
//
// 用户 2026-09-15 定的两条：**项目优先**（项目里写了就听项目的，全局那份
// 当默认值），以及缺档时**先自动下、下不了才把那台那一格灰掉**。这个文件
// 只钉第一条——配置层。
//
// 为什么要分出这一节：上面那些 `image` / `video` / `tts` 记的是**文件名**，
// 那是机器的属性（每台目录和文件名都不一样），只能放全局。而"要哪一档"是
// **剧**的属性，机器无关——档位 id 来自内置目录，每台都认得，各自去自己的
// 模型目录里找对应的文件。所以它能跟着项目目录走。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "config/settings.hpp"
#include "util/paths.hpp"

#include "scoped_env.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 写一份项目配置，回它的目录。
fs::path write_project(const std::string& tag, const std::string& body) {
    const auto dir = fs::temp_directory_path() / "changji_pick" / tag;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::ofstream(dir / "changji.toml") << body;
    return dir;
}

/// 往隔离出来的那份全局配置里写。
void write_global(const changji::test::ScopedUserConfigDir& iso,
                  const std::string& body) {
    std::error_code ec;
    const auto p = config::user_config_path();
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p) << body;
    (void)iso;
}

}  // namespace

TEST_CASE("项目里挑的那一档盖过全局") {
    changji::test::ScopedUserConfigDir iso("pick_wins");
    write_global(iso, "[models.pick]\nimage = \"全局挑的\"\n");
    const auto proj = write_project("wins", "[models.pick]\nimage = \"项目挑的\"\n");

    const auto s = config::load_settings(proj);
    CHECK(s.models.pick.at("image") == "项目挑的");
}

TEST_CASE("按键盖，不整份替换") {
    // ⚠️ **这一条是要害。** 项目多半只写了一两组；整份替换的话，项目里写
    // 一个 image 就把全局挑好的 llm / tts / video 全清空——而那几组清空之后
    // 走的是"从文件名反推"那条老路，表面上还能跑，直到某一组的文件名恰好
    // 不在目录里。
    changji::test::ScopedUserConfigDir iso("pick_merge");
    write_global(iso,
                 "[models.pick]\nllm = \"别动我\"\ntts = \"我也是\"\n");
    const auto proj =
        write_project("merge", "[models.pick]\nimage = \"项目只改了这一组\"\n");

    const auto s = config::load_settings(proj);
    CHECK(s.models.pick.at("image") == "项目只改了这一组");
    CHECK(s.models.pick.at("llm") == "别动我");
    CHECK(s.models.pick.at("tts") == "我也是");
    CHECK(s.models.pick.size() == 3);
}

TEST_CASE("不写这一节就什么都不动") {
    // 老项目一个字都不用改：没有 [models.pick] 时，"选了哪一档"照旧从文件名
    // 反推（那部分在 setup_api 的 current_option 里）。
    changji::test::ScopedUserConfigDir iso("pick_absent");
    write_global(iso, "[models.pick]\nllm = \"全局的\"\n");
    const auto proj = write_project("absent", "[video]\norientation = \"portrait\"\n");

    const auto s = config::load_settings(proj);
    CHECK(s.models.pick.size() == 1);
    CHECK(s.models.pick.at("llm") == "全局的");
}

TEST_CASE("不是字符串的值跳过，别把整节弄坏") {
    changji::test::ScopedUserConfigDir iso("pick_badtype");
    const auto proj =
        write_project("badtype", "[models.pick]\nimage = \"好的那个\"\nllm = 42\n");

    const auto s = config::load_settings(proj);
    CHECK(s.models.pick.at("image") == "好的那个");
    CHECK(s.models.pick.count("llm") == 0);
}

TEST_CASE("默认是空的——没挑过和挑了空串是两回事") {
    config::Settings s;
    CHECK(s.models.pick.empty());
}
