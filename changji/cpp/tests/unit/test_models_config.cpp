// [models] 配置的测试。
//
// 这一节 Python 侧没有对应，所以不是对拍——是纯粹的新逻辑，
// 唯一要测的是路径怎么解析。
//
// 路径这块值得测是因为它有三条分支（绝对、相对、~ 开头）而且错了不会立刻
// 报错：解析出一个不存在的路径，程序照常启动，直到真去加载模型时才炸，
// 那时候错误信息只会说"文件打不开"，看不出是解析错了还是文件没下。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "config/settings.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

TEST_CASE("模型路径解析") {
    config::ModelsConfig m;
    const fs::path ws = paths::from_utf8("C:/工作区");

    SUBCASE("空的返回空") {
        // 没配就是没配，不能返回一个"目录/"这样的半截路径——
        // 那种路径 is_regular_file 会返回 false，看起来像"文件不存在"，
        // 掩盖了"根本没配"这件事。
        CHECK(m.resolve("", ws).empty());
    }

    SUBCASE("相对路径接在 dir 后面") {
        m.dir = "D:/模型";
        CHECK(m.resolve("a.gguf", ws) == paths::from_utf8("D:/模型/a.gguf"));
        // 带子目录的相对路径
        CHECK(m.resolve("wan/b.gguf", ws) == paths::from_utf8("D:/模型/wan/b.gguf"));
    }

    SUBCASE("绝对路径原样用，不拼 dir") {
        // 多台机器共享网络盘时会这么填
        m.dir = "D:/模型";
        const fs::path abs = paths::from_utf8("E:/共享/c.gguf");
        CHECK(m.resolve(paths::to_utf8(abs), ws) == abs);
    }

    SUBCASE("dir 留空时回落到项目库下的 models") {
        CHECK(m.dir_path(ws) == ws / "models");
        CHECK(m.resolve("a.gguf", ws) == ws / "models" / "a.gguf");
    }

    SUBCASE("dir 是空串等同于没填") {
        m.dir = "";
        CHECK(m.dir_path(ws) == ws / "models");
    }

    SUBCASE("~ 会展开") {
        m.dir = "~/模型";
        const fs::path got = m.dir_path(ws);
        CHECK(got.is_absolute());
        // 展开之后不该还留着波浪号
        CHECK(paths::to_utf8(got).find('~') == std::string::npos);

        // 条目本身以 ~ 开头也要展开，而且不能被当成相对路径拼到 dir 后面
        const fs::path entry = m.resolve("~/单独放的.gguf", ws);
        CHECK(entry.is_absolute());
        CHECK(paths::to_utf8(entry).find("模型") == std::string::npos);
    }
}

TEST_CASE("模型配置不做存在性校验") {
    // 存在性检查在 doctor 里，不在这里。
    //
    // 理由：模型动辄好几个 G，装好程序还没下模型是常态。
    // 如果配置加载阶段就因为文件不在而失败，用户连界面都进不去，
    // 也就没法在界面里看到到底缺哪个文件——只能盯着一行启动错误猜。
    config::ModelsConfig m;
    m.llm = "根本不存在的文件.gguf";
    m.dir = "Z:/这个盘也不存在";
    CHECK(m.validate().empty());

    config::Settings s;
    s.models = m;
    CHECK(s.validate().empty());
}

TEST_CASE("模型配置能从 toml 读出来") {
    const fs::path tmp = fs::temp_directory_path() /
                         paths::from_utf8("changji_模型配置");
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    const fs::path cfg = tmp / "changji.toml";
    {
        std::ofstream out(cfg, std::ios::binary);
        REQUIRE(out.good());
        out << "[models]\n"
               "dir = \"D:/模型库\"\n"
               "llm = \"Qwen3-14B-Q4_K_M.gguf\"\n"
               "video = \"Wan2.2-TI2V-5B-Q4_K_M.gguf\"\n"
               "video_vae = \"Wan2.2_VAE.safetensors\"\n"
               "video_text_encoder = \"umt5-xxl-encoder-Q5_K_M.gguf\"\n"
               "image = \"Qwen-Image-Edit-Q4_K_M.gguf\"\n";
    }

    const config::Settings s = config::load_settings(tmp);
    REQUIRE(s.models.dir.has_value());
    CHECK(*s.models.dir == "D:/模型库");
    CHECK(s.models.llm == "Qwen3-14B-Q4_K_M.gguf");
    CHECK(s.models.video == "Wan2.2-TI2V-5B-Q4_K_M.gguf");
    CHECK(s.models.video_vae == "Wan2.2_VAE.safetensors");
    CHECK(s.models.video_text_encoder == "umt5-xxl-encoder-Q5_K_M.gguf");
    CHECK(s.models.image == "Qwen-Image-Edit-Q4_K_M.gguf");

    // 中文目录名要能一路走到底而不乱码。MSVC 上 fs::path 和 std::string
    // 之间用错转换函数的话，这里会变成问号或者直接抛异常。
    CHECK(s.models.resolve(s.models.llm, tmp) ==
          paths::from_utf8("D:/模型库/Qwen3-14B-Q4_K_M.gguf"));

    SUBCASE("没写的项保持空，不会被填上猜的默认值") {
        const fs::path cfg2 = tmp / "changji.toml";
        {
            std::ofstream out(cfg2, std::ios::binary);
            out << "[models]\nllm = \"only-this.gguf\"\n";
        }
        const config::Settings s2 = config::load_settings(tmp);
        CHECK(s2.models.llm == "only-this.gguf");
        // 猜默认文件名只会让人以为配好了，然后在加载模型时才炸
        CHECK(s2.models.video.empty());
        CHECK(s2.models.image.empty());
        CHECK_FALSE(s2.models.dir.has_value());
    }

    fs::remove_all(tmp, ec);
}
