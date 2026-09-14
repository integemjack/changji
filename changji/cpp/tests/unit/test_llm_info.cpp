// /api/llm/providers 和 /api/llm/models 的测试。
//
// 这两个原本在方案的破契约白名单里。**决策 4 之后那个理由不成立了**——
// 用远端大模型不再是过渡状态，是长期形态之一（树莓派没有跑 14B 的内存）。
// 所以这里测的是"和 Python 形状一致"，外加新增字段是**向后兼容**的。

#include <doctest/doctest.h>

#include <algorithm>
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

TEST_CASE("本机 gguf 列表已经空了，但字段还在") {
    // **2026-09-14 把进程内后端整个删了**，本机那份 gguf 一个都选不了，
    // 所以这一段不再去扫目录——摆出来只会让人以为还能在本机跑。
    //
    // **字段本身留着**：少一个键会让还没更新的页面在取值时炸，
    // 而这一层没法知道对面是新版还是旧版。
    const fs::path dir = make_models_dir(
        "列表", {"Qwen3-14B-Q4_K_M.gguf", "llama3.gguf", "readme.txt",
                 "Wan2.2.GGUF", "不是模型.safetensors"});

    FakeGet get;
    get.reply = ok_body(json{{"data", json::array({json{{"id", "远端模型"}}})}});
    const auto r = http::get_llm_models(test_settings(dir), get.fn());

    // 远端那份原样在
    CHECK(r.body.at("models") == json::array({"远端模型"}));
    CHECK(r.body.at("current") == "qwen3:14b");

    // 本机那份是空的，但三个键都在——目录里明明有 gguf 也不列
    const json& local = r.body.at("local");
    CHECK(local.at("dir") == "");
    CHECK(local.at("current") == "");
    CHECK(local.at("files") == json::array());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("远端连不上时 local 字段照样在") {
    // 键少一个会让页面在取值时炸，所以连不上也要把这个空壳给出去。
    const fs::path dir = make_models_dir("离线", {"a.gguf"});
    FakeGet get;
    get.reply.status = 0;
    get.reply.transport_error = "Connection refused";

    const auto r = http::get_llm_models(test_settings(dir), get.fn());
    CHECK(r.body.at("models") == json::array());
    CHECK(r.body.contains("error"));
    CHECK(r.body.at("local").at("files") == json::array());

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

TEST_CASE("认识的那几家带一本小抄：known") {
    // **为什么非有这个不可**（2026-09-13 实测）：智谱的 /models
    // 只回 glm-4.5 / 4.5-air / 4.6 / 4.7 / 5 / 5-turbo / 5.1 / 5.2 /
    // 5.3 / 5.3-flash——**glm-4.7-flash 不在里面而它能用**，
    // 而它正好是我们的默认。只照 /models 渲染下拉的话，
    // 默认那个模型在自己的下拉里是找不到的。
    config::Settings s = test_settings();
    s.llm.base_url = "https://open.bigmodel.cn/api/paas/v4";
    s.llm.model = "glm-4.7-flash";

    FakeGet get;
    // 照着实测的样子回——**故意不含 glm-4.7-flash**。
    get.reply = ok_body(json{{"data", json::array({
                                 json{{"id", "glm-4.6"}},
                                 json{{"id", "glm-5.3"}},
                             })}});
    const auto r = http::get_llm_models(s, get.fn());

    // models 仍然只装服务真答应的那些：这两个字段**不合并**。
    // 混进小抄之后，前端那句"这台服务上没有 X"就会在模型真的不存在时
    // 也不吭声。合并是前端的事。
    CHECK(r.body.at("models") == json::array({"glm-4.6", "glm-5.3"}));

    REQUIRE(r.body.contains("known"));
    const json& known = r.body.at("known");
    REQUIRE(known.is_array());
    REQUIRE(!known.empty());

    std::vector<std::string> ids;
    for (const auto& k : known) {
        CHECK(k.contains("id"));
        // **每一项都得有一句话。** 一串 glm-4.5/4.6/4.7/5/5.1/5.2/5.3
        // 摆在那儿，要紧的两件事——哪个不要钱、哪个会写——名字上一个字
        // 都看不出来。
        CHECK(k.contains("note"));
        CHECK_FALSE(k.at("note").get<std::string>().empty());
        ids.push_back(k.at("id").get<std::string>());
    }
    // 头一个是推荐顺序的头一个，前端换家时就挑它，所以别随手改顺序。
    CHECK(ids.front() == "glm-4.7-flash");
    CHECK(std::find(ids.begin(), ids.end(), "glm-5.3") != ids.end());

    SUBCASE("z.ai 是同一套后端，同样给") {
        s.llm.base_url = "https://api.z.ai/api/paas/v4";
        FakeGet g2;
        g2.reply = ok_body(json{{"data", json::array()}});
        CHECK_FALSE(http::get_llm_models(s, g2.fn()).body.at("known").empty());
    }

    SUBCASE("不认识的家给空的，不瞎猜") {
        // 这是本我们自己维护的小抄，不是模型总表。认不出的地址上
        // 编几个名字出来，比不给更糟。
        FakeGet g2;
        g2.reply = ok_body(json{{"data", json::array()}});
        const auto r2 = http::get_llm_models(test_settings(), g2.fn());
        CHECK(r2.body.at("known") == json::array());
    }
}

TEST_CASE("连不上也要给小抄") {
    // 刚装好还没填密钥时 /models 必然 401，**而那正是最需要
    // 「这家都有什么、该挑哪个」的时候**。这时候把下拉渲染成空的，
    // 人只能回去手打一个自己也不确定的名字。
    config::Settings s = test_settings();
    s.llm.base_url = "https://open.bigmodel.cn/api/paas/v4";

    for (const int status : {401, 429, 500}) {
        CAPTURE(status);
        FakeGet get;
        get.reply = llm::HttpResponse{status, "{}", std::nullopt};
        const auto r = http::get_llm_models(s, get.fn());
        // 失败时的老契约一个字不动：空 models、有 error、没有 current。
        CHECK(r.body.at("models") == json::array());
        CHECK(r.body.contains("error"));
        CHECK_FALSE(r.body.contains("current"));
        // 新加的这份照给。
        CHECK_FALSE(r.body.at("known").empty());
    }

    SUBCASE("连都连不上也一样") {
        FakeGet get;
        get.reply.status = 0;
        get.reply.transport_error = "Connection refused";
        const auto r = http::get_llm_models(s, get.fn());
        CHECK(r.body.at("models") == json::array());
        CHECK_FALSE(r.body.at("known").empty());
    }
}
