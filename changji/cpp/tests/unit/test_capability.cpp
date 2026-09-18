// 「这台机器能产什么」的推导。
//
// 要钉住的事只有一件，但它很重：**算错了不会报错**。
// 算多了（说能干其实不能）的表现是派过去被拒、那一镜失败；算少了
// （能干却说不能）的表现是那台机器一直不参与，界面上灰着一格，
// 而用户完全不知道为什么。两种都只能靠人去猜。

#include <doctest/doctest.h>

#include "infer/capability.hpp"

using namespace changji::infer;

namespace {

/// 一台什么都齐的机器。各用例从它开始往下减。
NodeFacts full() {
    NodeFacts f;
    f.built_with_sd = true;
    f.built_with_llama_tts = true;
    f.ffmpeg_ok = true;
    // 写文这一项在这一版上只认"指到远端了没有"——本机那条路 2026-09-14
    // 删了。所以"什么都齐"必须包括这一个，不然 full() 自己就写不了文。
    f.llm_remote = true;
    f.models = {
        {"llm", true},   {"tts", true},       {"tts_decoder", true},
        {"image", true}, {"image_vae", true}, {"video", true},
        {"video_vae", true},
    };
    return f;
}

bool able(const NodeFacts& f, Capability c) {
    return missing_for(c, f).empty();
}

}  // namespace

TEST_CASE("什么都齐的机器五样全能干") {
    const auto rs = capabilities_of(full());
    REQUIRE(rs.size() == all_capabilities().size());
    for (const auto& r : rs) {
        CAPTURE(to_string(r.cap));
        CAPTURE(r.why);
        CHECK(r.able);
        CHECK(r.why.empty());
    }
}

TEST_CASE("没编 sd.cpp：出图出片都不行，别的照旧") {
    // build-coord 那份就是这样编的（CHANGJI_SD=OFF）。它起得来、
    // 界面能开，但一张图都出不了——这种机器接了出图的活就是白接。
    NodeFacts f = full();
    f.built_with_sd = false;

    CHECK_FALSE(able(f, Capability::Frame));
    CHECK_FALSE(able(f, Capability::Video));
    CHECK(able(f, Capability::Llm));
    CHECK(able(f, Capability::Tts));
    CHECK(able(f, Capability::Assemble));
}

TEST_CASE("没编 llama.cpp：配音只能指到外部服务") {
    NodeFacts f = full();
    f.built_with_llama_tts = false;
    CHECK_FALSE(able(f, Capability::Tts));

    // 指到外部就不看本机有什么了——这条路上它只是个转发的。
    f.tts_remote = true;
    CHECK(able(f, Capability::Tts));

    // 写文跟这个旗子没关系，别再串在一起。
    CHECK(able(f, Capability::Llm));
}

TEST_CASE("写文只认远端：没指地址的机器一律不能写") {
    // ⚠️ **这一条挡的是"算多了"。** 进程内那条大模型 2026-09-14 删了，
    // 这个分支是在那之前拉的，原来这儿判的是"编没编 llama.cpp"加
    // [models].llm 那个权重文件。合进来照旧判的话，一台只装了配音模型的
    // 机器会在那张表和 /api/doctor 上亮"能写文"，而它一个字都写不出来。
    NodeFacts f = full();
    f.llm_remote = false;
    CHECK_FALSE(able(f, Capability::Llm));

    // 编进了 llama.cpp（那是配音那半）也好，[models].llm 那个权重文件
    // 在盘上也好，都不作数——没有任何东西会去读它们来写文。
    f.built_with_llama_tts = true;
    f.models["llm"] = true;
    CHECK_FALSE(able(f, Capability::Llm));

    f.llm_remote = true;
    CHECK(able(f, Capability::Llm));
}

TEST_CASE("没有 ffmpeg：出片和装配都不行，出图不受影响") {
    // 这一条是实机烧出来的：扩散 8 步全跑完，到最后编码那一步才报
    // 找不到 ffmpeg。八张卡上，"跑几十秒再失败"乘以八就是几分钟白烧。
    NodeFacts f = full();
    f.ffmpeg_ok = false;

    CHECK(able(f, Capability::Frame));
    CHECK_FALSE(able(f, Capability::Video));
    CHECK_FALSE(able(f, Capability::Assemble));
}

TEST_CASE("VAE：图像那份没配就退回视频那份") {
    NodeFacts f = full();
    f.models["image_vae"] = false;
    CHECK(able(f, Capability::Frame));       // 还有 video_vae 兜着

    f.models["video_vae"] = false;
    CHECK_FALSE(able(f, Capability::Frame)); // 两个都没有才是真不行
}

TEST_CASE("配音缺一个文件就不算能干") {
    // 缺了不会报错，只会退回估算后端出一段静音——那才是最阴的，
    // 整章跑完才发现没声音。所以这里必须拦住。
    NodeFacts f = full();
    f.models["tts_decoder"] = false;
    CHECK_FALSE(able(f, Capability::Tts));

    f = full();
    f.models["tts"] = false;
    CHECK_FALSE(able(f, Capability::Tts));
}

TEST_CASE("说不出原因的「不能干」是不合格的") {
    // 那句话会显示在表格的格子上，用户要靠它知道该去做什么。
    NodeFacts f;  // 什么都没有
    for (const auto& r : capabilities_of(f)) {
        CAPTURE(to_string(r.cap));
        CHECK_FALSE(r.able);
        CHECK_FALSE(r.why.empty());
    }
}

TEST_CASE("每个能力都有名字和显示名") {
    for (const Capability c : all_capabilities()) {
        CHECK(std::string(to_string(c)) != "unknown");
        CHECK(std::string(label_of(c)) != "");
    }
}
