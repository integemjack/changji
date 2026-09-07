// 配置写回的测试。
//
// 这里最要紧的一条是**注释必须活下来**。内置的配置模板整个是注释写的，
// 每一节都在解释这一项是干什么的、改了会怎样。抹掉之后用户下次打开
// 配置文件看到的是一堆没有说明的键值对，而那时候他也不知道是谁抹的。
//
// Python 那边用 tomlkit 天生保留格式，toml++ 不保留——解析成 table
// 再序列化，注释全没。所以 C++ 侧走的是逐行编辑，那就得自己测这件事。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "config/writeback.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path tmp_dir(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_写回_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void put(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    REQUIRE(out.good());
    out << text;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// 写回之后再加载一遍，确认文件还是合法的、值也对。
config::Settings reload(const fs::path& dir) {
    return config::load_settings(dir);
}

}  // namespace

TEST_CASE("TOML 字面量的写法") {
    CHECK(config::to_toml_literal(json("abc")) == "\"abc\"");
    CHECK(config::to_toml_literal(json(true)) == "true");
    CHECK(config::to_toml_literal(json(false)) == "false");
    CHECK(config::to_toml_literal(json(42)) == "42");

    SUBCASE("浮点必须带小数点") {
        // 写成 `1` 的话下次读回来是整数，而字段类型是 float，
        // toml++ 取值时会拒绝——一次写回把整份配置写坏了。
        CHECK(config::to_toml_literal(json(1.0)) == "1.0");
        CHECK(config::to_toml_literal(json(60.0)) == "60.0");
        CHECK(config::to_toml_literal(json(0.25)) == "0.25");
    }

    SUBCASE("字符串里的引号和反斜杠要转义") {
        CHECK(config::to_toml_literal(json("a\"b")) == "\"a\\\"b\"");
        CHECK(config::to_toml_literal(json("C:\\models")) == "\"C:\\\\models\"");
    }

    SUBCASE("null 写成空串") {
        // TOML 没有 null。Python 那边写回时把 None 换成空串，照抄。
        CHECK(config::to_toml_literal(json(nullptr)) == "\"\"");
    }

    SUBCASE("中文原样") {
        CHECK(config::to_toml_literal(json("思源黑体")) == "\"思源黑体\"");
    }
}

TEST_CASE("改一个已有的键，注释一个不少") {
    const fs::path dir = tmp_dir("保注释");
    const fs::path cfg = dir / "changji.toml";
    put(cfg,
        "# 这是整个文件的说明\n"
        "\n"
        "[llm]\n"
        "# 剧本和分镜用的大模型。默认走本地 Ollama。\n"
        "base_url = \"http://127.0.0.1:11434/v1\"\n"
        "# 模型名要和服务上的一致\n"
        "model = \"qwen3:14b\"\n"
        "\n"
        "[assembly]\n"
        "fps = 24\n");

    config::save_user_config(json{{"llm", {{"model", "qwen3:32b"}}}}, cfg);
    const std::string after = slurp(cfg);

    // 改对了
    CHECK(after.find("model = \"qwen3:32b\"") != std::string::npos);
    CHECK(after.find("qwen3:14b") == std::string::npos);

    // 三条注释一条都不能少
    CHECK(after.find("# 这是整个文件的说明") != std::string::npos);
    CHECK(after.find("# 剧本和分镜用的大模型") != std::string::npos);
    CHECK(after.find("# 模型名要和服务上的一致") != std::string::npos);

    // 没被改的项一个字节都不动
    CHECK(after.find("base_url = \"http://127.0.0.1:11434/v1\"") !=
          std::string::npos);
    CHECK(after.find("fps = 24") != std::string::npos);

    // 还是合法配置
    const config::Settings s = reload(dir);
    CHECK(s.llm.model == "qwen3:32b");
    CHECK(s.assembly.fps == 24);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("加一个节里还没有的键，插在已有键后面") {
    // 插在节头后面的话，会挤在节的说明注释和第一个键之间，读起来断裂。
    const fs::path dir = tmp_dir("插新键");
    const fs::path cfg = dir / "changji.toml";
    put(cfg,
        "[llm]\n"
        "# 这一节讲的是大模型\n"
        "base_url = \"http://a/v1\"\n"
        "\n"
        "# 下面这段注释是在讲装配那一节\n"
        "[assembly]\n"
        "fps = 24\n");

    config::save_user_config(json{{"llm", {{"model", "新模型"}}}}, cfg);
    const std::string after = slurp(cfg);

    const std::size_t sec = after.find("# 这一节讲的是大模型");
    const std::size_t base = after.find("base_url");
    const std::size_t model = after.find("model = \"新模型\"");
    REQUIRE(sec != std::string::npos);
    REQUIRE(base != std::string::npos);
    REQUIRE(model != std::string::npos);
    // 顺序：节说明 -> base_url -> 新加的 model
    CHECK(sec < base);
    CHECK(base < model);
    // 而且要在下一节之前
    CHECK(model < after.find("[assembly]"));

    const config::Settings s = reload(dir);
    CHECK(s.llm.model == "新模型");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("整节都没有就追加到文件末尾") {
    const fs::path dir = tmp_dir("新节");
    const fs::path cfg = dir / "changji.toml";
    put(cfg, "[llm]\nbase_url = \"http://a/v1\"\n");

    config::save_user_config(
        json{{"models", {{"dir", "D:/模型"}, {"llm", "a.gguf"}}}}, cfg);
    const std::string after = slurp(cfg);

    CHECK(after.find("[models]") != std::string::npos);
    CHECK(after.find("dir = \"D:/模型\"") != std::string::npos);
    // 原来那节还在
    CHECK(after.find("[llm]") < after.find("[models]"));

    const config::Settings s = reload(dir);
    REQUIRE(s.models.dir.has_value());
    CHECK(*s.models.dir == "D:/模型");
    CHECK(s.models.llm == "a.gguf");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("顶层的键要插在第一个节头之前") {
    // 插在后面的话它会被算进那一节里，变成 [llm].vram_gb_override。
    const fs::path dir = tmp_dir("顶层键");
    const fs::path cfg = dir / "changji.toml";
    put(cfg, "# 开头的注释\n[llm]\nbase_url = \"http://a/v1\"\n");

    config::save_user_config(json{{"vram_gb_override", 15.9}}, cfg);
    const std::string after = slurp(cfg);

    const std::size_t key = after.find("vram_gb_override");
    REQUIRE(key != std::string::npos);
    CHECK(key < after.find("[llm]"));

    const config::Settings s = reload(dir);
    REQUIRE(s.vram_gb_override.has_value());
    CHECK(*s.vram_gb_override == doctest::Approx(15.9));

    SUBCASE("已经有的话就地改") {
        config::save_user_config(json{{"vram_gb_override", 8.0}}, cfg);
        const config::Settings s2 = reload(dir);
        REQUIRE(s2.vram_gb_override.has_value());
        CHECK(*s2.vram_gb_override == doctest::Approx(8.0));
        // 没变成两行
        const std::string t = slurp(cfg);
        CHECK(t.find("vram_gb_override") == t.rfind("vram_gb_override"));
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("注释掉的同名键不当成已有") {
    // 模板里很多项是注释掉的示例（`# model = "qwen3:14b"`）。
    // 把它当成已有键去改的话，改完还是注释，用户看不出哪里没生效。
    const fs::path dir = tmp_dir("注释键");
    const fs::path cfg = dir / "changji.toml";
    put(cfg,
        "[models]\n"
        "# dir = \"~/models\"\n"
        "# llm = \"Qwen3-14B-Q4_K_M.gguf\"\n");

    config::save_user_config(json{{"models", {{"llm", "真的.gguf"}}}}, cfg);
    const std::string after = slurp(cfg);

    // 注释那行原样留着
    CHECK(after.find("# llm = \"Qwen3-14B-Q4_K_M.gguf\"") != std::string::npos);
    // 真正生效的那行加进去了
    CHECK(after.find("\nllm = \"真的.gguf\"") != std::string::npos);

    const config::Settings s = reload(dir);
    CHECK(s.models.llm == "真的.gguf");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("文件不存在时从内置模板起步") {
    // 从零写一个只有两行的文件的话，用户第一次打开配置看到的是
    // 两个孤零零的键，不知道还能配什么。
    const fs::path dir = tmp_dir("新文件");
    const fs::path cfg = dir / "changji.toml";
    REQUIRE_FALSE(fs::exists(cfg));

    config::save_user_config(json{{"llm", {{"model", "新模型"}}}}, cfg);
    const std::string after = slurp(cfg);

    CHECK(after.find("model = \"新模型\"") != std::string::npos);
    // 模板里的注释和别的节也在
    CHECK(after.find('#') != std::string::npos);
    CHECK(after.find("[assembly]") != std::string::npos);
    CHECK(after.size() > 500);

    CHECK_NOTHROW(reload(dir));
    CHECK(reload(dir).llm.model == "新模型");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("CRLF 的文件改一行不会变成混合换行") {
    // 变成混合换行的话，用 git 管配置的人会看到整个文件都变了。
    const fs::path dir = tmp_dir("换行");
    const fs::path cfg = dir / "changji.toml";
    put(cfg, "[llm]\r\nbase_url = \"http://a/v1\"\r\nmodel = \"old\"\r\n");

    config::save_user_config(json{{"llm", {{"model", "new"}}}}, cfg);
    const std::string after = slurp(cfg);

    // 每一个 \n 前面都得有 \r
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (after[i] == '\n') {
            CAPTURE(i);
            REQUIRE(i > 0);
            CHECK(after[i - 1] == '\r');
        }
    }
    CHECK(reload(dir).llm.model == "new");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("一次改好几节") {
    const fs::path dir = tmp_dir("多节");
    const fs::path cfg = dir / "changji.toml";
    put(cfg,
        "[comfy]\nbase_url = \"http://old:8188\"\n\n"
        "[llm]\nmodel = \"old\"\n\n"
        "[assembly]\nfps = 24\ncrf = 18\n");

    config::save_user_config(json{
        {"comfy", {{"base_url", "http://new:8188"}}},
        {"llm", {{"model", "new"}, {"temperature", 0.3}}},
        {"assembly", {{"fps", 30}}},
        {"vram_gb_override", 12.0},
    }, cfg);

    const config::Settings s = reload(dir);
    CHECK(s.comfy.base_url == "http://new:8188");
    CHECK(s.llm.model == "new");
    CHECK(s.llm.temperature == doctest::Approx(0.3));
    CHECK(s.assembly.fps == 30);
    CHECK(s.assembly.crf == 18);   // 没动的还在
    REQUIRE(s.vram_gb_override.has_value());
    CHECK(*s.vram_gb_override == doctest::Approx(12.0));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("改完的文件再改一次还是对的") {
    // 写回是幂等的，而且第二次改不会把第一次的搞乱。
    const fs::path dir = tmp_dir("连改");
    const fs::path cfg = dir / "changji.toml";
    put(cfg, "[llm]\n# 说明\nmodel = \"a\"\n");

    for (int i = 0; i < 5; ++i) {
        config::save_user_config(
            json{{"llm", {{"model", "m" + std::to_string(i)}}}}, cfg);
    }
    const std::string after = slurp(cfg);
    CHECK(after.find("model = \"m4\"") != std::string::npos);
    // 只有一行 model
    std::size_t count = 0, pos = 0;
    while ((pos = after.find("\nmodel = ", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    CHECK(count == 1);
    CHECK(after.find("# 说明") != std::string::npos);
    CHECK(reload(dir).llm.model == "m4");

    std::error_code ec;
    fs::remove_all(dir, ec);
}
