// [models] 配置的测试。
//
// 这一节 Python 侧没有对应，所以不是对拍——是纯粹的新逻辑，
// 唯一要测的是路径怎么解析。
//
// 路径这块值得测是因为它有三条分支（绝对、相对、~ 开头）而且错了不会立刻
// 报错：解析出一个不存在的路径，程序照常启动，直到真去加载模型时才炸，
// 那时候错误信息只会说"文件打不开"，看不出是解析错了还是文件没下。

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

#include "scoped_env.hpp"
#include "config/settings.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;

namespace {

/// 一条这个平台上真正算绝对的路径，带中文。
///
/// **不能写死 `D:/模型`。** 那在 Windows 上是绝对路径，在 Linux 上是
/// 相对路径——`resolve` 会把它接到工作区后面，断言全错。而这几条用例
/// 钉的是"绝对路径原样用、相对路径接 dir 后面"，和盘符没关系。
/// 中文目录名保留：那是这几条的另一半意图，中文一路走到底不能乱码。
std::string abs_utf8(const char* tail) {
#ifdef _WIN32
    return std::string("D:/") + tail;
#else
    return std::string("/mnt/") + tail;
#endif
}

}  // namespace

TEST_CASE("模型路径解析") {
    config::ModelsConfig m;
    const fs::path ws = paths::from_utf8(abs_utf8("工作区"));

    SUBCASE("空的返回空") {
        // 没配就是没配，不能返回一个"目录/"这样的半截路径——
        // 那种路径 is_regular_file 会返回 false，看起来像"文件不存在"，
        // 掩盖了"根本没配"这件事。
        CHECK(m.resolve("", ws).empty());
    }

    SUBCASE("相对路径接在 dir 后面") {
        m.dir = abs_utf8("模型");
        CHECK(m.resolve("a.gguf", ws) == paths::from_utf8(abs_utf8("模型/a.gguf")));
        // 带子目录的相对路径
        CHECK(m.resolve("wan/b.gguf", ws) ==
              paths::from_utf8(abs_utf8("模型/wan/b.gguf")));
    }

    SUBCASE("绝对路径原样用，不拼 dir") {
        // 多台机器共享网络盘时会这么填
        m.dir = abs_utf8("模型");
        const fs::path abs = paths::from_utf8(abs_utf8("共享/c.gguf"));
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
            << "dir = \"" << abs_utf8("模型库") << "\"\n"
            << "llm = \"Qwen3-14B-Q4_K_M.gguf\"\n"
            << "video = \"Wan2.2-TI2V-5B-Q4_K_M.gguf\"\n"
            << "video_vae = \"Wan2.2_VAE.safetensors\"\n"
            << "video_text_encoder = \"umt5-xxl-encoder-Q5_K_M.gguf\"\n"
            << "image = \"Qwen-Image-Edit-Q4_K_M.gguf\"\n";
    }

    const config::Settings s = config::load_settings(tmp);
    REQUIRE(s.models.dir.has_value());
    CHECK(*s.models.dir == abs_utf8("模型库"));
    CHECK(s.models.llm == "Qwen3-14B-Q4_K_M.gguf");
    CHECK(s.models.video == "Wan2.2-TI2V-5B-Q4_K_M.gguf");
    CHECK(s.models.video_vae == "Wan2.2_VAE.safetensors");
    CHECK(s.models.video_text_encoder == "umt5-xxl-encoder-Q5_K_M.gguf");
    CHECK(s.models.image == "Qwen-Image-Edit-Q4_K_M.gguf");

    // 中文目录名要能一路走到底而不乱码。MSVC 上 fs::path 和 std::string
    // 之间用错转换函数的话，这里会变成问号或者直接抛异常。
    CHECK(s.models.resolve(s.models.llm, tmp) ==
          paths::from_utf8(abs_utf8("模型库/Qwen3-14B-Q4_K_M.gguf")));

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

// ── 环境变量覆盖 ─────────────────────────────────────────────────────
//
// **这一组防的是"两处不同步"。** 一个环境变量要在两个地方各写一遍：
// `env_mapping()` 那张表（决定 /api/connections 的 env_locked 里报不报它，
// 界面靠这个把输入框置灰）和 `apply_env()`（决定它到底生不生效）。
//
// 只加了表：界面说"被环境变量锁住了"，而值其实没被覆盖。
// 只加了 apply_env：值覆盖了，界面还让人编辑，改完悄悄丢掉。
// **两种都不会报错，都只能靠人发现。**
//
// CHANGJI_MODELS_ENGINE 就是补出来的——别的 [models] 键都有，
// 偏偏这个"走进程内还是走 ComfyUI"的开关漏了，
// 而它正是容器里和对拍时最需要临时翻的一个。

// ScopedEnv 挪去 scoped_env.hpp 了：which 那边也要用，各写一份迟早分家。

TEST_CASE("环境变量：改了值就要生效，而且要在 env_locked 里报出来") {
    struct Case {
        const char* var;      ///< 不带 CHANGJI_ 前缀
        const char* key;      ///< env_locked 里的键名
        const char* value;
    };
    // 挑的是这次新加的三个。老的那些同理，出问题的方式一模一样。
    const Case cases[] = {
        {"MODELS_ENGINE", "models_engine", "sd"},
        {"MODELS_TTS", "models_tts", "some-talker.gguf"},
        {"MODELS_TTS_DECODER", "models_tts_decoder", "some-decoder.gguf"},
    };

    for (const auto& c : cases) {
        CAPTURE(c.var);
        const test::ScopedEnv guard(std::string("CHANGJI_") + c.var, c.value);

        // 一、表里有它，界面才知道该把输入框置灰。
        const auto locked = config::env_overridden();
        CHECK_MESSAGE(locked.count(c.key) == 1,
                      "env_mapping() 里少了这一项，界面不会显示它被锁住");

        // 二、值真的被覆盖了。
        const config::Settings s = config::load_settings();
        const std::string got =
            std::string(c.key) == "models_engine"      ? s.models.engine
            : std::string(c.key) == "models_tts"       ? s.models.tts
                                                       : s.models.tts_decoder;
        CHECK_MESSAGE(got == c.value,
                      "apply_env() 里少了这一项，值没被覆盖");
    }
}

TEST_CASE("环境变量把 engine 写错了要被拦住，不能悄悄接受") {
    // 悄悄接受的话整条出片的路会走岔，而表现是"连不上 ComfyUI"或者
    // "没编进出图后端"——两句话都指不到真正的原因（环境变量拼错了）。
    const test::ScopedEnv guard("CHANGJI_MODELS_ENGINE", "sdcpp");
    const config::Settings s = config::load_settings();
    CHECK(s.models.engine == "sdcpp");
    const auto errs = s.models.validate();
    REQUIRE_FALSE(errs.empty());
    // ComfyUI 拆掉之后 engine 只剩 sd，而**老配置填 comfy 要给迁移说明**，
    // 不能只说"只能是 sd"——用户不知道自己那套工作流该怎么办。
    CHECK(errs[0].find("sd") != std::string::npos);
}

TEST_CASE("flash attention 默认开，配置里能关") {
    // sd.cpp 的 sd_ctx_params_init 把它设成 false，而方案第二节选 sd.cpp 的
    // 理由里就列着 --diffusion-fa。6 GB 卡上这一项直接影响塞不塞得下，
    // 不该靠用户自己想起来加。
    CHECK(config::ModelsConfig{}.diffusion_flash_attn);

    const fs::path tmp = fs::temp_directory_path() / "changji_fa_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    {
        std::ofstream f(tmp / "changji.toml", std::ios::binary);
        f << "[models]\ndiffusion_flash_attn = false\n";
    }
    CHECK_FALSE(config::load_settings(tmp).models.diffusion_flash_attn);
    fs::remove_all(tmp, ec);
}

TEST_CASE("默认配置模板里 [models] 必须是注释掉的") {
    // **不是风格问题，是 Python 后端起不起得来的问题。**
    //
    // Python 引擎的 Settings 是 extra="forbid"，只要用户配置里出现 [models]，
    // 它整份加载失败（"Extra inputs are not permitted"），后端根本起不来。
    // 而迁移期间两个后端共用这一份文件。
    //
    // 这条用例是踩过之后加的：我拿 --init-config 当"看一眼配置在哪"的探针，
    // 一敲就把模板写进了用户配置，然后对拍里 152 条全变成
    // "Python 侧：连不上"——而那看起来像是网络或端口的问题。
    const fs::path tmp = fs::temp_directory_path() / "changji_tpl_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const fs::path f = tmp / "config.toml";
    config::write_default_config(f);

    std::ifstream in(f, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    // 行首的 [models] 就是生效的节头；"# [models]" 是注释，不算。
    CHECK(text.find("\n[models]") == std::string::npos);
    CHECK(text.find("\nengine = \"sd\"") == std::string::npos);
    // 但内容要还在，只是注释掉——不然用户不知道有这一节可以填。
    CHECK(text.find("# [models]") != std::string::npos);
    fs::remove_all(tmp, ec);
}


// ---------------------------------------------------------------------------
// 配置模板必须提到代码真的认的每一个值。
//
// **这一条是被一个真 bug 逼出来的。** 模板里 `[tts] backend` 的注释写着
// "填 comfy……填 http……"，只列了两个值，而代码认三个——漏掉的正是
// `local`：阶段 9 做的进程内配音，`--say` 验过真能出声。
// `[models]` 那一节也没有 `tts` / `tts_decoder` 这两个键，
// 而它们是 local 那条路必须填的。
//
// 后果不是"少个功能"，是**把人推去装一个根本不需要的东西**：
// 用户跑 `--init-config`（这是他做的第一件事），照着模板只能选 comfy 或
// http，于是去装 34 GB 的 ComfyUI 或者另起一个配音服务，
// 而这台机器上的这个二进制自己就能出声。
//
// 单元测试看不见这种错——模板是注释，改错了每个值仍然解析得动、
// 每条断言仍然是绿的。所以这里改成**拿代码认的值去查模板**：
// 以后再加一个后端而不写进模板，这条就会红。
// ---------------------------------------------------------------------------

namespace {

/// 从模板里切出一节：从 `[名字]` 那行到下一个顶格的 `[` 为止。
///
/// **整份模板做子串搜索是不行的**，第一版就栽在这上面：查 `"tts"`
/// 撞上 `[tts]` 这个节名本身，查 `image` 撞上 `workflows/image.json`，
/// 两条用例从头到尾没验过任何东西——把模板改回漏掉 local 的样子，
/// 它们照样全绿。
std::string toml_section(const std::string& tpl, const std::string& name) {
    // [models] 那一节在模板里**整个是注释掉的**（默认走 Python 引擎，
    // 取消注释才切到进程内推理），所以两种开头都要认。
    const std::string head = "[" + name + "]";
    std::string opener = head;
    std::size_t i = tpl.find("\n" + opener);
    if (i == std::string::npos) {
        opener = "# " + head;
        i = tpl.find("\n" + opener);
        if (i == std::string::npos) return {};
    }
    i += 1;
    std::size_t j = i + opener.size();
    while (true) {
        const std::size_t nl = tpl.find('\n', j);
        if (nl == std::string::npos) return tpl.substr(i);
        // 下一节的开头：顶格的 '[' 或者顶格的 "# ["
        const bool bare = nl + 1 < tpl.size() && tpl[nl + 1] == '[';
        const bool commented = tpl.compare(nl + 1, 3, "# [") == 0;
        if (bare || commented) return tpl.substr(i, nl - i);
        j = nl + 1;
    }
}

}  // namespace

TEST_CASE("模板的 [tts] 那一节提到了代码认的每一个后端") {
    // 代码里真的分派到的三个值：doctor/needs.cpp、http/run_deps.cpp
    const std::string sec =
        toml_section(config::default_config_template(), "tts");
    REQUIRE_FALSE(sec.empty());

    for (const char* backend : {"local", "http"}) {
        CAPTURE(backend);
        CHECK_MESSAGE(sec.find(backend) != std::string::npos,
                      "[tts] 那一节里没提 " << backend
                      << " —— 用户照着模板配就不会知道有这条路");
    }
}

TEST_CASE("模板的 [models] 那一节列出了每一个会被读的键") {
    // settings.cpp 里 take(t, "...", ...) 那一串。
    // **按 `键 = ` 的形状找**，不是找到这个词就算——散文里提一嘴不等于
    // 给了用户一行可以取消注释就用的东西。
    const std::string sec =
        toml_section(config::default_config_template(), "models");
    REQUIRE_FALSE(sec.empty());

    for (const char* key : {"dir", "engine", "llm", "video", "video_vae",
                            "video_text_encoder", "image", "image_vae",
                            "image_text_encoder", "image_text_encoder_vision",
                            "tts", "tts_decoder", "weights",
                            "video_cfg", "video_flow_shift",
                            "image_cfg", "image_flow_shift", "frame_tier",
                            "frame_steps",
                            "vram_reserve_gb", "video_high_noise",
                            "video_moe_boundary", "video_llm",
                            "video_llm_vision", "video_audio_vae",
                            "video_rng", "video_lora",
                            "video_lora_strength", "video_vae_tile",
                            "vae_vram_min_gb", "video_lora_tiers",
                            "image_weights"}) {
        CAPTURE(key);
        // 模板里这一节整个是注释掉的，所以形状是 `# 键 = `
        const std::string want = std::string("# ") + key + " = ";
        CHECK_MESSAGE(sec.find(want) != std::string::npos,
                      "[models] 里没有 `" << want << "` 这一行");
    }
}

TEST_CASE("模板自己解析得动，而且解析出来就是默认值") {
    // 模板是用户拿到的第一个文件。它要是解析不动，或者解析出来和默认值
    // 不一样，那"生成一份模板"这件事本身就是在骗人。
    const fs::path dir = fs::temp_directory_path() /
                         paths::from_utf8("changji_模板往返");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream f(dir / "changji.toml", std::ios::binary);
        f << config::default_config_template();
    }
    const auto s2 = config::load_settings(dir);
    const config::Settings def;
    CHECK(s2.tts.backend == def.tts.backend);
    CHECK(s2.llm.model == def.llm.model);
    CHECK(s2.models.engine == def.models.engine);
}

TEST_CASE("[models].weights：默认 smart，认 auto，别的拒") {
    // **默认 smart（2026-09-10 起）**：按模型多大、卡多大自己算，换卡不用改。
    // 原来默认 cpu，是"6 GB 卡上能跑"的保守选择，但代价是大卡上一直走慢路
    // 且没提示——用户："都应该让程序自己算。"smart 在小卡上自己会退回 cpu。
    CHECK(config::ModelsConfig{}.weights == "smart");

    const fs::path tmp = fs::temp_directory_path() / "changji_weights_cfg";
    std::error_code ec;
    fs::create_directories(tmp, ec);
    {
        std::ofstream f(tmp / "changji.toml", std::ios::binary);
        f << "[models]\nweights = \"auto\"\n";
    }
    CHECK(config::load_settings(tmp).models.weights == "auto");

    // 第三种取值：sd.cpp 的组件规格，原样传下去。
    // **给"差一点就装得下"的卡用的**：5090 上 fp8 图像模型用 auto 会连
    // 编码器一起塞进显存，挤不下 VAE 解码那 6.6 GB；只把编码器和 VAE
    // 放内存就够了，而编码器只在采样前跑一次，放内存几乎不影响速度。
    {
        std::ofstream f(tmp / "changji.toml", std::ios::binary);
        f << "[models]\nweights = \"te=cpu,vae=cpu\"\n";
    }
    CHECK(config::load_settings(tmp).models.weights == "te=cpu,vae=cpu");

    config::Settings s;
    s.models.weights = "";      // 空的才拒——别的都可能是合法规格
    const auto errs = s.validate();
    bool said = false;
    for (const auto& e : errs) {
        if (e.find("weights") != std::string::npos) said = true;
    }
    CHECK(said);
    fs::remove_all(tmp, ec);
}

TEST_CASE("video_lora_tiers：Turbo 只挂草稿档") {
    // Turbo 那类蒸馏 LoRA 拿画质换速度（实测采样 164 秒 → 41 秒），
    // 所以适合只挂草稿档：草稿看叙事和构图，成片跑满步数要最好的画面。
    // 上下文是两档共用的，所以这个决定落在**每次请求**上。
    CHECK(config::ModelsConfig{}.video_lora_tiers == "both");

    for (const char* v : {"draft", "final", "both"}) {
        CAPTURE(v);
        config::Settings ok;
        ok.models.video_lora_tiers = v;
        for (const auto& e : ok.validate()) {
            CHECK_MESSAGE(e.find("video_lora_tiers") == std::string::npos, e);
        }
    }

    config::Settings bad;
    bad.models.video_lora_tiers = "草稿";   // 只认那三个英文值
    bool said = false;
    for (const auto& e : bad.validate()) {
        if (e.find("video_lora_tiers") != std::string::npos) said = true;
    }
    CHECK(said);
}

TEST_CASE("weights = smart：按视频模型多大和卡多大算，不用人填") {
    // 用户的原话："都应该让程序自己算。"以前 smart 只看卡，5090 上要人手填
    // cpu；换了大卡还得记得改回来，不改就一直走慢路，而且没有任何提示。
    //
    // 账是 5090 上量的：扩散权重常驻还要给 1280×704 的计算缓冲 19.7 GB
    // 和余量 4 GB；VAE 也常驻再加 5.5 GB；卡按九成算。
    config::ModelsConfig m;
    m.weights = "smart";
    const double h3 = 18.8;   // MiniMax-H3 Q4_K_M

    // **实测锚点**：5090（32.6 GB）上 H3 18.8 GB 常驻差 788 MB 装不下 → cpu，
    // 和用户之前手填的一样，只是现在不用填了
    CHECK(m.weights_for(32.6, h3) == "cpu");   // 18.8 + 14.6 = 33.4 > 32.6
    // 小模型（Wan 5B 约 10 GB）在同一张卡上就装得下：10 + 14.6 = 24.6 ≤ 32.6
    CHECK(m.weights_for(32.6, 10.0) == "te=cpu,vae=cpu");
    // 大卡全装得下 → 只有文本编码器留内存（18.8 + 14.6 + 5.5 = 38.9 ≤ 80，且 ≥ 40）
    CHECK(m.weights_for(80.0, h3) == "te=cpu");
    // 拿不到模型大小：按装不下处理——猜错是整集出片失败，放内存只是慢
    CHECK(m.weights_for(80.0, 0.0) == "cpu");

    // **文本编码器永远放内存**：它每镜只跑一次（H3 实测 8 到 9 秒），
    // 却是最大的一块（18.9 GB）。80 GB 的卡上也不该占着它。
    CHECK(m.weights_for(80.0, h3).find("te=cpu") != std::string::npos);

    // VAE 那道门槛仍然可配：抬高它，80 GB 也不让 VAE 进显存
    m.vae_vram_min_gb = 100.0;
    CHECK(m.weights_for(80.0, h3) == "te=cpu,vae=cpu");

    // 别的取值原样传下去，不碰
    for (const char* w : {"cpu", "auto", "te=cpu,vae=cpu"}) {
        config::ModelsConfig other;
        other.weights = w;
        CAPTURE(w);
        CHECK(other.weights_for(8.0, h3) == w);
        CHECK(other.weights_for(80.0, h3) == w);
    }

    // 负门槛要拒
    config::Settings bad;
    bad.models.vae_vram_min_gb = -1.0;
    bool said = false;
    for (const auto& e : bad.validate()) {
        if (e.find("vae_vram_min_gb") != std::string::npos) said = true;
    }
    CHECK(said);
}

TEST_CASE("[models]：双专家视频模型的两项") {
    // Wan 2.2 的 A14B 是混合专家：高噪声专家跑前几步定构图和运动，
    // 低噪声专家跑后几步出细节。换它的理由是 TI2V-5B 的动作质量不够
    // （用户看了草稿档的原话：图生视频还是太差了）。
    const fs::path tmp = fs::temp_directory_path() / "changji_moe_cfg";
    std::error_code ec;
    fs::create_directories(tmp, ec);
    {
        std::ofstream f(tmp / "changji.toml", std::ios::binary);
        f << "[models]\nvideo = \"low.gguf\"\n"
             "video_high_noise = \"high.gguf\"\n"
             "video_moe_boundary = 0.9\n";
    }
    const auto s = config::load_settings(tmp);
    CHECK(s.models.video_high_noise == "high.gguf");
    CHECK(s.models.video_moe_boundary == doctest::Approx(0.9));

    // 默认：空 + sd.cpp 的 0.875
    CHECK(config::ModelsConfig{}.video_high_noise.empty());
    CHECK(config::ModelsConfig{}.video_moe_boundary == doctest::Approx(0.875));

    SUBCASE("只填高噪声那份要拒") {
        // 症状会是"出的片和以前一样"——高噪声那份被静默忽略，看不出来，
        // 所以这里必须拦住。
        config::Settings bad;
        bad.models.video_high_noise = "high.gguf";
        bad.models.video.clear();
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("video_high_noise") != std::string::npos) said = true;
        }
        CHECK(said);
    }
    SUBCASE("出片的 LoRA：默认就指着 Turbo 那份") {
        // 用途是 Turbo 那类蒸馏适配器：H3 的 Turbo LoRA 把 28 步压到 6 步。
        // **挂上之后步数要跟着改**，不改的话白挂，28 步跑 Turbo 只会更糊。
        // **默认非空**（2026-09-10 改的）：用户要"都使用 turbo 加速"，
        // 那就不该是个要手填的旋钮。文件不在时按"没配"处理并在日志里
        // 说一声——没下过 LoRA 的机器照样出片，只是慢。
        CHECK(config::ModelsConfig{}.video_lora.find("turbo") !=
              std::string::npos);
        CHECK(config::ModelsConfig{}.video_lora_strength ==
              doctest::Approx(1.0));
        {
            std::ofstream f(tmp / "changji.toml", std::ios::binary);
            f << "[models]\nvideo_lora = \"loras/turbo.safetensors\"\n"
                 "video_lora_strength = 0.8\n";
        }
        const auto got = config::load_settings(tmp);
        CHECK(got.models.video_lora == "loras/turbo.safetensors");
        CHECK(got.models.video_lora_strength == doctest::Approx(0.8));

        // VAE 分块：0 = 用内置的 16×11，负数拒。调小它换显存——
        // VAE 放内存解码 71 秒，放显存 8 秒，而按内置块大小放显存差 112 MB。
        CHECK(config::ModelsConfig{}.video_vae_tile == 0);
        config::Settings bad;
        bad.models.video_vae_tile = -1;
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("video_vae_tile") != std::string::npos) said = true;
        }
        CHECK(said);
    }
    SUBCASE("随机数发生器：默认 cuda，认 cpu/std，别的拒") {
        // sd.cpp 的默认是 cuda，Wan 那一路就用它；上游给 MiniMax-H3 的
        // 命令行是 --rng cpu。发生器不同则同一个种子出的画面不同，
        // **而且不报错**，所以这一项必须能配、且拼错要拦住。
        CHECK(config::ModelsConfig{}.video_rng == "cuda");
        {
            std::ofstream f(tmp / "changji.toml", std::ios::binary);
            f << "[models]\nvideo_rng = \"cpu\"\n";
        }
        CHECK(config::load_settings(tmp).models.video_rng == "cpu");

        config::Settings bad;
        bad.models.video_rng = "gpu";   // 没有这个取值，是 cuda
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("video_rng") != std::string::npos) said = true;
        }
        CHECK(said);
    }
    SUBCASE("t5xxl 和 llm 两个编码器参数位只能填一个") {
        // MiniMax-H3 用裁过的 Qwen3-VL-32B 当编码器，在 sd.cpp 里是
        // llm_path；Wan 用 UMT5-XXL，是 t5xxl_path。**填错了不报错**——
        // 照常加载，然后出一段和提示词没关系的片，没有任何日志指到这儿。
        config::Settings bad;
        bad.models.video = "h3.gguf";
        bad.models.video_llm = "qwen3vl.gguf";
        bad.models.video_text_encoder = "umt5.safetensors";
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("video_llm") != std::string::npos) said = true;
        }
        CHECK(said);

        // 各填一个都行
        for (const bool use_llm : {true, false}) {
            CAPTURE(use_llm);
            config::Settings ok;
            ok.models.video = "v.gguf";
            ok.models.video_text_encoder.clear();
            ok.models.video_llm.clear();
            (use_llm ? ok.models.video_llm : ok.models.video_text_encoder) =
                "enc.gguf";
            for (const auto& e : ok.validate()) {
                CHECK_MESSAGE(e.find("video_llm") == std::string::npos, e);
            }
        }
    }
    SUBCASE("交班点要在 0 和 1 之间") {
        for (const double v : {0.0, 1.0, 1.5, -0.1}) {
            CAPTURE(v);
            config::Settings bad;
            bad.models.video_moe_boundary = v;
            bool said = false;
            for (const auto& e : bad.validate()) {
                if (e.find("video_moe_boundary") != std::string::npos) said = true;
            }
            CHECK(said);
        }
    }

}

TEST_CASE("[models].frame_tier：默认 final，认 draft，别的拒") {
    // **默认是 final，别改回 draft。** 曾经跟 Python 一样默认草稿档，
    // 但画幅搬到项目上（`[video]`）之后，那个默认只盖成片档的宽高，
    // 草稿档还停在档位表里的 512×288。默认草稿档 = 新用户什么都不配
    // 就拿 512×288 的首帧去喂 704×1280 的视频，锚点放大两倍再用。
    //
    // 这条**不会报错**，出来的片子只是"看着不太行"。所以钉在这儿。
    CHECK(config::ModelsConfig{}.frame_tier == "final");

    // 变量别叫 small：Windows 的 rpcndr.h 里 `#define small char`，
    // 报错是"Settings 后面接 char 是非法的"，指着的是下一行。
    config::Settings tiny;
    tiny.models.frame_tier = "draft";   // 小卡上显存不够，有意降档
    for (const auto& e : tiny.validate()) {
        CHECK_MESSAGE(e.find("frame_tier") == std::string::npos, e);
    }

    config::Settings ok;
    ok.models.frame_tier = "final";
    for (const auto& e : ok.validate()) {
        CHECK_MESSAGE(e.find("frame_tier") == std::string::npos, e);
    }

    config::Settings bad;
    bad.models.frame_tier = "preview";   // 有这个档位，但首帧只认两个
    bool said = false;
    for (const auto& e : bad.validate()) {
        if (e.find("frame_tier") != std::string::npos) said = true;
    }
    CHECK(said);
}

TEST_CASE("[models].vram_reserve_gb：默认 6，负数拒") {
    // 默认 6 GB 是量出来的：5090 上 1280×704 的 VAE 解码要 6576 MB。
    // 留少了的症状是出图全失败，而且在 sd.cpp 的日志接上之前，
    // 上层只看得到一句"出图失败，看一眼上面 sd.cpp 打的日志"。
    CHECK(config::ModelsConfig{}.vram_reserve_gb == doctest::Approx(6.0));

    config::Settings bad;
    bad.models.vram_reserve_gb = -1.0;
    bool said = false;
    for (const auto& e : bad.validate()) {
        if (e.find("vram_reserve_gb") != std::string::npos) said = true;
    }
    CHECK(said);

    // 0 是合法的：大卡上不想留就不留
    config::Settings zero;
    zero.models.vram_reserve_gb = 0.0;
    for (const auto& e : zero.validate()) {
        CHECK_MESSAGE(e.find("vram_reserve_gb") == std::string::npos, e);
    }
}

TEST_CASE("[tiers]：填了以填的为准，没填按显存推") {
    // **这一节是被"设完重启就丢"逼出来的。** 档位以前只在进程内生效，
    // 用户把成片档调成 1280×704 跑了一集，重启回到 960×544，界面没提示。
    config::TiersConfig t;
    CHECK(t.draft_width == 0);          // 0 = 没填
    CHECK(t.validate().empty());

    SUBCASE("分辨率要是 32 的倍数") {
        // 不是的话 Wan 那一族潜空间对不齐，出图直接失败而日志指不到这儿。
        // **这条以前只在接口层查**，从配置文件进来是绕过的。
        config::TiersConfig bad;
        bad.final_width = 1000;
        bool said = false;
        for (const auto& e : bad.validate()) {
            if (e.find("final_width") != std::string::npos) said = true;
        }
        CHECK(said);

        config::TiersConfig ok;
        ok.final_width = 1280;
        ok.final_height = 704;
        CHECK(ok.validate().empty());
    }
    SUBCASE("负数拒，步数不要求 32 的倍数") {
        config::TiersConfig bad;
        bad.final_steps = -1;
        CHECK_FALSE(bad.validate().empty());
        config::TiersConfig ok;
        ok.final_steps = 6;             // 6 不是 32 的倍数，但步数不受这条管
        CHECK(ok.validate().empty());
    }
}

TEST_CASE("[video]：横竖屏加清晰度，宽高算出来") {
    // **用户要决定的是"竖屏还是横屏、720p 还是 2K"**，不是
    // "928 还是 1440、544 还是 920"。中间那层换算不该甩给用户——
    // 填错一个不是 32 倍数的数，报错要到出图那一步才出现。
    //
    // 这一节放在**项目目录的 changji.toml** 里，一部剧一份：一台机器上
    // 可以同时有竖屏短剧和横屏片子，画幅是剧的属性不是机器的属性。
    // 不进 project.json——那份在对拍覆盖范围内，Python 没有这些字段。
    SUBCASE("默认竖屏 720p") {
        const config::VideoConfig v;
        CHECK(v.orientation == "portrait");
        CHECK(v.quality == "720p");
        // **标准档 544×928**（2026-09-10 用户定的）。920 ÷ 32 = 28.75
        // 除不尽，取最近的 928 = 32 × 29；544 = 32 × 17。
        // 32 对齐是硬约束，不对齐 sd.cpp 直接出图失败。
        CHECK(v.size() == std::pair<int, int>{544, 928});
    }
    SUBCASE("横屏把长短边调过来") {
        config::VideoConfig v;
        v.orientation = "landscape";
        CHECK(v.size() == std::pair<int, int>{928, 544});
        v.quality = "2k";
        CHECK(v.size() == std::pair<int, int>{2560, 1440});
    }
    SUBCASE("**四种组合的宽高都得是 32 的倍数**") {
        // Wan 那一族的潜空间要求。不对齐出图直接失败，日志里指不到这儿，
        // 所以这条要钉死，不能靠人每次心算。
        for (const char* o : {"portrait", "landscape"}) {
            for (const char* q : {"720p", "2k"}) {
                config::VideoConfig v;
                v.orientation = o;
                v.quality = q;
                const auto [w, h] = v.size();
                // CAPTURE 一个 const char* 打的是指针，看不出是哪一组
                CAPTURE(std::string(o));
                CAPTURE(std::string(q));
                CHECK(w % 32 == 0);
                CHECK(h % 32 == 0);
            }
        }
    }
    SUBCASE("认不出的取值要拒，别悄悄当默认") {
        for (const auto& [field, bad] :
             std::vector<std::pair<std::string, std::string>>{
                 {"orientation", "竖"}, {"quality", "1080p"}}) {
            config::VideoConfig v;
            (field == "orientation" ? v.orientation : v.quality) = bad;
            CAPTURE(field);
            CAPTURE(bad);
            CHECK_FALSE(v.validate().empty());
        }
    }
}

TEST_CASE("老配置里的 comfy 自己换掉，不是让程序起不来") {
    // **拆掉一条路之后，老配置不能让程序起不来。**
    //
    // 拆 ComfyUI 时只在 validate() 里加了迁移说明，于是升级上来的用户
    // 遇到的是：程序直接退出，往 stderr 打一句"已经不支持了"。
    // 双击启动的人连那句都看不到，窗口一闪就没了。而那句话让他去改的
    // toml，正是他多半不知道在哪的那个文件——他本来会去设置页改，
    // 可设置页就是这个进程发的，起不来就打不开。
    //
    // 这一条是在本机跑二进制时真栽的：自己的 ~/.config 里还留着
    // tts.backend = "comfy"，程序起不来。
    config::Settings s;
    s.tts.backend = "comfy";
    s.models.engine = "comfy";

    const auto notes = config::migrate_legacy(s);

    CHECK(s.tts.backend == "local");
    CHECK(s.models.engine == "sd");
    // 换完必须是合法的，否则只是把"起不来"往后挪了一步
    for (const auto& e : s.validate()) {
        CHECK_MESSAGE(e.find("tts.backend") == std::string::npos, e);
        CHECK_MESSAGE(e.find("models.engine") == std::string::npos, e);
    }

    // **两处都要说一声。** 悄悄换掉比报错更糟：用户以为还在走 ComfyUI。
    REQUIRE(notes.size() == 2);
    bool said_tts = false, said_engine = false;
    for (const auto& n : notes) {
        if (n.find("[tts].backend") != std::string::npos) said_tts = true;
        if (n.find("[models].engine") != std::string::npos) said_engine = true;
        CHECK(n.find("comfy") != std::string::npos);   // 说清是从什么换过来的
    }
    CHECK(said_tts);
    CHECK(said_engine);
}

TEST_CASE("没有老取值时迁移一句话都不说") {
    // 每次启动都刷两行"已经帮你改了"，用户会当噪音略过——
    // 而真需要看的那一次也就跟着略过了。
    config::Settings s;
    CHECK(config::migrate_legacy(s).empty());
    CHECK(s.tts.backend == "local");
    CHECK(s.models.engine == "sd");

    // 认得的取值也不该被动
    config::Settings http;
    http.tts.backend = "http";
    CHECK(config::migrate_legacy(http).empty());
    CHECK(http.tts.backend == "http");
}

TEST_CASE("这一轮真正会用的规格：出片跟 Turbo，首帧不跟") {
    // **界面和真跑的必须是同一个数。** 2026-09-10 用户问"怎么没用 turbo"，
    // 因为设置页照着档位表显示"成片步数 28"，而每一镜实际跑的是 6 步。
    // 那时候这段判断只写在 run.cpp 里，设置页那边自己算——必然分叉。
    // 现在两边都调 effective_spec，这条用例钉住它的行为。
    config::Settings s;
    s.video.orientation = "portrait";
    s.video.quality = "720p";

    SUBCASE("没挂 Turbo：出片和首帧都用档位表的数") {
        // video_lora 留空 = 没挂
        s.models.video_lora = "";
        const auto e = config::effective_spec(s, 28);
        CHECK_FALSE(e.turbo);
        CHECK(e.final_steps == 28);
        CHECK(e.frame_steps == 28);
        CHECK(e.width == 544);      // 标准档 2026-09-10 从 704×1280 改成
        CHECK(e.height == 928);     // 544×928，见 VideoConfig::size()
    }

    SUBCASE("挂了 Turbo：出片 6 步，首帧还是档位表那个数") {
        // 文件真的要在——配了个不存在的路径不算挂上，那时候压到 6 步
        // 就是拿一个没有 LoRA 的模型跑 6 步，画面直接废掉。
        const auto dir = std::filesystem::temp_directory_path() /
                         "changji_eff_spec";
        std::filesystem::create_directories(dir / "loras");
        { std::ofstream f(dir / "loras" / "turbo.safetensors"); f << "x"; }
        s.models.dir = paths::to_utf8(dir);
        s.models.video_lora = "loras/turbo.safetensors";

        const auto e = config::effective_spec(s, 28);
        CHECK(e.turbo);
        CHECK(e.final_steps == 6);
        // **这一条是关键**：Turbo 只挂在视频模型上，出图那一步没有它。
        CHECK(e.frame_steps == 28);

        // 配了但文件不在 = 没挂上
        config::Settings missing = s;
        missing.models.video_lora = "loras/不存在.safetensors";
        const auto m = config::effective_spec(missing, 28);
        CHECK_FALSE(m.turbo);
        CHECK(m.final_steps == 28);

        std::filesystem::remove_all(dir);
    }

    SUBCASE("用户在 [tiers] 里写死了步数，Turbo 不许改它") {
        s.tiers.final_steps = 12;
        const auto e = config::effective_spec(s, 28);
        CHECK(e.steps_pinned);
        CHECK(e.final_steps == 12);
    }

    SUBCASE("[models].frame_steps 填了就以它为准") {
        s.models.frame_steps = 20;
        CHECK(config::effective_spec(s, 28).frame_steps == 20);
    }

    SUBCASE("画幅跟项目走") {
        s.video.orientation = "landscape";
        s.video.quality = "2k";
        const auto e = config::effective_spec(s, 28);
        CHECK(e.width == 2560);
        CHECK(e.height == 1440);
    }
}


TEST_CASE("[models].image_weights：图像模型的权重放哪，不跟 weights 走") {
    // **量出来的。** 5090 上 weights = "cpu"（给 18 GB 视频模型的）让 7 GB
    // 图像模型也每一步从内存搬权重：采样时 GPU 利用率 18%、一步 6.8 秒；
    // 扩散权重常驻时 82%、一步 1.25 秒。慢五倍，而且看起来像"显卡没吃满"。
    config::ModelsConfig m;
    CHECK(m.image_weights == "smart");
    m.weights = "cpu";
    // **按模型大小算，不是按卡算。** 32.6 GB 的 5090：
    //   fp8 20 GB → 20 + 6.6 + 4 = 30.6 > 29.3，装不下→cpu（实测第 34/62 段 OOM）
    //   Q6_K 16 GB → 26.6 ≤ 29.3，装得下→常驻
    CHECK(m.image_weights_for(32.6, 20.0) == "cpu");
    CHECK(m.image_weights_for(32.6, 16.0) == "te=cpu,vae=cpu");
    CHECK(m.image_weights_for(32.6, 12.0) == "te=cpu,vae=cpu");
    // 小卡：全放内存，不然加载就 OOM
    CHECK(m.image_weights_for(6.0, 12.0) == "cpu");
    // 拿不到模型大小：按装不下处理——猜错是六镜全废，放内存只是慢
    CHECK(m.image_weights_for(32.6, 0.0) == "cpu");
    // 显式填了就照填的来
    m.image_weights = "auto";
    CHECK(m.image_weights_for(32.6, 20.0) == "auto");
    // 视频那一项一个字不动
    CHECK(m.weights_for(32.0, 18.8) == "cpu");
}

// ---- 老实的显存需求（调度器问过卡之后拿它比） ----

TEST_CASE("老实数 = 常驻权重 + 计算缓冲，和决定放哪用同一组常数") {
    config::ModelsConfig m;

    SUBCASE("权重全放内存：只剩缓冲") {
        // 视频 14.6；图像 6.6 + 4.0。
        CHECK(m.video_live_vram_gb("cpu", 18.8) == doctest::Approx(14.6));
        CHECK(m.image_live_vram_gb("cpu", 20.0) == doctest::Approx(10.6));
    }
    SUBCASE("只有扩散常驻") {
        CHECK(m.video_live_vram_gb("te=cpu,vae=cpu", 18.8) ==
              doctest::Approx(18.8 + 14.6));
        CHECK(m.image_live_vram_gb("te=cpu,vae=cpu", 16.0) ==
              doctest::Approx(16.0 + 10.6));
    }
    SUBCASE("VAE 也常驻：再加 5.5") {
        CHECK(m.video_live_vram_gb("te=cpu", 18.8) ==
              doctest::Approx(18.8 + 14.6 + 5.5));
    }
    SUBCASE("不认得的规格按全常驻算——估高只是多卸一次，估低是 OOM") {
        CHECK(m.video_live_vram_gb("auto", 18.8) ==
              doctest::Approx(18.8 + 14.6 + 5.5));
    }
}

TEST_CASE("老实数要和 *_for 的判断对得上") {
    // 两边共用常数，所以"算得下"和"要占多少"必须自洽：
    // weights_for 说装得下的时候，老实数就该不超过整卡。
    config::ModelsConfig m;
    const double card = 32.6;
    const double model = 16.0;             // Q6_K
    const std::string w = m.weights_for(card, model);
    CHECK(m.video_live_vram_gb(w, model) <= card);

    // 而 H3 那种 18.8 GB 的，同一张卡上 weights_for 会判 "cpu"，
    // 老实数也就只剩缓冲，仍然装得下。
    const std::string w2 = m.weights_for(card, 18.8);
    CHECK(w2 == "cpu");
    CHECK(m.video_live_vram_gb(w2, 18.8) <= card);
}

TEST_CASE("大模型的老实数：权重 + KV 缓存和上下文") {
    config::ModelsConfig m;
    // 实测那一组：9 GB 的文件载进去 15.4 GB，式子给 15.25，对得上。
    CHECK(m.llm_live_vram_gb(9.0) == doctest::Approx(15.25));
    // **多出来的那部分不是常数**：KV 缓存跟着模型大小走。
    // 写成"加一个固定值"的话只在锚点上对，模型一换就偏。
    const double d1 = m.llm_live_vram_gb(4.0) - 4.0;
    const double d2 = m.llm_live_vram_gb(20.0) - 20.0;
    CHECK(d2 > d1);
    // 拿不到文件大小就说"没有"，让调用方退回保守估算。
    // **不许猜**：猜小了是 OOM，退回保守只是多卸一次。
    CHECK(m.llm_live_vram_gb(0.0) == doctest::Approx(0.0));
    CHECK(m.llm_live_vram_gb(-1.0) == doctest::Approx(0.0));
}
