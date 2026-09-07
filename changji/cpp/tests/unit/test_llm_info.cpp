// /api/llm/providers 和 /api/llm/models 的测试。
//
// 这两个原本在方案的破契约白名单里。**决策 4 之后那个理由不成立了**——
// 用远端大模型不再是过渡状态，是长期形态之一（树莓派没有跑 14B 的内存）。
// 所以这里测的是"和 Python 形状一致"，外加新增字段是**向后兼容**的。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/llm_info.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

config::Settings test_settings(const fs::path& models_dir = {}) {
    config::Settings s;
    s.llm.base_url = "http://127.0.0.1:11434/v1";
    s.llm.model = "qwen3:14b";
    s.llm.api_key = "ollama";
    s.llm.timeout_s = 300.0;
    if (!models_dir.empty()) {
        s.models.dir = paths::to_utf8(models_dir);
        s.models.llm = "Qwen3-14B-Q4_K_M.gguf";
    }
    return s;
}

struct FakeGet {
    std::vector<std::string> urls;
    std::vector<double> timeouts;
    llm::HttpResponse reply;

    http::HttpGet fn() {
        return [this](const std::string& url,
                      const std::map<std::string, std::string>&,
                      double timeout_s) {
            urls.push_back(url);
            timeouts.push_back(timeout_s);
            return reply;
        };
    }
};

llm::HttpResponse ok_body(const json& j) {
    return llm::HttpResponse{200, j.dump(), std::nullopt};
}

fs::path make_models_dir(const std::string& tag,
                         const std::vector<std::string>& names) {
    const fs::path dir =
        fs::temp_directory_path() / paths::from_utf8("changji_模型_" + tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    for (const auto& n : names) {
        std::ofstream out(dir / paths::from_utf8(n), std::ios::binary);
        out << "假的";
    }
    return dir;
}

}  // namespace

TEST_CASE("平台清单和 Python 一致") {
    const auto r = http::get_llm_providers();
    CHECK(r.status == 200);
    REQUIRE(r.body.at("providers").is_array());
    // 这份清单是从 Python 的 LLM_PROVIDERS 生成的，不是手抄的。
    // 十几家平台每家四个字段，手抄必然错，而且各家地址会变——
    // 手抄的那份只会越来越旧且没有任何迹象提示它旧了。
    CHECK(r.body.at("providers").size() >= 15);

    for (const auto& p : r.body.at("providers")) {
        CAPTURE(p.at("id").get<std::string>());
        // 前端靠 id 匹配"当前地址是哪一家"，靠 base_url 一键填入
        CHECK(p.contains("id"));
        CHECK(p.contains("name"));
        CHECK(p.contains("base_url"));
        CHECK(p.contains("local"));
        CHECK(p.contains("note"));
        CHECK(p.at("local").is_boolean());
        const std::string url = p.at("base_url").get<std::string>();
        CHECK(url.rfind("http", 0) == 0);
    }

    SUBCASE("本机那几家标了 local") {
        bool found_ollama = false;
        for (const auto& p : r.body.at("providers")) {
            if (p.at("id") == "ollama") {
                found_ollama = true;
                CHECK(p.at("local") == true);
                CHECK(p.at("base_url") == "http://127.0.0.1:11434/v1");
            }
        }
        CHECK(found_ollama);
    }
}

TEST_CASE("模型列表：拿得到的时候") {
    FakeGet get;
    get.reply = ok_body(json{{"data", json::array({
        json{{"id", "qwen3:14b"}},
        json{{"id", "llama3:8b"}},
        json{{"id", "qwen3:14b"}},   // 重复的要去掉
    })}});

    const auto r = http::get_llm_models(test_settings(), get.fn());
    CHECK(r.status == 200);
    // 去重且排序，对应 Python 的 sorted(set(names))
    CHECK(r.body.at("models") == json::array({"llama3:8b", "qwen3:14b"}));
    CHECK(r.body.at("current") == "qwen3:14b");
    // 成功时**没有 error 键**。形状是契约。
    CHECK_FALSE(r.body.contains("error"));

    REQUIRE(get.urls.size() == 1);
    CHECK(get.urls[0] == "http://127.0.0.1:11434/v1/models");
    // 超时写死 10 秒，不用 llm.timeout_s（那个默认 300 秒）。
    // 拿 300 秒来问一个列表，服务不在的时候设置页会转五分钟圈。
    CHECK(get.timeouts[0] == 10.0);
}

TEST_CASE("模型列表：拿不到的三种情况") {
    // 三种都返回**空列表加一句原因**，不抛异常。
    // 列不出来不该让整个设置页打不开——界面退回手打就行。
    const auto check_failed = [](const llm::HttpResponse& reply,
                                 const std::string& want_in_error) {
        FakeGet get;
        get.reply = reply;
        const auto r = http::get_llm_models(test_settings(), get.fn());
        CHECK(r.status == 200);
        CHECK(r.body.at("models") == json::array());
        REQUIRE(r.body.contains("error"));
        const std::string err = r.body.at("error").get<std::string>();
        CAPTURE(err);
        CHECK(err.find(want_in_error) != std::string::npos);
        // 失败时**没有 current 键**，和 Python 一致
        CHECK_FALSE(r.body.contains("current"));
    };

    llm::HttpResponse down;
    down.status = 0;
    down.transport_error = "Connection refused";
    check_failed(down, "连不上");
    check_failed(llm::HttpResponse{500, "{}", std::nullopt}, "返回 500");
    check_failed(llm::HttpResponse{200, "<html>", std::nullopt}, "不是 JSON");
}

TEST_CASE("模型列表能认几种返回形状") {
    // 各家服务的 /models 返回不完全一样。认不出来就是空列表，
    // 而空列表在界面上和"服务没起来"长得一样，用户会去查错的地方。
    const auto names = [](const json& reply) {
        FakeGet get;
        get.reply = ok_body(reply);
        return http::get_llm_models(test_settings(), get.fn()).body.at("models");
    };

    // 标准 OpenAI：{"data": [{"id": ...}]}
    CHECK(names(json{{"data", json::array({json{{"id", "a"}}})}}) ==
          json::array({"a"}));
    // 顶层直接是数组
    CHECK(names(json::array({json{{"id", "b"}}})) == json::array({"b"}));
    // 数组里是纯字符串
    CHECK(names(json::array({"c", "d"})) == json::array({"c", "d"}));
    // data 是空的
    CHECK(names(json{{"data", json::array()}}) == json::array());
    // 完全不认识的形状，空列表但不崩
    CHECK(names(json{{"whatever", 1}}) == json::array());
}

TEST_CASE("本机 gguf 列表是新增字段，不替换原来的") {
    // 方案原本写的是"改成列本地 gguf"。**那会真的破契约**——
    // 设置页那个下拉框会突然从"远端服务上的模型"变成"本机文件"，
    // 而用户配的是远端服务。加字段前端不看，是向后兼容的。
    const fs::path dir = make_models_dir(
        "列表", {"Qwen3-14B-Q4_K_M.gguf", "llama3.gguf", "readme.txt",
                 "Wan2.2.GGUF", "不是模型.safetensors"});

    FakeGet get;
    get.reply = ok_body(json{{"data", json::array({json{{"id", "远端模型"}}})}});
    const auto r = http::get_llm_models(test_settings(dir), get.fn());

    // 远端那份原样在
    CHECK(r.body.at("models") == json::array({"远端模型"}));
    CHECK(r.body.at("current") == "qwen3:14b");

    // 本机那份在新字段里
    const json& local = r.body.at("local");
    CHECK(local.at("dir") == paths::to_utf8(dir));
    CHECK(local.at("current") == "Qwen3-14B-Q4_K_M.gguf");
    // 只收 .gguf，大小写都认；别的扩展名不要
    CHECK(local.at("files") ==
          json::array({"Qwen3-14B-Q4_K_M.gguf", "Wan2.2.GGUF", "llama3.gguf"}));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("远端连不上时本机列表照样给") {
    // 用户可能压根没配远端服务，那时候本机这份就是他唯一能选的东西。
    const fs::path dir = make_models_dir("离线", {"a.gguf"});
    FakeGet get;
    get.reply.status = 0;
    get.reply.transport_error = "Connection refused";

    const auto r = http::get_llm_models(test_settings(dir), get.fn());
    CHECK(r.body.at("models") == json::array());
    CHECK(r.body.contains("error"));
    CHECK(r.body.at("local").at("files") == json::array({"a.gguf"}));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("模型目录不存在也不报错") {
    // 装好程序还没下模型是常态。这时候返回空列表，
    // 不该让设置页打不开。
    config::Settings s = test_settings();
    s.models.dir = "Z:/这个目录不存在";
    FakeGet get;
    get.reply = ok_body(json{{"data", json::array()}});
    const auto r = http::get_llm_models(s, get.fn());
    CHECK(r.status == 200);
    CHECK(r.body.at("local").at("files") == json::array());
}
