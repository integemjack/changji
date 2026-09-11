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

    SUBCASE("限流和服务端错误") {
        CHECK(llm::explain_status(cfg, 429, "{}").find("太频繁") !=
              std::string::npos);
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

TEST_CASE("服务不支持 json_schema 时退回普通模式") {
    // 各家服务对"不支持这个 response_format"回的码五花八门，400、404、422
    // 都见过。判断哪个是"不支持"哪个是"真错了"不现实，所以无条件重试一次。
    for (const int code : {400, 404, 422, 500}) {
        CAPTURE(code);
        FakeHttp http;
        http.responses.push_back(llm::HttpResponse{code, "{}", std::nullopt});
        http.responses.push_back(ok("退回之后成功了"));
        llm::RemoteClient c(test_cfg(), http.fn());
        pipeline::CancelToken tok;

        CHECK(c.complete(simple_req(), tok) == "退回之后成功了");
        REQUIRE(http.calls.size() == 2);
        CHECK(http.calls[0].body.at("response_format").at("type") == "json_schema");
        CHECK(http.calls[1].body.at("response_format").at("type") == "json_object");
    }
}

TEST_CASE("两次都失败就报错，带上人话") {
    FakeHttp http;
    const std::string err = R"({"error": "model 'qwen3:14b' not found"})";
    http.responses.push_back(llm::HttpResponse{404, err, std::nullopt});
    http.responses.push_back(llm::HttpResponse{404, err, std::nullopt});
    llm::RemoteClient c(test_cfg(), http.fn());
    pipeline::CancelToken tok;

    try {
        c.complete(simple_req(), tok);
        FAIL("该抛异常");
    } catch (const llm::LlmError& e) {
        const std::string msg = e.what();
        CHECK(msg.find("ollama pull") != std::string::npos);
    }
    CHECK(http.calls.size() == 2);
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
