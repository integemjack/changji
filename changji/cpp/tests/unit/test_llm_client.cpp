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
#include "infer/llama_chat.hpp"
#include "llm/local_client.hpp"
#include "pipeline/jobs.hpp"

#include "scoped_env.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

config::LLMConfig test_cfg() {
    // **每次取 fixture 都把那笔账清掉。** "哪个服务不支持 json_schema" 是
    // 进程级的记忆（见 llm::reset_schema_support）：前一个用例刚验出这个假
    // 服务不支持，后一个用例就再也发不出 json_schema 了，而它断言的正是
    // 那一下——两个用例单跑都绿，一起跑才挂，最难查的那种。
    llm::reset_schema_support();

    config::LLMConfig c;
    c.base_url = "http://127.0.0.1:11434/v1";
    c.model = "qwen3:14b";
    c.api_key = "ollama";
    c.temperature = 0.7;
    c.timeout_s = 300.0;
    return c;
}

/// 默认那套（OpenRouter + 按任务分流），只是补上密钥。
config::LLMConfig test_cfg_openrouter() {
    llm::reset_schema_support();
    config::LLMConfig c;
    c.api_key = "sk-or-v1-测试";
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

TEST_CASE("请求体的形状") {
    const auto cfg = test_cfg();
    const json p = json(llm::build_payload(cfg, simple_req(), true));

    CHECK(p.at("model") == "qwen3:14b");
    CHECK(p.at("temperature") == 0.7);
    REQUIRE(p.at("messages").is_array());
    REQUIRE(p.at("messages").size() == 1);
    CHECK(p.at("messages")[0].at("role") == "user");
    CHECK(p.at("messages")[0].at("content") == "写一集短剧");

    const json& rf = p.at("response_format");
    CHECK(rf.at("type") == "json_schema");
    CHECK(rf.at("json_schema").at("name") == "script");
    CHECK(rf.at("json_schema").at("strict") == true);
    CHECK(rf.at("json_schema").at("schema").at("type") == "object");

    SUBCASE("退回普通 JSON 模式") {
        const json q = json(llm::build_payload(cfg, simple_req(), false));
        CHECK(q.at("response_format").at("type") == "json_object");
        CHECK_FALSE(q.at("response_format").contains("json_schema"));
    }

    SUBCASE("没给 schema 就不加 response_format") {
        // 加一个空的会被某些服务直接拒掉
        llm::Request r = simple_req();
        r.schema = nlohmann::ordered_json();
        const json q = json(llm::build_payload(cfg, r, true));
        CHECK_FALSE(q.contains("response_format"));
    }
}

// ---------------------------------------------------------------------------
// 削 schema
//
// **这一条钉的是"我们最管用的那几条约束不会把整份 schema 一起带走"。**
// 兼容接口的 json_schema 严格模式碰到 minItems / minLength / pattern
// 这些校验关键字是**整份退回 400**，而 400 会触发"不带 schema 再发一次"
// 那条退路——于是「加了 minItems 让它没得选」变成这一次连字段名都没约束，
// 并且全程不报错。见 llm::remote_schema。
// ---------------------------------------------------------------------------

TEST_CASE("哪些地址要 API Key") {
    // 判错的代价不对称：把云当成本机，用户会被初始化页放过去、体检也报绿，
    // 然后在第一次写剧本时撞上 401；反过来只是多提示一句。所以认不准就当要。
    config::LLMConfig c;

    SUBCASE("默认那一档是 OpenRouter，必须自己填密钥") {
        // **这里曾经内置过一把智谱的免费密钥，2026-09-13 用户要求删掉。**
        // 别再往回加：密钥明文编进二进制，strings 一抓就有，也会进 git。
        CHECK(c.base_url.find("openrouter.ai") != std::string::npos);
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

TEST_CASE("发往 OpenRouter 的请求要关掉「先想再写」") {
    // **2026-09-13 实测出来的，不关它这条流水线在长任务上全军覆没。**
    // nemotron-3-super：6000 token 烧掉 5288 在思考上，content 字段装的
    // 和 reasoning 字段一模一样的 12962 字英文思考稿，JSON 一个字解不出；
    // nex-n2.5 和 ling-3.0 干脆回一个空的 content。
    // 同一个模型关掉之后：正文字数翻倍（507→955）、快 2.5 倍。
    //
    // 短提示词上试不出来——思考几句就够了，正文照样出得来。
    config::LLMConfig c = test_cfg_openrouter();
    FakeHttp http;
    http.responses.push_back(ok("{\"ok\":1}"));
    llm::RemoteClient rc(c, http.fn());
    pipeline::CancelToken tok;
    rc.complete(simple_req(), tok);

    REQUIRE(http.calls.size() == 1);
    const json body = http.calls[0].body;
    REQUIRE(body.contains("reasoning"));
    CHECK(body.at("reasoning").at("enabled") == false);

    SUBCASE("想开就开得回来") {
        c.reasoning = true;
        FakeHttp h;
        h.responses.push_back(ok("{\"ok\":1}"));
        llm::RemoteClient rc2(c, h.fn());
        pipeline::CancelToken t2;
        rc2.complete(simple_req(), t2);
        REQUIRE(h.calls.size() == 1);
        CHECK_FALSE(h.calls[0].body.contains("reasoning"));
    }

    SUBCASE("只对 OpenRouter 发这个字段") {
        // reasoning 是 OpenRouter 的统一参数，别家不认。发过去被当成非法
        // 字段整个打回的话，会被我们的退路误判成"这家不支持 json_schema"，
        // 白白退两档。
        FakeHttp h;
        h.responses.push_back(ok("{\"ok\":1}"));
        llm::RemoteClient rc3(test_cfg(), h.fn());   // 本机 Ollama 那套
        pipeline::CancelToken t3;
        rc3.complete(simple_req(), t3);
        REQUIRE(h.calls.size() == 1);
        CHECK_FALSE(h.calls[0].body.contains("reasoning"));
    }
}

TEST_CASE("按任务分流：哪一步用哪个模型") {
    // 用户 2026-09-13 定的方向："发挥各自的优势，不同功能使用不同的模型"。
    // 这条流水线要两种本事，而免费模型里没有一个两样都强：写正文要文采
    // （Inkling 榜上 72.5），拆分镜要听话（Nemotron 3 Super 是免费档里
    // 唯一带完整 structured_outputs 的）。
    config::LLMConfig c;

    // 写作那几步和结构那几步必须**不是同一个模型**——分流的全部意义
    // 就在这儿。具体是谁会随实测改（Inkling 就是这么被换掉的：
    // 榜上最高分，但 OpenRouter 回 403 限定入口，我们用不了）。
    CHECK(c.model_for("chapter") != c.model_for("storyboard"));
    CHECK(c.model_for("chapter") == c.model_for("premises"));
    CHECK(c.model_for("storyboard") == c.model_for("bible"));
    // 结构那几步要挑带 structured_outputs 的，兜底也是它
    CHECK(c.model_for("storyboard") == c.model);

    SUBCASE("没点名的任务落到兜底那个") {
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
        const config::LLMConfig cfg = test_cfg_openrouter();
        llm::Request r = simple_req();
        r.schema_name = "chapter";
        llm::RemoteClient rc(cfg, http.fn());
        pipeline::CancelToken tok;
        rc.complete(r, tok);
        REQUIRE(http.calls.size() == 1);
        CHECK(http.calls[0].body.at("model") == cfg.model_for("chapter"));
        CHECK(http.calls[0].body.at("model") != cfg.model);
    }
}

TEST_CASE("远端的 schema 要削掉严格模式不认的校验关键字") {
    const auto schema = nlohmann::ordered_json::parse(R"({
      "type": "object",
      "properties": {
        "beats": {
          "type": "array",
          "description": "四拍",
          "minItems": 4,
          "maxItems": 4,
          "uniqueItems": true,
          "items": {"type": "string", "minLength": 2, "maxLength": 80}
        },
        "score": {"type": "integer", "minimum": 0, "maximum": 10},
        "id": {"type": "string", "pattern": "^[a-z_]+$", "minLength": 3},
        "angle": {"type": "string", "enum": ["low", "eye_level"]}
      },
      "required": ["beats", "score"],
      "additionalProperties": false
    })");

    const auto out = llm::remote_schema(schema);
    const std::string dumped = json(out).dump();
    for (const char* kw : {"minItems", "maxItems", "uniqueItems", "minLength",
                           "maxLength", "pattern", "minimum", "maximum"}) {
        CAPTURE(kw);
        CHECK(dumped.find(std::string("\"") + kw + "\"") == std::string::npos);
    }

    // 结构、枚举、required、additionalProperties 一个都不能少——
    // 这些是严格模式**认**的，削过头等于自废武功。
    CHECK(out.at("required") == json::array({"beats", "score"}));
    CHECK(out.at("additionalProperties") == false);
    CHECK(out.at("properties").at("angle").at("enum") ==
          json::array({"low", "eye_level"}));
    CHECK(out.at("properties").at("beats").at("items").at("type") == "string");

    SUBCASE("那些数折进 description，不是丢掉") {
        // **远端这条路上模型是看得见 description 的**（整份 schema 进请求体），
        // 本地那条看不见（GBNF 只留结构）。所以折进描述之后这些数还在起
        // 作用，只是从"语法不让它少写"变成"它读得到"。
        const std::string beats =
            out.at("properties").at("beats").at("description").get<std::string>();
        CHECK(beats.find("四拍") == 0);          // 原来的描述还在最前面
        CHECK(beats.find("4~4 项") != std::string::npos);
        CHECK(beats.find("不要重复") != std::string::npos);

        // 本来没有描述的字段，折出来的那句就是它的描述
        CHECK(out.at("properties").at("score").at("description") == "（0~10）");
        CHECK(out.at("properties").at("beats").at("items").at("description") ==
              "（2~80 字）");
        // 一头有一头没有
        CHECK(out.at("properties").at("id").at("description") == "（至少 3 字）");
    }

    SUBCASE("叫 pattern 的字段不是关键字，不许摘") {
        // properties 下面挂的是属性名。不分这一支的话，一个真叫
        // pattern / format 的字段会被当成关键字整个摘掉，而表现是
        // 模型再也不填这个字段——不报错。
        const auto s = nlohmann::ordered_json::parse(R"({
          "type": "object",
          "properties": {
            "pattern": {"type": "string"},
            "format":  {"type": "string", "minLength": 1}
          }
        })");
        const auto r = llm::remote_schema(s);
        CHECK(r.at("properties").contains("pattern"));
        CHECK(r.at("properties").contains("format"));
        CHECK(r.at("properties").at("format").at("type") == "string");
        CHECK_FALSE(r.at("properties").at("format").contains("minLength"));
    }

    SUBCASE("$defs 里的也要削，$ref 不能动") {
        const auto s = nlohmann::ordered_json::parse(R"({
          "$defs": {"Line": {"type": "string", "maxLength": 12}},
          "type": "object",
          "properties": {"a": {"$ref": "#/$defs/Line"}}
        })");
        const auto r = llm::remote_schema(s);
        CHECK_FALSE(r.at("$defs").at("Line").contains("maxLength"));
        CHECK(r.at("$defs").at("Line").at("description") == "（最多 12 字）");
        CHECK(r.at("properties").at("a").at("$ref") == "#/$defs/Line");
    }

    SUBCASE("空 schema 原样回，别塞个空对象出去") {
        CHECK(llm::remote_schema(nlohmann::ordered_json()).is_null());
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

    SUBCASE("认证失败指向设置页") {
        for (const int code : {401, 403}) {
            const std::string s = llm::explain_status(cfg, code, "{}");
            CHECK(s.find("API Key") != std::string::npos);
            CHECK(s.find("设置页") != std::string::npos);
            CHECK(s.find(std::to_string(code)) != std::string::npos);
        }
    }

    SUBCASE("401 的两种情况说两句不同的话") {
        config::LLMConfig c;   // 默认：OpenRouter + 空密钥

        // 一、默认状态：密钥还没填。说"密钥不对"会把人支去检查一个他
        //     根本没填过的东西，而且该提哪儿领要说清楚。
        REQUIRE(c.api_key.empty());
        const std::string fresh = llm::explain_status(c, 401, "{}");
        CHECK(fresh.find("还没填") != std::string::npos);
        CHECK(fresh.find("openrouter.ai") != std::string::npos);
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
        CHECK(broke.find(":free") != std::string::npos);   // 指一条出路
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
    CHECK(http.calls[0].body.at("response_format").at("type") == "json_schema");
}

TEST_CASE("服务不收原样的 schema 时，先削一遍再说") {
    // 各家服务对"不支持这个 response_format"回的码五花八门，400、404、422
    // 都见过。判断哪个是"不支持"哪个是"真错了"不现实，所以一律往下退一档。
    //
    // **退的第一步是削 schema，不是直接丢掉 response_format。**
    // 会退 400 的多半是 OpenAI 那种严格模式——它拒的是 minItems 这类校验
    // 关键字，不是 json_schema 本身。削掉就过了，而结构约束还在。
    for (const int code : {400, 404, 422, 500}) {
        CAPTURE(code);
        FakeHttp http;
        http.responses.push_back(llm::HttpResponse{code, "{}", std::nullopt});
        http.responses.push_back(ok("{\"ok\":1}"));
        llm::RemoteClient c(test_cfg(), http.fn());
        pipeline::CancelToken tok;

        CHECK(c.complete(simple_req(), tok) == "{\"ok\":1}");
        REQUIRE(http.calls.size() == 2);
        CHECK(http.calls[0].body.at("response_format").at("type") == "json_schema");
        CHECK(http.calls[1].body.at("response_format").at("type") == "json_schema");
    }
}

TEST_CASE("远端两条路各自的加工：第一次削 schema，退回那次把 schema 写进提示词") {
    llm::Request r = simple_req();
    r.schema = nlohmann::ordered_json::parse(
        R"({"type":"object",
            "properties":{"beats":{"type":"array","minItems":4,
                                   "items":{"type":"string"}}}})");

    FakeHttp http;
    http.responses.push_back(llm::HttpResponse{400, "{}", std::nullopt});
    http.responses.push_back(llm::HttpResponse{400, "{}", std::nullopt});
    http.responses.push_back(ok("{\"beats\":[]}"));
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;
    CHECK(c.complete(r, tok) == "{\"beats\":[]}");
    REQUIRE(http.calls.size() == 3);

    // **第一下 schema 一个字不动**，minItems 照发。OpenRouter 这类网关
    // 根本不拒它，削早了等于白丢我们最管用的那根杠杆。
    const json zeroth = http.calls[0].body;
    CHECK(zeroth.at("response_format").at("json_schema").at("schema")
              .dump().find("minItems") != std::string::npos);
    CHECK(zeroth.at("messages")[0].at("content") == "写一集短剧");

    // 第二次：被拒了才削，minItems 摘掉、折进描述。
    const json first = http.calls[1].body;
    const json sent = first.at("response_format").at("json_schema").at("schema");
    CHECK(sent.dump().find("minItems") == std::string::npos);
    CHECK(sent.at("properties").at("beats").at("description") == "（至少 4 项）");
    // 提示词这两次都没被动过
    CHECK(first.at("messages")[0].at("content") == "写一集短剧");

    // 第三次：response_format 退成 json_object，schema 改从提示词里带过去。
    const json second = http.calls[2].body;
    CHECK(second.at("response_format").at("type") == "json_object");
    const std::string prompt =
        second.at("messages")[0].at("content").get<std::string>();
    CHECK(prompt.find("写一集短剧") == 0);
    CHECK(prompt.find("\"beats\"") != std::string::npos);
    CHECK(prompt.find("minItems") != std::string::npos);
}

TEST_CASE("回了 200 但不是 JSON，也要退一步重来") {
    // **2026-09-13 在 glm-4.7-flash 上实测到的，比 400 那条阴得多。**
    // 它对 response_format: json_schema 既不报错也不照做——回 200，内容是
    // 一段 markdown 散文。只按状态码判的话退路永远不触发：这一层把散文
    // 原样交出去，炸在调用方的 JSON 解析上，而真正的原因在日志里看不见。
    FakeHttp http;
    const std::string prose = "1. **暴风雨中，他独自伫立在天台边缘。**\n2. 她冲上天台。";
    http.responses.push_back(ok(prose));   // 第 0 档：原样的 schema
    http.responses.push_back(ok(prose));   // 第 1 档：削过的
    http.responses.push_back(ok("{\"beats\": [\"一\", \"二\", \"三\", \"四\"]}"));
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;

    CHECK(c.complete(simple_req(), tok) == "{\"beats\": [\"一\", \"二\", \"三\", \"四\"]}");
    REQUIRE(http.calls.size() == 3);
    CHECK(http.calls[0].body.at("response_format").at("type") == "json_schema");
    CHECK(http.calls[1].body.at("response_format").at("type") == "json_schema");
    CHECK(http.calls[2].body.at("response_format").at("type") == "json_object");
    // 退到 json_object 那一下才把 schema 写进提示词——不然这一下模型
    // 连字段名都不知道
    CHECK(http.calls[2].body.at("messages")[0].at("content")
              .get<std::string>()
              .find("JSON Schema") != std::string::npos);

    SUBCASE("本来就没要 JSON 的，散文是正常结果，不许重发") {
        FakeHttp h2;
        h2.responses.push_back(ok("就是一段话"));
        llm::Request r = simple_req();
        r.schema = nlohmann::ordered_json();   // 没 schema
        llm::RemoteClient c2(test_cfg(), h2.fn());
        pipeline::CancelToken t2;
        CHECK(c2.complete(r, t2) == "就是一段话");
        CHECK(h2.calls.size() == 1);
    }

    SUBCASE("验明之后不再白发第一下") {
        // **这一笔省的不是一点点。** glm-4.7-flash 实测：json_schema 那一下
        // 78.6 秒（吐一篇散文），退路那一下 12.4 秒（吐的是合规 JSON）。
        // 不记的话每次调用都要先白花那 78 秒，而免费档本来就限流。
        FakeHttp h;
        h.responses.push_back(ok("散文，不是 JSON"));   // 第 0 档
        h.responses.push_back(ok("散文，不是 JSON"));   // 第 1 档
        h.responses.push_back(ok("{\"a\":1}"));        // 第 2 档，成了
        h.responses.push_back(ok("{\"b\":2}"));        // 第 2 次调用：直奔第 2 档
        llm::RemoteClient c2(test_cfg(), h.fn());
        pipeline::CancelToken t2;

        CHECK(c2.complete(simple_req(), t2) == "{\"a\":1}");
        CHECK(c2.complete(simple_req(), t2) == "{\"b\":2}");
        // 4 而不是 6：第二轮没再白发前两档
        REQUIRE(h.calls.size() == 4);
        CHECK(h.calls[3].body.at("response_format").at("type") == "json_object");
    }

    SUBCASE("包在 ```json 里的不算不是 JSON") {
        // 解析阶段的括号扫描本来就兜着这种。为它多发一次请求是白花钱，
        // 在限流很紧的免费档上更是白白多等三十秒。
        FakeHttp h3;
        h3.responses.push_back(ok("{\"a\":1}"));
        llm::RemoteClient c3(test_cfg(), h3.fn());
        pipeline::CancelToken t3;
        CHECK(c3.complete(simple_req(), t3) == "{\"a\":1}");
        CHECK(h3.calls.size() == 1);
    }
}

TEST_CASE("连 response_format 都不收的服务，退到第三档") {
    // **OpenRouter 上一大半模型是这样的**（2026-09-13 查的清单）：
    // supported_parameters 里压根没有 response_format——Inkling、
    // Nemotron 3 Ultra、Ling 3.0、Laguna、North 都没有。只有两档的话，
    // 这些模型上两次请求都会被网关打回来，而它们其实只要把 schema 写在
    // 提示词里就写得出 JSON。
    FakeHttp http;
    for (int i = 0; i < 3; ++i) {
        http.responses.push_back(llm::HttpResponse{400, "{}", std::nullopt});
    }
    http.responses.push_back(ok("{\"ok\":1}"));
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;

    CHECK(c.complete(simple_req(), tok) == "{\"ok\":1}");
    REQUIRE(http.calls.size() == 4);
    CHECK(http.calls[0].body.at("response_format").at("type") == "json_schema");
    CHECK(http.calls[1].body.at("response_format").at("type") == "json_schema");
    CHECK(http.calls[2].body.at("response_format").at("type") == "json_object");
    // 最后一档：**整个 response_format 都不发**
    CHECK_FALSE(http.calls[3].body.contains("response_format"));
    // schema 还在，只是改走提示词
    CHECK(http.calls[3].body.at("messages")[0].at("content")
              .get<std::string>()
              .find("JSON Schema") != std::string::npos);

    SUBCASE("验明之后直接从最后一档起，不再白发前三次") {
        // **不能再调 test_cfg()**：那个 fixture 会把刚记下的那笔账清掉
        // （见它开头的 reset_schema_support）。这里要的正是"账还在"。
        config::LLMConfig same;
        same.base_url = "http://127.0.0.1:11434/v1";
        same.model = "qwen3:14b";
        same.api_key = "ollama";

        FakeHttp h;
        h.responses.push_back(ok("{\"again\":1}"));
        llm::RemoteClient c2(same, h.fn());
        pipeline::CancelToken t2;
        CHECK(c2.complete(simple_req(), t2) == "{\"again\":1}");
        REQUIRE(h.calls.size() == 1);
        CHECK_FALSE(h.calls[0].body.contains("response_format"));
    }
}

TEST_CASE("限流和认证错不该被当成「不支持 json_schema」") {
    // **2026-09-13 实测撞上的。** 智谱那个免费模型限流很勤，连发三次全是
    // 429 / code 1305「该模型当前请求量较大」。按"无条件退"的老规矩，
    // 每一次限流都会变成一次去掉 schema 的重发——**而第二次多半会成功**，
    // 于是这一集的分镜在没有任何结构约束的情况下生成完了，日志上一切正常。
    for (const int code : {401, 403, 408, 429}) {
        CAPTURE(code);
        FakeHttp http;
        http.responses.push_back(llm::HttpResponse{code, "{}", std::nullopt});
        http.responses.push_back(ok("这一条不该被发出去"));
        llm::RemoteClient c(test_cfg(), http.fn());
        pipeline::CancelToken tok;

        CHECK_THROWS_AS(c.complete(simple_req(), tok), llm::LlmError);
        CHECK(http.calls.size() == 1);   // 只发了一次，没有第二次
    }

    SUBCASE("剩下的照退不误") {
        for (const int code : {400, 404, 422, 500, 503}) {
            CAPTURE(code);
            FakeHttp http;
            http.responses.push_back(llm::HttpResponse{code, "{}", std::nullopt});
            http.responses.push_back(ok("{\"ok\":1}"));
            llm::RemoteClient c(test_cfg(), http.fn());
            pipeline::CancelToken tok;
            CHECK(c.complete(simple_req(), tok) == "{\"ok\":1}");
            CHECK(http.calls.size() == 2);
        }
    }
}

TEST_CASE("四档都失败就报错，带上人话") {
    // 退路是四档（原样 schema / 削过的 schema / json_object / 什么都不发），
    // 全撞墙才轮到报错。报的是最后那一下的状态码。
    FakeHttp http;
    const std::string err = R"({"error": "model 'qwen3:14b' not found"})";
    for (int i = 0; i < 4; ++i) {
        http.responses.push_back(llm::HttpResponse{404, err, std::nullopt});
    }
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;

    try {
        c.complete(simple_req(), tok);
        FAIL("该抛异常");
    } catch (const llm::LlmError& e) {
        const std::string msg = e.what();
        CHECK(msg.find("ollama pull") != std::string::npos);
    }
    CHECK(http.calls.size() == 4);
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

// ---------------------------------------------------------------------------
// 发给大模型服务的请求体，和 Python 逐字段比。
//
// 上面那条「请求体的形状」钉的是我们自己的意图。请求体是**真的发到外部
// 服务上的东西**：temperature 差一点、response_format 少一层、strict 没
// 带上，模型回来的就是另一种东西——而两边都会"成功"，差异要到成片里
// 才看得出来。
//
// **一处结构差异**：Python 是三个阶段各拼各的（bible.py / script.py /
// storyboard.py 里各有一份 _complete），C++ 是一个 build_payload 三处共用。
// 所以语料按阶段导，这边用同样的输入调那一个函数——共用的那份要是漏了
// 某个阶段的特殊处理，就在这里露出来。
// ---------------------------------------------------------------------------

TEST_CASE("请求体和 Python 逐字段一样") {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/llm_payload.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    nlohmann::json g;
    in >> g;

    const auto cases = g.at("cases");
    // 语料读空了循环一次都不转，而用例照样绿。
    REQUIRE(cases.size() == 9);

    config::LLMConfig cfg;
    cfg.model = g.at("model").get<std::string>();
    cfg.temperature = g.at("temperature").get<double>();

    for (const auto& c : cases) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);

        llm::Request req;
        req.prompt = g.at("prompt").get<std::string>();
        req.schema_name = c.at("schema_name").get<std::string>();
        if (!c.at("schema").is_null()) {
            req.schema = nlohmann::ordered_json::parse(c.at("schema").dump());
        }

        const auto got = llm::build_payload(
            cfg, req, c.at("json_schema_mode").get<bool>());

        // 键的顺序不算契约（JSON 对象无序），值要一模一样。
        CHECK(nlohmann::json::parse(got.dump()) == c.at("payload"));
    }
}

TEST_CASE("[llm].backend 选哪条后端") {
    // 没编进程内大模型的构建里，配 local 也要能跑——退回远端，
    // 别抛。用户多半只是拿了个不带 llama 的构建，而远端只要地址填了就能用。
    auto dummy_post = [](const std::string&, const std::string&,
                         const std::map<std::string, std::string>&, double) {
        llm::HttpResponse r;
        r.status = 200;
        r.body = R"({"choices":[{"message":{"content":"好"}}]})";
        return r;
    };
    const auto c = llm::make_client(dummy_post);
    REQUIRE(c != nullptr);

    // 这个测试目标是不带 llama 编的，所以拿到的一定是远端那条。
    // 真装了 llama 的构建里 backend=local 才会给 LocalClient——
    // 那条要真模型才跑得动，不在单元测试里验。
    CHECK_FALSE(infer::llama_chat_available());
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

TEST_CASE("远端 SSE：服务不认 json_schema 就不带 schema 再来一次") {
    FakeHttp http;
    FakeStream stream;
    stream.statuses = {400, 400, 200};
    stream.error_body = R"({"error":{"message":"response_format not supported"}})";
    stream.chunks = {{}, {}, {sse_chunk("{\\\"ok\\\":1}"), "data: [DONE]\n\n"}};

    llm::RemoteClient c(test_cfg(), http.fn(), stream.fn());
    pipeline::CancelToken tok;
    const std::string out = c.complete(simple_req(), tok, [](const std::string&) {});

    CHECK(out == "{\"ok\":1}");
    REQUIRE(stream.calls.size() == 3);
    // 前两次都是 json_schema（原样的、削过的），第三次才退回 json_object
    //（不是把 response_format 整个去掉——那是再下一档。退到这儿结构已经
    // 没保证了，全靠提示词里那份 schema 和解析阶段的括号扫描兜底）
    CHECK(stream.calls[0].body.at("response_format").at("type") == "json_schema");
    CHECK(stream.calls[1].body.at("response_format").at("type") == "json_schema");
    CHECK(stream.calls[2].body.at("response_format").at("type") == "json_object");
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
