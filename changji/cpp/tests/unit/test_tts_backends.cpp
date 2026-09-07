// 两个真配音后端的测试。
//
// **最要紧的一条不是怎么调，是怎么发现"调成功了但没出声"。**
//
// ComfyUI 的 TTS 节点内部捕获异常之后，会输出一个一秒的空音频然后正常返回，
// 服务端的执行状态报的是 success。只信状态码的客户端会被完全骗过，
// 拿到一堆静音文件还以为配音成功了——实测撞上过：节点缺 librosa 报错，
// 任务状态却是成功。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "stages/tts_backends.hpp"
#include "util/paths.hpp"

using namespace changji;
using comfy::OrderedJson;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_tts_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

TEST_CASE("台词按键名填，不按节点类型填") {
    // TTS 那一片的自定义节点包换得很勤，类型名各不相同，而输入名反而稳定。
    // 按类型列白名单的话，用户换一个节点包就一个字都填不进去。
    comfy::ApiWorkflow wf(OrderedJson{
        {"1", {{"class_type", "谁也没听过的TTS节点"},
               {"inputs", {{"tts_text", "占位"}, {"speaker", "none"},
                           {"emotion", "neutral"}}}}},
    });

    CHECK(stages::apply_text(wf, "我等了你三年。", "林晚.wav", "sad"));
    CHECK(wf.get_input("1", "tts_text") == "我等了你三年。");
    CHECK(wf.get_input("1", "speaker") == "林晚.wav");
    CHECK(wf.get_input("1", "emotion") == "sad");

    SUBCASE("neutral 不填") {
        // 填了的话有的引擎会按"中性"这个词去调语气，反而比不填更平。
        comfy::ApiWorkflow w2(OrderedJson{
            {"1", {{"class_type", "X"},
                   {"inputs", {{"text", "占位"}, {"emotion", "原来的"}}}}},
        });
        stages::apply_text(w2, "你好", std::nullopt, "neutral");
        CHECK(w2.get_input("1", "emotion") == "原来的");
    }

    SUBCASE("连线型和数字型的同名输入不能被写成字符串") {
        // 往连线上写字符串会让工作流提交时被服务端拒绝，
        // 而报错是节点校验失败，看不出是这里写坏的。
        comfy::ApiWorkflow w3(OrderedJson{
            {"1", {{"class_type", "X"},
                   {"inputs", {{"text", OrderedJson::array({"9", 0})},
                               {"speaker", 42}}}}},
        });
        CHECK_FALSE(stages::apply_text(w3, "你好", "音色", "neutral"));
        CHECK(w3.get_input("1", "text").is_array());
        CHECK(w3.get_input("1", "speaker") == 42);
    }

    SUBCASE("一个能填的节点都没有时返回 false") {
        // 调用方要据此报"工作流里找不到文本节点"，而不是提交一个
        // 原样的工作流然后配出一句占位词。
        comfy::ApiWorkflow w4(OrderedJson{
            {"1", {{"class_type", "KSampler"}, {"inputs", {{"steps", 20}}}}},
        });
        CHECK_FALSE(stages::apply_text(w4, "你好", std::nullopt, "neutral"));
    }
}

TEST_CASE("疑似空音频要被拦下来") {
    // **这道检查存在的理由**：ComfyUI 的 TTS 节点内部捕获异常后，
    // 会输出一个一秒的空音频然后正常返回，状态报的是 success。
    const fs::path dir = temp_dir("空音频");
    const fs::path p = dir / paths::from_utf8("一.wav");
    stages::write_silence(p, 1.0, 24000);

    const std::string line = "那一夜的雨下得特别大，她站在天台边上很久很久。";

    try {
        stages::reject_silent_audio(p, 1.0, line);
        FAIL("该抛");
    } catch (const stages::AudioError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        // 话要说到根因上：状态是成功的，用户第一反应不会是去看日志
        CHECK(msg.find("任务状态是成功") != std::string::npos);
        CHECK(msg.find("ComfyUI 的日志") != std::string::npos);
        // 要给出判断依据，不然用户没法确认这是不是误杀
        CHECK(msg.find("1.00 秒") != std::string::npos);
    }
}

TEST_CASE("两条下限都要过") {
    // 判据是**绝对下限（1.05 秒）和相对下限（估算的 35%）都满足才放过**。
    //
    // ⚠️ 副作用：一个字的台词（"嗯。"估约 0.94 秒）如果配出来不到 1.05 秒，
    // 会被当成空音频拦下来。这是**照抄 Python 的行为**，不是这里引入的。
    // 实际影响有限——TTS 引擎自己会加头尾静音，真实的"嗯。"通常在一秒以上。
    // 真撞上了要改的是那个常数，而不是悄悄放宽，因为放宽的代价是
    // 一整集静音文件被当成配音成功。
    const fs::path dir = temp_dir("下限");
    const fs::path p = dir / paths::from_utf8("嗯.wav");
    stages::write_silence(p, 1.2, 24000);

    SUBCASE("过了绝对下限的短台词放过") {
        CHECK_NOTHROW(stages::reject_silent_audio(p, 1.2, "嗯。"));
    }

    SUBCASE("没到绝对下限就拦，哪怕台词很短") {
        CHECK_THROWS_AS(stages::reject_silent_audio(p, 0.6, "嗯。"),
                        stages::AudioError);
    }

    SUBCASE("长台词配出一秒就是不对") {
        // 相对下限管的就是这种：绝对值过关，但和台词长度完全不匹配。
        CHECK_THROWS_AS(
            stages::reject_silent_audio(
                p, 1.1, "那一夜的雨下得特别大，她站在天台边上很久很久。"),
            stages::AudioError);
    }
}

TEST_CASE("wav 自己读，别的格式没有 ffmpeg 时说清缺什么") {
    const fs::path dir = temp_dir("时长");
    const fs::path w = dir / paths::from_utf8("一.wav");
    stages::write_silence(w, 2.0, 24000);
    CHECK(stages::probe_audio_duration(w, std::nullopt) ==
          doctest::Approx(2.0).epsilon(0.01));

    SUBCASE("非 wav 而且没有 ffmpeg") {
        // 只说"读不出时长"的话，用户会去查文件是不是坏了。
        const fs::path m = dir / paths::from_utf8("一.mp3");
        { std::ofstream f(m, std::ios::binary); f << "假的 mp3"; }
        try {
            stages::probe_audio_duration(m, std::nullopt);
            FAIL("该抛");
        } catch (const stages::AudioError& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            CHECK(msg.find("ffmpeg") != std::string::npos);
            CHECK(msg.find("assembly.ffmpeg_path") != std::string::npos);
        }
    }

    SUBCASE("扩展名是 wav 但内容不是，有 ffmpeg 就再试一次") {
        // 有些引擎给的其实是别的格式，只是文件名叫 wav。
        const fs::path fake = dir / paths::from_utf8("其实不是.wav");
        { std::ofstream f(fake, std::ios::binary); f << "OggS 什么的"; }

        media::FFmpeg ff("ffmpeg", "ffprobe",
                         [](const std::string&, const std::vector<std::string>&,
                            double) {
                             media::ProcResult r;
                             r.launched = true;
                             r.out = R"({"streams":[{"codec_type":"audio"}],)"
                                     R"("format":{"duration":"3.5"}})";
                             return r;
                         });
        CHECK(stages::probe_audio_duration(fake, ff) == doctest::Approx(3.5));
    }
}

TEST_CASE("HTTP 后端：把服务端的话带上") {
    const fs::path dir = temp_dir("http");
    const fs::path out = dir / paths::from_utf8("一.wav");

    SUBCASE("连不上时说清地址") {
        const auto b = stages::http_tts_backend(
            "http://127.0.0.1:9999/", 30.0,
            [](const std::string&, const std::string&,
               const std::map<std::string, std::string>&, double) {
                llm::HttpResponse r;
                r.status = 0;
                r.transport_error = "connection refused";
                return r;
            },
            std::nullopt);
        try {
            b.synthesize("你好", out, std::nullopt, "neutral", 0.5);
            FAIL("该抛");
        } catch (const stages::AudioError& e) {
            const std::string msg = e.what();
            CAPTURE(msg);
            // 结尾的斜杠要去掉，不然拼出来是 //tts
            CHECK(msg.find("http://127.0.0.1:9999）") != std::string::npos);
        }
    }

    SUBCASE("回错误码时把 body 带上") {
        // TTS 服务出错时常常回一整页 traceback，那里面才有真正的原因。
        const auto b = stages::http_tts_backend(
            "http://x", 30.0,
            [](const std::string&, const std::string&,
               const std::map<std::string, std::string>&, double) {
                llm::HttpResponse r;
                r.status = 500;
                r.body = "RuntimeError: 模型没加载";
                return r;
            },
            std::nullopt);
        try {
            b.synthesize("你好", out, std::nullopt, "neutral", 0.5);
            FAIL("该抛");
        } catch (const stages::AudioError& e) {
            CHECK(std::string(e.what()).find("模型没加载") != std::string::npos);
        }
    }

    SUBCASE("正常时写盘并验产出") {
        // 造一段真的 wav 当响应体
        const fs::path src = dir / paths::from_utf8("源.wav");
        stages::write_silence(src, 3.0, 24000);
        std::ifstream in(src, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());

        std::string sent_url;
        std::string sent_body;
        const auto b = stages::http_tts_backend(
            "http://x/", 30.0,
            [&](const std::string& url, const std::string& body,
                const std::map<std::string, std::string>&, double) {
                sent_url = url;
                sent_body = body;
                llm::HttpResponse r;
                r.status = 200;
                r.body = bytes;
                return r;
            },
            std::nullopt);

        const auto res =
            b.synthesize("我等了你三年。", out, "林晚.wav", "sad", 0.7);
        CHECK(res.duration_s == doctest::Approx(3.0).epsilon(0.01));
        REQUIRE(res.audio_path.has_value());
        CHECK(fs::exists(out));
        CHECK(sent_url == "http://x/tts");

        const auto payload = nlohmann::json::parse(sent_body);
        CHECK(payload.at("text") == "我等了你三年。");
        CHECK(payload.at("voice_id") == "林晚.wav");
        CHECK(payload.at("emotion") == "sad");
        CHECK(payload.at("emotion_intensity") == doctest::Approx(0.7));
    }

    SUBCASE("HTTP 后端同样要验产出物") {
        // 独立服务一样会"成功但没出声"。
        const fs::path src = dir / paths::from_utf8("静音.wav");
        stages::write_silence(src, 1.0, 24000);
        std::ifstream in(src, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());

        const auto b = stages::http_tts_backend(
            "http://x", 30.0,
            [&](const std::string&, const std::string&,
                const std::map<std::string, std::string>&, double) {
                llm::HttpResponse r;
                r.status = 200;
                r.body = bytes;
                return r;
            },
            std::nullopt);
        CHECK_THROWS_AS(
            b.synthesize("那一夜的雨下得特别大，她站在天台边上很久很久。", out,
                         std::nullopt, "neutral", 0.5),
            stages::AudioError);
    }
}

TEST_CASE("HTTP 后端不给音色列表") {
    // 独立服务的音色接口没有统一约定。猜一个路径去问，问不到会被当成
    // "服务端一个音色都没有"，然后自动挑选挑出个空的。
    // 不给的话走"照填的来"，那是对的。
    const auto b = stages::http_tts_backend(
        "http://x", 30.0,
        [](const std::string&, const std::string&,
           const std::map<std::string, std::string>&, double) {
            return llm::HttpResponse{};
        },
        std::nullopt);
    CHECK_FALSE(static_cast<bool>(b.list_voices));
}
