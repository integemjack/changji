// 大模型客户端的测试。
//
// HTTP 是假的——注入一个记账的回调。测的是三件纯逻辑：
// 请求怎么拼、错误怎么翻成人话、返回怎么抽内容。
//
// 这三件事里第二件最容易被当成"不重要"而写马虎，但它恰恰是用户最常撞上的：
// 地址填错、模型名写错、密钥过期。Python 那边一开始就是 raise_for_status()
// 了事，结果这类失败一路穿到最外面变成一句「Internal Server Error」，
// 用户完全不知道该去改什么。stages/_llm.py 那个模块就是为此加的。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "llm/client.hpp"
#include "llm/schema_validate.hpp"
#include "pipeline/jobs.hpp"

#include "scoped_env.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

config::LLMConfig test_cfg() {
    config::LLMConfig c;
    c.base_url = "http://127.0.0.1:11434/v1";
    c.model = "qwen3:14b";
    c.api_key = "ollama";
    c.temperature = 0.7;
    c.timeout_s = 300.0;
    return c;
}

/// 默认那套（OpenRouter + 按任务分流），只是补上密钥。
/// 默认那家云服务（2026-09-14 起是智谱 bigmodel.cn）+ 一把密钥。
///
/// **地址不写死在这儿**，跟着 `LLMConfig` 的默认走：这个 fixture 要问的
/// 一直是"默认配置下发出去的是什么"，换家的时候该跟着变的正是它。
config::LLMConfig test_cfg_cloud() {
    config::LLMConfig c;
    c.api_key = "测试密钥";
    return c;
}

/// 记下每次请求，按顺序吐出预设的响应。
struct FakeHttp {
    struct Call {
        std::string url;
        json body;
        std::map<std::string, std::string> headers;
        double timeout_s = 0;
    };
    std::vector<Call> calls;
    std::vector<llm::HttpResponse> responses;

    llm::HttpPost fn() {
        return [this](const std::string& url, const std::string& body,
                      const std::map<std::string, std::string>& headers,
                      double timeout_s) -> llm::HttpResponse {
            calls.push_back(
                Call{url, json::parse(body, nullptr, false), headers, timeout_s});
            if (calls.size() <= responses.size()) return responses[calls.size() - 1];
            return llm::HttpResponse{500, "{}", std::nullopt};
        };
    }
};

llm::HttpResponse ok(const std::string& content) {
    const json body = {
        {"choices", json::array({json{{"message", {{"content", content}}}}})}};
    return llm::HttpResponse{200, body.dump(), std::nullopt};
}

llm::Request simple_req() {
    llm::Request r;
    r.prompt = "写一集短剧";
    r.schema = nlohmann::ordered_json{{"type", "object"}};
    r.schema_name = "script";
    r.temperature = 0.7;
    return r;
}

}  // namespace

TEST_CASE("请求体的形状：schema 贴在提示词里，不发 response_format") {
    // **2026-09-14 起只有这一种发法。** 原来挂着一部四档退档梯子
    // （json_schema → 削过的 schema → json_object → 什么都不发），为的是
    // 兜住各家对 response_format 支持得七零八落。它的代价一直很实在：
    // 任何一个别的 400（比如 glm-5.3 不收 thinking 的关闭值）都会被它读成
    // "这家不支持 json_schema"，于是**悄悄**退到最宽那一档接着生成，
    // 日志上看一切正常——实跑十次全中。
    const auto cfg = test_cfg();
    const json p = json(llm::build_payload(cfg, simple_req()));

    CHECK(p.at("model") == "qwen3:14b");
    CHECK(p.at("temperature") == 0.7);
    REQUIRE(p.at("messages").is_array());
    REQUIRE(p.at("messages").size() == 1);
    CHECK(p.at("messages")[0].at("role") == "user");
    // 一个字段都不发
    CHECK_FALSE(p.contains("response_format"));

    SUBCASE("schema 以文字接在提示词后面") {
        const std::string content =
            p.at("messages")[0].at("content").get<std::string>();
        CHECK(content.rfind("写一集短剧", 0) == 0);   // 提示词本身在最前面
        CHECK(content.find("\"type\": \"object\"") != std::string::npos);
        CHECK(content.size() > std::string("写一集短剧").size());
    }

    SUBCASE("没给 schema 就只发提示词本身") {
        llm::Request r = simple_req();
        r.schema = nlohmann::ordered_json();
        const json q = json(llm::build_payload(cfg, r));
        CHECK(q.at("messages")[0].at("content") == "写一集短剧");
        CHECK_FALSE(q.contains("response_format"));
    }
}

TEST_CASE("温度：req 没意见就用配置里的，有意见就听它的") {
    // **这一条钉的是一个真空转过的旋钮。** build_payload 原来发的是
    // cfg.temperature，req 里那个从来没人读——于是 kChapterTemperature
    // （写正文 0.5）在远端那条路上一直没生效，而远端正是现在的默认后端。
    // 两条路都返回 200，所以这件事只能靠读代码发现。
    config::LLMConfig cfg = test_cfg();
    cfg.temperature = 0.35;

    SUBCASE("空着：用用户在设置页上定的那个") {
        llm::Request r = simple_req();
        r.temperature.reset();
        const json p = json(llm::build_payload(cfg, r));
        CHECK(p.at("temperature") == doctest::Approx(0.35));
    }

    SUBCASE("填了：这一步说了算") {
        llm::Request r = simple_req();
        r.temperature = 0.5;
        const json p = json(llm::build_payload(cfg, r));
        CHECK(p.at("temperature") == doctest::Approx(0.5));
    }
}

TEST_CASE("温度按任务分档：编东西的放开，拆结构的收紧") {
    // **不是每一步都该跑同一个温度。** 大纲和选题是从无到有编东西，温度低了
    // 永远是那几个套路（用户的判词「每次写文章的内容都差不多」）；分镜和定妆
    // 是把已有的东西转成结构，发挥在那儿一律是错——编一个剧本里没有的道具，
    // 后面每一镜都得跟着它错下去。
    //
    // 做成相对偏移而不是绝对值表：绝对值表会把设置页那个旋钮架空。
    config::LLMConfig cfg;
    cfg.temperature = 0.7;

    CHECK(cfg.temperature_for("story_outline") == doctest::Approx(0.95));
    CHECK(cfg.temperature_for("premises") == doctest::Approx(0.95));
    CHECK(cfg.temperature_for("trailer") == doctest::Approx(0.95));
    CHECK(cfg.temperature_for("storyboard") == doctest::Approx(0.28));
    CHECK(cfg.temperature_for("bible") == doctest::Approx(0.28));
    CHECK(cfg.temperature_for("story_analysis") == doctest::Approx(0.28));
    // 认不出的、以及写剧本写正文那几步，跟着基准走
    CHECK(cfg.temperature_for("script") == doctest::Approx(0.7));
    CHECK(cfg.temperature_for("chapter") == doctest::Approx(0.7));
    CHECK(cfg.temperature_for("") == doctest::Approx(0.7));

    SUBCASE("用户那个旋钮还推得动所有档") {
        cfg.temperature = 0.3;
        CHECK(cfg.temperature_for("story_outline") == doctest::Approx(0.55));
        CHECK(cfg.temperature_for("storyboard") == doctest::Approx(0.12));
        CHECK(cfg.temperature_for("script") == doctest::Approx(0.3));
    }

    SUBCASE("发散那档封在 1.1：再高多数服务开始吐坏 JSON") {
        cfg.temperature = 1.4;
        CHECK(cfg.temperature_for("story_outline") == doctest::Approx(1.1));
    }

    SUBCASE("真发出去的就是这个数") {
        FakeHttp http;
        http.responses.push_back(ok("{\"ok\":1}"));
        config::LLMConfig c = test_cfg();
        c.temperature = 0.7;
        llm::RemoteClient rc(c, http.fn());
        pipeline::CancelToken tok;
        llm::Request r = simple_req();
        r.temperature.reset();
        r.schema_name = "story_outline";
        rc.complete(r, tok);
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("temperature").get<double>() ==
              doctest::Approx(0.95));
    }

    SUBCASE("这一步自己填了温度的，分档管不着它") {
        // 写正文那步有自己测出来的数（kChapterTemperature），别被档位盖掉。
        FakeHttp http;
        http.responses.push_back(ok("{\"ok\":1}"));
        llm::RemoteClient rc(test_cfg(), http.fn());
        pipeline::CancelToken tok;
        llm::Request r = simple_req();
        r.temperature = 0.5;
        r.schema_name = "storyboard";
        rc.complete(r, tok);
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("temperature").get<double>() ==
              doctest::Approx(0.5));
    }
}

TEST_CASE("哪些地址要 API Key") {
    // 判错的代价不对称：把云当成本机，用户会被初始化页放过去、体检也报绿，
    // 然后在第一次写剧本时撞上 401；反过来只是多提示一句。所以认不准就当要。
    config::LLMConfig c;

    SUBCASE("默认那一档是智谱，必须自己填密钥") {
        // **这里曾经内置过一把智谱的免费密钥，2026-09-13 用户要求删掉。**
        // 别再往回加：密钥明文编进二进制，strings 一抓就有，也会进 git。
        // ——2026-09-14 默认又换回智谱了，但**密钥这件事不跟着回来**：
        // 默认那个 glm-4.7-flash 不要钱，人还是得自己去领一把。
        CHECK(c.base_url.find("bigmodel.cn") != std::string::npos);
        CHECK(c.backend == "remote");
        CHECK(c.api_key.empty());
        CHECK(c.needs_api_key());
    }

    SUBCASE("本机和局域网的不要") {
        for (const char* url : {"http://127.0.0.1:11434/v1",
                                "http://localhost:1234/v1",
                                "http://192.168.1.20:8000/v1",
                                "http://10.0.0.5:8000/v1"}) {
            CAPTURE(url);
            c.base_url = url;
            CHECK_FALSE(c.needs_api_key());
        }
    }

    SUBCASE("进程内那条压根不用问") {
        c.backend = "local";
        CHECK_FALSE(c.needs_api_key());
    }

    SUBCASE("别的云都要") {
        for (const char* url : {"https://api.deepseek.com/v1",
                                "https://api.siliconflow.cn/v1",
                                "https://api.z.ai/api/paas/v4"}) {
            CAPTURE(url);
            c.base_url = url;
            CHECK(c.needs_api_key());
        }
    }

    SUBCASE("发出去的 Authorization 头就是配置里那把") {
        FakeHttp http;
        http.responses.push_back(ok("{\"ok\":1}"));
        config::LLMConfig mine = test_cfg();
        mine.api_key = "sk-or-v1-测试";
        llm::RemoteClient rc(mine, http.fn());
        pipeline::CancelToken tok;
        rc.complete(simple_req(), tok);
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].headers.at("Authorization") == "Bearer sk-or-v1-测试");
    }
}

TEST_CASE("密钥单独一个文件，不混进 config.toml") {
    // **config.toml 那个文件到处跑**：部署是整包 tar 推到服务器，排查问题
    // 时整份贴进聊天窗口。密钥混在里面的话每一次都是一次泄漏，而且泄漏时
    // 没有任何迹象。所以它单独一个文件，单独存、单独换、单独 gitignore。
    changji::test::ScopedUserConfigDir iso("apikey");

    CHECK(config::user_api_key_path().parent_path() ==
          config::user_config_path().parent_path());
    CHECK(config::user_api_key_path().filename() == "api_key");

    SUBCASE("没有那个文件就是空的，不报错") {
        CHECK(config::read_api_key_file().empty());
    }

    SUBCASE("写进去能原样读回来") {
        config::write_api_key_file("sk-or-v1-abc123");
        CHECK(config::read_api_key_file() == "sk-or-v1-abc123");
    }

    SUBCASE("首尾空白要剃掉") {
        // 用编辑器存出来的多半带一个结尾换行。带着它发出去的
        // Authorization 头会被网关判成非法，报 401——而那会把人支去查一个
        // 其实填对了的密钥。
        config::write_api_key_file("  sk-or-v1-abc123\n\n");
        CHECK(config::read_api_key_file() == "sk-or-v1-abc123");
    }

    SUBCASE("清空就是删掉，不留一个空文件让人猜") {
        config::write_api_key_file("sk-or-v1-abc123");
        config::write_api_key_file("");
        CHECK(config::read_api_key_file().empty());
        CHECK_FALSE(std::filesystem::exists(config::user_api_key_path()));
    }

    SUBCASE("它压过 config.toml 里那份") {
        // 老配置里写了 [llm].api_key 的照样认，但单独那个文件在就以它为准。
        std::error_code ec;
        std::filesystem::create_directories(
            config::user_config_path().parent_path(), ec);
        std::ofstream(config::user_config_path())
            << "[llm]\napi_key = \"老配置里那把\"\n";
        CHECK(config::load_settings(std::nullopt).llm.api_key == "老配置里那把");

        config::write_api_key_file("单独存的那把");
        CHECK(config::load_settings(std::nullopt).llm.api_key == "单独存的那把");
    }
}

TEST_CASE("按任务分流：哪一步用哪个模型") {
    // 用户 2026-09-13 定的方向："发挥各自的优势，不同功能使用不同的模型"。
    // 这条流水线要两种本事：写正文要文采，拆分镜要听话（分镜那份 schema
    // 有六十多个类型定义和一串枚举，文采在那儿一点用都没有）。
    config::LLMConfig c;

    // **默认不分流。** 2026-09-14 换到智谱之后，默认那一档
    // （glm-4.7-flash）是这家**唯一免费**的模型，分流无从分起——与其
    // 写九行一模一样的模型名，不如空着全落到兜底那个。
    // 想分流照 config.toml 模板里注释掉的那段抄（写作 glm-5.3 / 结构
    // glm-5.3-flash）。
    CHECK(c.task_models.empty());
    CHECK(c.model_for("chapter") == c.model);
    CHECK(c.model_for("storyboard") == c.model);

    // 机制本身照旧：配了就按配的走，写作那几步和结构那几步分得开。
    c.task_models["chapter"] = "glm-5.3";
    c.task_models["premises"] = "glm-5.3";
    c.task_models["storyboard"] = "glm-5.3-flash";
    CHECK(c.model_for("chapter") != c.model_for("storyboard"));
    CHECK(c.model_for("chapter") == c.model_for("premises"));

    SUBCASE("没点名的任务落到兜底那个") {
        CHECK(c.model_for("bible") == c.model);
        CHECK(c.model_for("这个任务还没有") == c.model);
        CHECK(c.model_for("") == c.model);
    }

    SUBCASE("配置里填空串也算没配") {
        // 不这么判的话，用户想"这一步用回默认"就只能把整个键删掉，
        // 而设置页上清空一个输入框比删一行配置自然得多。
        c.task_models["chapter"] = "";
        CHECK(c.model_for("chapter") == c.model);
    }

    SUBCASE("真发出去的 model 是分流之后那个") {
        // **不是 cfg.model。** 分流要是没接上，九个任务会全用兜底那个，
        // 而那件事不报错——只是正文忽然变难看。
        FakeHttp http;
        http.responses.push_back(ok("{\"ok\":1}"));
        config::LLMConfig cfg = test_cfg_cloud();
        cfg.task_models["chapter"] = "glm-5.3";
        llm::Request r = simple_req();
        r.schema_name = "chapter";
        llm::RemoteClient rc(cfg, http.fn());
        pipeline::CancelToken tok;
        rc.complete(r, tok);
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("model") == "glm-5.3");
        CHECK(http.calls[0].body.at("model") != cfg.model);
    }
}

TEST_CASE("退回 json_object 时 schema 要写进提示词") {
    // **不写的话这一下模型连字段名都不知道。** response_format 只剩
    // {"type":"json_object"}，而我们的提示词里从来没有字段名——分镜那份
    // 光 schema 就几十个字段加一串枚举。原来这条退路的注释写着"全靠提示词
    // 里那句『只输出 JSON』"，那句话保不住任何东西。
    llm::Request r = simple_req();
    r.schema = nlohmann::ordered_json::parse(
        R"({"type":"object","properties":{"hook":{"type":"string","minLength":6}}})");

    const std::string out = llm::schema_as_prompt(r.prompt, r.schema);
    CHECK(out.find("写一集短剧") == 0);      // 原提示词在最前面，不能被挤走
    CHECK(out.find("\"hook\"") != std::string::npos);
    // 抄的是**原始** schema：这里它是给模型读的文字，minLength 读得懂就有用
    CHECK(out.find("minLength") != std::string::npos);

    SUBCASE("没有 schema 就原样返回，不要平白多一段废话") {
        CHECK(llm::schema_as_prompt("就这一句", nlohmann::ordered_json()) ==
              "就这一句");
    }
}

TEST_CASE("从返回里抽内容") {
    const json body = {
        {"choices", json::array({json{{"message", {{"content", "结果"}}}}})}};
    CHECK(llm::extract_content(body.dump()) == "结果");

    SUBCASE("形状不对要报错，不能返回空串") {
        // 返回空串的话，下一步的 JSON 解析会说"找不到合法 JSON"，
        // 把问题指到错误的地方——真正的问题是这个服务的返回不是 OpenAI 格式。
        CHECK_THROWS_AS(llm::extract_content("{}"), llm::LlmError);
        CHECK_THROWS_AS(llm::extract_content(R"({"choices": []})"), llm::LlmError);
        CHECK_THROWS_AS(llm::extract_content(R"({"choices": [{}]})"),
                        llm::LlmError);
        CHECK_THROWS_AS(llm::extract_content(R"({"choices": [{"message": {}}]})"),
                        llm::LlmError);
        CHECK_THROWS_AS(llm::extract_content("不是 JSON"), llm::LlmError);
    }
}

TEST_CASE("状态码翻成人话") {
    const auto cfg = test_cfg();

    SUBCASE("404 分两种") {
        // Ollama 地址不对和模型没拉下来都回 404。一律说「地址填错了」
        // 会把人支到错误的地方去查。
        const std::string no_model = llm::explain_status(
            cfg, 404, R"({"error": "model 'qwen3:14b' not found"})");
        CHECK(no_model.find("ollama pull qwen3:14b") != std::string::npos);
        CHECK(no_model.find("地址填错") == std::string::npos);

        const std::string no_route =
            llm::explain_status(cfg, 404, R"({"error": "404 page not found"})");
        CHECK(no_route.find("地址填错") != std::string::npos);
        CHECK(no_route.find("/v1") != std::string::npos);
        CHECK(no_route.find("ollama pull") == std::string::npos);
    }

    SUBCASE("认证失败要指出去哪儿填密钥") {
        // **指的是项目页那个模型弹窗，不是设置页。** 大模型的地址、模型名、
        // 密钥、温度 2026-09-14 全搬进了项目页「模型」那一行点开的窗；设置页
        // 那一节整个删了（SettingsView 里那段注释：「密钥根本不经过这一页」）。
        // 这条用例原来钉着「设置页」，把人支去一个没有那个框的页面。
        for (const int code : {401, 403}) {
            const std::string s = llm::explain_status(cfg, code, "{}");
            CHECK(s.find("API Key") != std::string::npos);
            CHECK(s.find("项目页") != std::string::npos);
            CHECK(s.find(std::to_string(code)) != std::string::npos);
        }
    }

    SUBCASE("401 的两种情况说两句不同的话") {
        config::LLMConfig c;   // 默认：智谱 + 空密钥

        // 一、默认状态：密钥还没填。说"密钥不对"会把人支去检查一个他
        //     根本没填过的东西，而且该提哪儿领要说清楚。
        REQUIRE(c.api_key.empty());
        const std::string fresh = llm::explain_status(c, 401, "{}");
        CHECK(fresh.find("还没填") != std::string::npos);
        CHECK(fresh.find("bigmodel.cn") != std::string::npos);
        CHECK(fresh.find("不对") == std::string::npos);

        // 二、换了一家云服务、密钥还没填。说"密钥不对"会把人支去检查
        //     一个他根本没填过的东西。
        c.base_url = "https://api.deepseek.com/v1";
        c.model = "deepseek-chat";
        const std::string empty = llm::explain_status(c, 401, "{}");
        CHECK(empty.find("还没填") != std::string::npos);
        CHECK(empty.find("不对") == std::string::npos);

        // 三、真填了一个错的。
        c.api_key = "填了但是错的";
        CHECK(llm::explain_status(c, 401, "{}").find("不对") !=
              std::string::npos);
    }

    SUBCASE("403 要分清「密钥不对」和「这个模型不让你用」") {
        // **2026-09-13 实测撞上的。** OpenRouter 上
        // thinkingmachines/inkling:free 回 403 说 "only available on
        // agentic harnesses"——密钥完全正常（同一把密钥列得出 445 个模型），
        // 是这个模型的免费档限定入口。照老话术报的话，用户会去反复换一把
        // 其实没问题的密钥，而真正该做的是换一个模型。
        config::LLMConfig c = test_cfg();
        c.api_key = "sk-or-v1-好好的";
        const std::string gated = llm::explain_status(
            c, 403,
            R"({"error":{"code":403,"message":"thinkingmachines/inkling:free is only available on agentic harnesses. Try plugging it into a coding agent or productivity app listed on https://openrouter.ai/apps"}})");
        CHECK(gated.find("换一个模型") != std::string::npos);
        CHECK(gated.find("密钥本身多半没问题") != std::string::npos);
        CHECK(gated.find("API Key 不对") == std::string::npos);
        // 服务的原话要原样带上——那才是唯一说清了原因的东西
        CHECK(gated.find("agentic harnesses") != std::string::npos);

        SUBCASE("认不出的 403 还是回到「八成是密钥不对」") {
            // 宁可漏判：漏判只是退回原来那句，而服务原话照样跟在后面；
            // 误判是在密钥真过期时把人支去换模型。
            const std::string plain = llm::explain_status(
                c, 403, R"({"error":"forbidden"})");
            CHECK(plain.find("API Key 不对") != std::string::npos);
        }
    }

    SUBCASE("429 要分清「太频繁」和「没钱」") {
        CHECK(llm::explain_status(cfg, 429, "{}").find("太频繁") !=
              std::string::npos);

        // 智谱把"余额不足/没有可用资源包"也回 429（code 1113）。报成
        // "等一会儿再试"的话，用户会一直等一件永远不会自己好起来的事。
        const std::string broke = llm::explain_status(
            cfg, 429,
            R"({"error":{"code":"1113","message":"余额不足或无可用资源包，请充值。"}})");
        CHECK(broke.find("余额") != std::string::npos);
        CHECK(broke.find("太频繁") == std::string::npos);
        // 指一条出路：这家免费的那个叫什么，直接说出来
        CHECK(broke.find("glm-4.7-flash") != std::string::npos);
    }

    SUBCASE("服务端错误") {
        CHECK(llm::explain_status(cfg, 503, "{}").find("服务自己出错") !=
              std::string::npos);
    }

    SUBCASE("每条都带上当前模型名") {
        // 用户配了好几个模型来回切，不说是哪个的话没法判断
        for (const int code : {400, 401, 404, 429, 500}) {
            CAPTURE(code);
            CHECK(llm::explain_status(cfg, code, "{}").find("当前模型：qwen3:14b") !=
                  std::string::npos);
        }
    }

    SUBCASE("服务说的原话要带上") {
        const std::string s =
            llm::explain_status(cfg, 400, R"({"error": "context too long"})");
        CHECK(s.find("服务说：context too long") != std::string::npos);
    }

    SUBCASE("响应不是 JSON 也要能翻") {
        const std::string s =
            llm::explain_status(cfg, 502, "<html>Bad Gateway</html>");
        CHECK(s.find("服务自己出错") != std::string::npos);
        CHECK(s.find("Bad Gateway") != std::string::npos);
    }

    SUBCASE("超长的服务返回要截断") {
        // 有些网关会回一整页 HTML。原样贴出来会把错误框撑爆。
        const std::string s = llm::explain_status(cfg, 500, std::string(5000, 'x'));
        CHECK(s.size() < 500);
    }

    SUBCASE("中文的截断不会切出半个字") {
        std::string cn = R"({"error": ")";
        for (int i = 0; i < 500; ++i) cn += "错";
        cn += R"("})";
        const std::string s = llm::explain_status(cfg, 500, cn);
        // 200 个「错」是 600 字节
        CHECK(s.find(std::string("错")) != std::string::npos);
        // 不能出现非法 UTF-8——能塞进 json 就说明是合法的
        CHECK_NOTHROW(json(s).dump());
    }
}

TEST_CASE("远端客户端跑通一次") {
    FakeHttp http;
    http.responses.push_back(ok("{\"title\": \"雨夜天台\"}"));
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;

    const std::string out = c.complete(simple_req(), tok);
    CHECK(out == "{\"title\": \"雨夜天台\"}");

    REQUIRE(http.calls.size() == 1);
    CHECK(http.calls[0].url == "http://127.0.0.1:11434/v1/chat/completions");
    CHECK(http.calls[0].headers.at("Authorization") == "Bearer ollama");
    CHECK(http.calls[0].timeout_s == 300.0);
    // 只发一次，而且一个 response_format 都不带——schema 在提示词里
    CHECK_FALSE(http.calls[0].body.contains("response_format"));
    CHECK(http.calls[0].body.at("messages")[0].at("content").get<std::string>().find(
              "\"type\": \"object\"") != std::string::npos);
}

TEST_CASE("连不上的时候不重试") {
    // 传输层失败重试一遍没有意义，只是让用户多等一个超时。
    FakeHttp http;
    llm::HttpResponse down;
    down.status = 0;
    down.transport_error = "Connection refused";
    http.responses.push_back(down);
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;

    try {
        c.complete(simple_req(), tok);
        FAIL("该抛异常");
    } catch (const llm::LlmError& e) {
        const std::string msg = e.what();
        CHECK(msg.find("连不上大模型服务") != std::string::npos);
        CHECK(msg.find("http://127.0.0.1:11434/v1") != std::string::npos);
        CHECK(msg.find("Connection refused") != std::string::npos);
    }
    CHECK(http.calls.size() == 1);
}

TEST_CASE("取消") {
    SUBCASE("请求前就取消，一次都不发") {
        FakeHttp http;
        http.responses.push_back(ok("不该被用到"));
        llm::RemoteClient c(test_cfg(), http.fn());
        pipeline::CancelToken tok;
        tok.request();
        CHECK_THROWS_AS(c.complete(simple_req(), tok), llm::LlmError);
        CHECK(http.calls.empty());
    }

    SUBCASE("退回重试之前会再查一次") {
        // 同步的 HTTP 调用中途打不断，能查的只有请求之间。
        // 不查的话，用户点了停止还要再等一个完整的超时。
        FakeHttp http;
        http.responses.push_back(llm::HttpResponse{400, "{}", std::nullopt});
        http.responses.push_back(ok("不该被用到"));
        llm::RemoteClient c(test_cfg(), http.fn());

        pipeline::CancelToken tok;
        // 第一次请求发出去之后取消
        llm::HttpPost inner = http.fn();
        llm::HttpPost wrapped =
            [&](const std::string& u, const std::string& b,
                const std::map<std::string, std::string>& h, double t) {
                auto r = inner(u, b, h, t);
                tok.request();
                return r;
            };
        llm::RemoteClient c2(test_cfg(), wrapped);
        CHECK_THROWS_AS(c2.complete(simple_req(), tok), llm::LlmError);
        CHECK(http.calls.size() == 1);   // 只发了第一次
    }
}

TEST_CASE("回放客户端") {
    llm::ReplayClient c({"第一次", "第二次"});
    pipeline::CancelToken tok;

    CHECK(c.complete(simple_req(), tok) == "第一次");
    CHECK(c.complete(simple_req(), tok) == "第二次");
    // 录到头了要响亮地失败，不能悄悄重放最后一条——
    // 那样测试会"通过"但测的是错的东西
    CHECK_THROWS_AS(c.complete(simple_req(), tok), llm::LlmError);

    CHECK(c.calls().size() == 3);
    CHECK(c.calls()[0].prompt == "写一集短剧");
}

TEST_CASE("只有远端这一条后端了") {
    // **2026-09-14 把进程内那条（LocalClient + LlamaChat）整个删了。**
    // 现在的模型都要思考，而本地那条唯一的独门武器是 GBNF 语法采样——
    // 它和思考是冲突的（思考被语法堵在 JSON 里之后会挤进键名和字符串），
    // 而结构约束已经整个交给提示词了。
    //
    // llama.cpp 本身还在链：进程内配音用的是它。
    auto dummy_post = [](const std::string&, const std::string&,
                         const std::map<std::string, std::string>&, double) {
        llm::HttpResponse r;
        r.status = 200;
        r.body = R"({"choices":[{"message":{"content":"好"}}]})";
        return r;
    };
    const auto c = llm::make_client(dummy_post);
    REQUIRE(c != nullptr);
    pipeline::CancelToken tok;
    CHECK(c->complete(simple_req(), tok) == "好");
}

// ---- 起服务时不预热 ----
//
// 这里原来有一组"预热：这几种情况一个线程都不该起"。**整个预热都去掉了**
// ——用户 2026-09-11 定的："用的时候才加载是对的，不做启动预载"。
//
// 留这段话是因为它反过来说明了现在的规矩：`register_llm_slot` 只登记，
// **装是借出时才发生的**。起服务之后显存上应该看不到大模型；看到了就是
// 哪儿又偷偷预装了。

// ── 远端 SSE ────────────────────────────────────────────────────────
//
// 远端那条原来是整段到的（Client::complete 的默认实现：跑一遍同步的，
// 整段回调一次）。云端 API 上写一章、写大纲于是没有"边写边看"，界面干等
// 一两分钟最后一下子蹦出来。用户 2026-09-12：「远端那条也上 SSE」。
//
// 下面这几条盯的是**退路**：接了 SSE 不能让任何一种服务变得更糟。

namespace {

/// 假的流式发送：把预先摆好的几段字节喂给 on_chunk。
struct FakeStream {
    struct Call {
        json body;
        std::map<std::string, std::string> headers;
    };
    std::vector<Call> calls;
    /// 每次调用喂哪些段。calls 用完了就回 500。
    std::vector<std::vector<std::string>> chunks;
    std::vector<int> statuses;      ///< 对应每次调用的状态码，默认 200
    std::string error_body;         ///< 状态码 >= 400 时回的体

    llm::HttpPostStream fn() {
        return [this](const std::string&, const std::string& body,
                      const std::map<std::string, std::string>& headers, double,
                      const llm::OnChunk& on_chunk) -> llm::HttpResponse {
            calls.push_back(Call{json::parse(body, nullptr, false), headers});
            const std::size_t i = calls.size() - 1;
            const int status = i < statuses.size() ? statuses[i] : 200;
            llm::HttpResponse r;
            r.status = status;
            if (status >= 400) {
                r.body = error_body;
                return r;
            }
            if (i < chunks.size()) {
                for (const auto& c : chunks[i]) {
                    if (!on_chunk(c.data(), c.size())) break;
                }
            }
            return r;
        };
    }
};

std::string sse_chunk(const std::string& content) {
    return "data: {\"choices\":[{\"delta\":{\"content\":\"" + content +
           "\"}}]}\n\n";
}

}  // namespace

TEST_CASE("远端 SSE：边收边回调，拼出来的是全文") {
    FakeHttp http;
    FakeStream stream;
    // 内容要长得像 JSON：simple_req() 是带 schema 的，而"回了 200 却不是
    // JSON 就退一步"那条规矩会在散文上触发（见下面那个用例）。
    stream.chunks = {{sse_chunk("{\\\"title\\\": \\\"第一"),
                      sse_chunk("章\\\"}"), "data: [DONE]\n\n"}};

    llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    const std::string out = c.complete(simple_req(), tok, [&](const std::string& p) {
        pieces.push_back(p);
    });

    CHECK(out == "{\"title\": \"第一章\"}");
    // **给的是增量不是累计**：一段几千字的正文，每次带全文的话光字符串
    // 拷贝就比生成还贵。
    CHECK(pieces == std::vector<std::string>{"{\"title\": \"第一", "章\"}"});
    // 整段那条一次都不该走
    CHECK(http.calls.empty());
    // 请求里要带 stream: true，不然服务端回的是整段
    REQUIRE(stream.calls.size() == 1);
    CHECK(stream.calls[0].body.at("stream") == true);
    CHECK(stream.calls[0].headers.at("Accept") == "text/event-stream");
}

TEST_CASE("远端 SSE：不给 stream_post 就还是整段那条") {
    // TTS 那边和一堆测试拿 RemoteClient 当普通客户端用，不该被迫注入
    // 第二个函数。没有它时行为和以前**一个字都不差**。
    FakeHttp http;
    http.responses = {ok("{\"ch\":\"整段回来的\"}")};
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    const std::string out =
        c.complete(simple_req(), tok, [&](const std::string& p) { pieces.push_back(p); });
    CHECK(out == "{\"ch\":\"整段回来的\"}");
    // 默认实现会把整段回调一次——**不是不回调**，否则流式那条路上
    // 什么都收不到，而且不报错，只是编辑器里一直空着。
    CHECK(pieces == std::vector<std::string>{"{\"ch\":\"整段回来的\"}"});
}

TEST_CASE("远端 SSE：服务端压根不认 stream，回了一份普通 JSON") {
    // 这条是"不会变得更糟"的核心：老服务照样能用，只是那一下是整段到的。
    FakeHttp http;
    FakeStream stream;
    // 200，但没有一条 SSE，body 里是整份普通响应
    stream.chunks = {{}, {}};
    llm::HttpPostStream raw = stream.fn();
    llm::HttpPostStream wrapped =
        [&raw](const std::string& url, const std::string& body,
               const std::map<std::string, std::string>& headers, double t,
               const llm::OnChunk& on_chunk) {
            llm::HttpResponse r = raw(url, body, headers, t, on_chunk);
            r.body = ok("{\"ch\":\"整段那份\"}").body;   // 服务端把整份 JSON 一次性回了
            return r;
        };

    llm::RemoteClient c(test_cfg(), http.fn(), wrapped);
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    const std::string out =
        c.complete(simple_req(), tok, [&](const std::string& p) { pieces.push_back(p); });

    CHECK(out == "{\"ch\":\"整段那份\"}");
    CHECK(pieces == std::vector<std::string>{"{\"ch\":\"整段那份\"}"});
}

TEST_CASE("远端 SSE：流里报错要抛，不能当成写完了") {
    FakeHttp http;
    FakeStream stream;
    stream.chunks = {
        {"data: {\"error\":{\"message\":\"model not found\"}}\n\n"},
        {"data: {\"error\":{\"message\":\"model not found\"}}\n\n"}};

    llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
    pipeline::CancelToken tok;
    // 先回 200 再在流里说"这个模型没有"是常见做法。当成生成完了的话，
    // 用户拿到的是一段空正文外加一句"写好了"。
    CHECK_THROWS_AS(c.complete(simple_req(), tok, [](const std::string&) {}),
                    llm::LlmError);
}

TEST_CASE("远端 SSE：中途取消要断掉，别让它继续生成") {
    FakeHttp http;
    FakeStream stream;
    stream.chunks = {{sse_chunk("头一段"), sse_chunk("不该有的"), "data: [DONE]\n\n"}};

    llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
    pipeline::CancelToken tok;
    std::vector<std::string> pieces;
    CHECK_THROWS_AS(c.complete(simple_req(), tok,
                               [&](const std::string& p) {
                                   pieces.push_back(p);
                                   tok.request();   // 收到第一段就叫停
                               }),
                    llm::LlmError);
    // 叫停之后不该再有第二段——on_chunk 返回 false，传输层会断开
    CHECK(pieces.size() == 1);
}

/**
 * 这一层是用来接住"整份输出坏了"的——被 length 截断、内容过滤掐掉、
 * 压根不是 JSON。**一个字段没填好不是那一类**，而在这儿 return 一个错的
 * 代价是整份作废：一集十七镜、或者一章十分钟，全没。
 *
 * 两条都是实跑撞出来的，所以一起钉在这儿。
 */
TEST_CASE("校验层：一个字段没填好，不该把整份输出作废") {
    using changji::llm::validate_json_schema;
    using nlohmann::json;
    using ordered = nlohmann::ordered_json;

    SUBCASE("枚举填了表外的值：放行") {
        // 2026-09-16 实测：一集十七镜里第三镜的 face_pose 填了个表外的值，
        // 整个 /api/plan 回 400，十七镜全没了——而下游 drop_unknown_enums
        // 本来就会把它抹成默认值，一镜都不该丢。
        const auto schema = ordered::parse(
            R"({"type":"object","properties":{
                 "face_pose":{"type":"string","enum":["front","back"]}}})");
        CHECK_FALSE(validate_json_schema(json{{"face_pose", "三分之二侧"}}, schema)
                        .has_value());
    }

    SUBCASE("字符串写短了：放行；写空了：拦") {
        // 2026-09-16 实测：写了十分钟、三场戏都在，就因为第三场的 worse
        // 短了几个字，一个字都没留下：
        //   大模型输出不符合 chapter Schema：$.scenes[2].worse 太短
        // 这些 minLength 本来就是"推一把"的数（last_line 的下限还从 12 降到
        // 过 4），而走远端 API 的模型根本不按 GBNF 生成，它对它们只是建议。
        const auto schema = ordered::parse(
            R"({"type":"object","properties":{
                 "worse":{"type":"string","minLength":8}}})");
        CHECK_FALSE(validate_json_schema(json{{"worse", "少了退路"}}, schema)
                        .has_value());
        // 空串是另一回事：那不是"写短了"，是这一栏压根没写
        const auto empty = validate_json_schema(json{{"worse", ""}}, schema);
        REQUIRE(empty.has_value());
        CHECK(empty->find("$.worse") != std::string::npos);
    }

    SUBCASE("真坏了的还得拦住") {
        // 这一层的本职：类型不对、缺必填、整份走错分支
        const auto schema = ordered::parse(
            R"({"type":"object","properties":{"n":{"type":"integer"}},
                "required":["n"]})");
        CHECK(validate_json_schema(json{{"n", "不是数"}}, schema).has_value());
        CHECK(validate_json_schema(json::object(), schema).has_value());
        // 数组的下限是地板，不是建议——少写了几场，下游没法凭空补出来
        const auto arr = ordered::parse(
            R"({"type":"object","properties":{
                 "scenes":{"type":"array","minItems":3}}})");
        CHECK(validate_json_schema(json{{"scenes", json::array({1, 2})}}, arr)
                  .has_value());
    }

    SUBCASE("数组写多了：放行。多和少不对称") {
        // 2026-09-16 / 17 两次实撞：
        //     大模型输出不符合 script Schema：$.scenes.s2.beats 的项目太多
        // 一次五分钟的改编，就因为第二场多写了一拍，整份作废。而多出来的
        // 那一拍下游本来就吃得下——拍子会变成镜头，多一个少一个不是结构问题。
        const auto arr = ordered::parse(
            R"({"type":"object","properties":{
                 "beats":{"type":"array","minItems":2,"maxItems":3}}})");
        CHECK_FALSE(
            validate_json_schema(json{{"beats", json::array({1, 2, 3, 4})}}, arr)
                .has_value());
        // 少写了还是拦
        CHECK(validate_json_schema(json{{"beats", json::array({1})}}, arr)
                  .has_value());
    }
}
