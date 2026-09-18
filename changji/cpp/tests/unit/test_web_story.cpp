// 从网上找热点写故事：工具的解析、带工具的那条对话。
//
// 上网走假的 HttpGet，按网址回固定的页面。真上网不在单测里。

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "llm/client.hpp"
#include "stages/story_from_web.hpp"
#include "stages/web_tools.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

/// 假的网：按网址开头回一段。
stages::WebTools fake_web(std::map<std::string, std::string> pages) {
    stages::WebTools w;
    w.get = [pages](const std::string& url, const std::map<std::string, std::string>&,
                    double) -> llm::HttpResponse {
        llm::HttpResponse r;
        for (const auto& [prefix, body] : pages) {
            if (url.rfind(prefix, 0) == 0) {
                r.status = 200;
                r.body = body;
                return r;
            }
        }
        r.status = 404;
        return r;
    };
    return w;
}

std::string baidu_json() {
    return json{{"data", {{"cards", json::array({json{{"content", json::array({
                    json{{"word", "夜班护士改药记录"}, {"desc", "一支针剂牵出师徒往事"}, {"url", "https://b/1"}},
                    json{{"word", "外卖员和程序员"}, {"desc", ""}, {"url", "https://b/2"}},
                })}}})}}}}.dump();
}

std::string toutiao_json() {
    return json{{"data", json::array({json{{"Title", "县城修车铺的一年"}, {"Url", "https://t/1"}}})}}.dump();
}

std::string chapter_json() {
    return json{{"title", "夜班"},
                {"source", "百度热搜：夜班护士改药记录"},
                {"text", "凌晨两点，走廊只亮一半。\n\n她把那张纸摊开。"}}
        .dump();
}

models::Story two_chapters() {
    models::Story s;
    s.logline = "一支针剂把师徒俩压在真相与人情之间";
    models::Chapter a;
    a.chapter_id = "ch01";
    a.title = "稳妥的字";
    a.text = "第一章的正文，结尾是：她认出了那把伞。";
    models::Chapter b;
    b.chapter_id = "ch02";
    b.title = "第 2 章";
    s.chapters = {a, b};
    return s;
}

}  // namespace

TEST_CASE("热榜解析：三家的 JSON 各认各的，合起来去重") {
    const auto b = stages::parse_baidu_hot(baidu_json());
    REQUIRE(b.size() == 2);
    CHECK(b[0].title == "夜班护士改药记录");
    CHECK(b[0].note == "一支针剂牵出师徒往事");
    CHECK(b[0].source == "百度");
    const auto t = stages::parse_toutiao_hot(toutiao_json());
    REQUIRE(t.size() == 1);
    CHECK(t[0].title == "县城修车铺的一年");
    const auto w = stages::parse_weibo_hot(
        json{{"data", {{"realtime", json::array({json{{"word", "某地暴雨"}, {"note", "预警"}}})}}}}.dump());
    REQUIRE(w.size() == 1);
    CHECK(w[0].url.find("s.weibo.com") != std::string::npos);
}

TEST_CASE("必应结果页：标题、链接、摘要各取各的，标签剥掉") {
    const std::string html =
        "<ol><li class=\"b_algo\"><h2><a href=\"https://a.example/x\">夜班<b>护士</b>的故事</a></h2>"
        "<div><p>一支针剂&amp;一段往事</p></div></li>"
        "<li class=\"b_algo\"><h2><a href=\"https://a.example/y\">第二条</a></h2></li></ol>";
    const auto hits = stages::parse_bing_results(html);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].title == "夜班护士的故事");
    CHECK(hits[0].url == "https://a.example/x");
    CHECK(hits[0].snippet == "一支针剂&一段往事");
    CHECK(hits[1].snippet.empty());
}

TEST_CASE("网页转正文：脚本样式扔掉，块级标签换行，实体解开") {
    const std::string html =
        "<html><head><style>p{}</style><script>var x=1;</script></head>"
        "<body><h1>标题</h1><p>第一段&nbsp;有&lt;符号&gt;</p><!-- 注释 --><div>第二段</div></body></html>";
    const std::string t = stages::html_to_text(html);
    CHECK(t.find("var x") == std::string::npos);
    CHECK(t.find("p{}") == std::string::npos);
    CHECK(t.find("注释") == std::string::npos);
    CHECK(t.find("标题\n") != std::string::npos);
    CHECK(t.find("第一段 有<符号>") != std::string::npos);
    CHECK(t.find("第二段") != std::string::npos);
}

TEST_CASE("run_web_tool：不认识的名字、坏参数都回一句话，不抛") {
    const stages::WebTools web = fake_web({});
    CHECK(stages::run_web_tool(web, "nope", "{}").find("没有叫") != std::string::npos);
    CHECK(stages::run_web_tool(web, "web_search", "不是json").find("参数不是") != std::string::npos);
    CHECK(stages::run_web_tool(web, "fetch_page", R"({"url":"ftp://x"})").find("http") != std::string::npos);
    CHECK(stages::url_encode("a b/中") == "a%20b%2F%E4%B8%AD");
}

TEST_CASE("那条对话：模型要看热榜、读一页，结果喂回去，最后写成这一章") {
    // 用户 2026-09-18：「点击直接让大语言模型使用 tools 从网上获取热门内容
    // 改写成一个完整的故事」「这次写的就只是这一章内容」。演三轮：看热榜 →
    // 读一页 → 写。
    const stages::WebTools web = fake_web({
        {"https://top.baidu.com/", baidu_json()},
        {"https://www.toutiao.com/", toutiao_json()},
        {"https://weibo.com/", "not json"},
        {"https://b/1", "<html><body><p>护士站的灯亮了一夜。</p></body></html>"},
    });
    llm::ReplayClient client({
        json{{"tool_calls", json::array({json{{"name", "hot_topics"}, {"arguments", "{}"}}})}}.dump(),
        json{{"tool_calls", json::array({json{{"name", "fetch_page"},
                                              {"arguments", json{{"url", "https://b/1"}}}}})}}.dump(),
        chapter_json(),
    });
    pipeline::CancelToken tok;
    std::vector<std::string> steps;
    stages::WebStoryHooks hooks;
    hooks.on_step = [&](const std::string& s) { steps.push_back(s); };

    const stages::WebChapter wc = stages::write_chapter_from_web(
        client, web, models::StyleLine::REALISTIC, two_chapters(), "ch02", tok, hooks);
    CHECK(wc.title == "夜班");
    CHECK(wc.text.find("凌晨两点") == 0);
    CHECK(wc.source.find("百度") != std::string::npos);
    CHECK(client.calls().size() == 3);

    // 工具的结果确实喂回去了：热榜那两家的标题、读出来的正文，都在 tool 那几条里
    std::string tools_said;
    for (const auto& m : client.last_messages()) {
        if (m.role == "tool") tools_said += m.content + "\n";
    }
    CHECK(tools_said.find("夜班护士改药记录") != std::string::npos);
    CHECK(tools_said.find("县城修车铺的一年") != std::string::npos);
    CHECK(tools_said.find("微博") != std::string::npos);   // 没拿到的也说了
    CHECK(tools_said.find("护士站的灯亮了一夜") != std::string::npos);

    // 开场那条是一句话 + 上下文 + schema，没有规矩表。写的是 ch02：带着
    // 这部电影那句话、前一章的结尾、这一章的标题，不分章
    const std::string opening = client.last_messages()[1].content;
    CHECK(opening.find("hot_topics") != std::string::npos);
    CHECK(opening.find("这一章的正文") != std::string::npos);
    CHECK(opening.find("分 ") == std::string::npos);
    CHECK(opening.find("一支针剂把师徒俩") != std::string::npos);
    CHECK(opening.find("她认出了那把伞") != std::string::npos);
    CHECK(opening.find("第 2 章") != std::string::npos);
    CHECK(opening.find("硬性要求") == std::string::npos);

    REQUIRE(steps.size() == 3);
    CHECK(steps[0] == "在看热搜");
    CHECK(steps[1].find("在读") == 0);
    CHECK(steps[2] == "在写");
}

TEST_CASE("那条对话：一直要工具不开口，十轮就停") {
    const stages::WebTools web = fake_web({});
    std::vector<std::string> replies;
    for (int i = 0; i < 12; ++i) {
        replies.push_back(json{{"tool_calls", json::array({json{{"name", "hot_topics"}, {"arguments", "{}"}}})}}.dump());
    }
    llm::ReplayClient client(replies);
    pipeline::CancelToken tok;
    CHECK_THROWS(stages::write_chapter_from_web(client, web, models::StyleLine::REALISTIC,
                                                two_chapters(), "ch01", tok, {}));
    CHECK(client.calls().size() == 10);
}
