// 命令行后端：把本机装着的 claude / codex 当大模型用。
//
// 用户 2026-09-18：「增加对 claudecode 和 codex 命令版的支持」。
//
// **这里一个真模型都不叫。** 拿 cat / findstr 当替身：提示词从标准输入进去、
// 原样印出来，那正是这条路要保证的"一段字进、一段字出"。真去叫 claude 的话
// 这套测试要花钱、要联网、还要那台机器登录过——三个条件哪个不满足都会红，
// 而红的原因和代码无关。
//
// 盯四件事，每一件都是"错了不会当场报错"的那一类：
//
//   **提示词真的送进去了**。走的是标准输入不是命令行参数——几万字塞不进
//   参数（Windows 32 KB 封顶）。这条错了的表现是模型在写一个空题目。
//
//   **schema 要贴在提示词后面**。远端那条路本来就是这么发的
//   （schema_as_prompt），两边共用同一个函数；这边漏了的话，模型在"随便
//   吐个 JSON"，而调用方按字段去解析它。
//
//   **「没装」和「装了但报错」要分开说**。两句话把人指向完全不同的方向。
//
//   **跑通但一个字没吐，不能当成空回答**。下游按 schema 解析空串会报一句
//   和这件事毫无关系的解析错。

#include <doctest/doctest.h>

#include <string>

#include "config/settings.hpp"
#include "llm/client.hpp"
#include "pipeline/jobs.hpp"

using namespace changji;

namespace {

/// 原样回显标准输入的那个命令。
config::LLMConfig echo_cfg() {
    config::LLMConfig c;
    c.backend = "command";
#ifdef _WIN32
    c.command = "findstr.exe";
    c.command_args = {"^"};  // 匹配每一行 = 原样回显
#else
    c.command = "cat";
    c.command_args = {};
#endif
    c.command_timeout_s = 30.0;
    return c;
}

}  // namespace

TEST_CASE("命令行后端：提示词从标准输入进去，印出来的就是回答") {
    llm::CommandClient cli(echo_cfg());
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "changji_prompt_marker 这一章写什么";
    const std::string out = cli.complete(req, tok);
    CHECK(out.find("changji_prompt_marker") != std::string::npos);
    CHECK(out.find("这一章写什么") != std::string::npos);
}

TEST_CASE("命令行后端：几万字的提示词也要整份送到") {
    // 真实的提示词就是这个量级。参数那条路在 Windows 上会被 32 KB 顶回来，
    // 而标准输入没有这个限制——这条盯着别有人"顺手"改回去用参数传。
    llm::CommandClient cli(echo_cfg());
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "开头marker\n";
    while (req.prompt.size() < 120000) req.prompt += "正文一行。\n";
    req.prompt += "结尾marker";
    const std::string out = cli.complete(req, tok);
    CHECK(out.find("开头marker") != std::string::npos);
    CHECK_MESSAGE(out.find("结尾marker") != std::string::npos,
                  "只送到了前面一截——多半是被参数长度或者管道缓冲截了");
}

TEST_CASE("命令行后端：schema 贴在提示词后面，和远端那条一个样") {
    llm::CommandClient cli(echo_cfg());
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "拆这一章的镜头";
    req.schema = nlohmann::ordered_json{
        {"type", "object"},
        {"properties", {{"changji_field_marker", {{"type", "string"}}}}}};
    const std::string out = cli.complete(req, tok);
    CHECK_MESSAGE(out.find("changji_field_marker") != std::string::npos,
                  "schema 没跟着提示词一起发出去");
    // 和远端那条共用同一个函数，不是各写一份
    CHECK(out.find(llm::schema_as_prompt(req.prompt, req.schema)) != std::string::npos);
}

TEST_CASE("命令行后端：没装那个程序，要说「找不到」而不是别的") {
    config::LLMConfig c = echo_cfg();
    c.command = "changji_这个程序不存在_98765";
    llm::CommandClient cli(c);
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "随便";
    CHECK_THROWS_WITH_AS(cli.complete(req, tok),
                         doctest::Contains("找不到"), llm::LlmError);
}

TEST_CASE("命令行后端：没填 command 要当场说清，不是跑到一半才失败") {
    config::LLMConfig c;
    c.backend = "command";
    c.command.clear();
    llm::CommandClient cli(c);
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "随便";
    CHECK_THROWS_AS(cli.complete(req, tok), llm::LlmError);
}

TEST_CASE("命令行后端：退出码非 0 时，把它自己那句话带出来") {
    // 实测最常见的一条是「Failed to authenticate: OAuth session expired」，
    // 那正是用户需要知道的。换成"命令行后端失败"等于把理由吃掉。
    config::LLMConfig c = echo_cfg();
#ifdef _WIN32
    c.command = "findstr.exe";
    c.command_args = {"/c:这个一定匹配不上的字符串_zzz"};  // 匹配不到就退 1
#else
    c.command = "grep";
    c.command_args = {"这个一定匹配不上的字符串_zzz"};
#endif
    llm::CommandClient cli(c);
    pipeline::CancelToken tok;
    llm::Request req;
    req.prompt = "别的内容";
    CHECK_THROWS_WITH_AS(cli.complete(req, tok),
                         doctest::Contains("退出码"), llm::LlmError);
}

TEST_CASE("默认参数表：claude 认得出，而且不许出现 --bare") {
    const auto a = llm::default_command_args("claude");
    REQUIRE(!a.empty());
    const std::string joined = [&] {
        std::string s;
        for (const auto& x : a) s += x + " ";
        return s;
    }();
    CHECK(joined.find("-p") != std::string::npos);
    // **--bare 会把 OAuth 和钥匙串一概关掉**，而走这条路的人图的正是订阅
    // 登录。实测加了就是「Not logged in · Please run /login」。
    CHECK_MESSAGE(joined.find("--bare") == std::string::npos,
                  "--bare 会让订阅登录失效");
    // 它默认是个带 Bash 和文件编辑的 agent，而我们只要一段字
    CHECK_MESSAGE(joined.find("--restricted") != std::string::npos,
                  "没把会跑命令、改文件的那些工具去掉");
}

TEST_CASE("默认参数表：带路径、带扩展名、大小写都要认得出来") {
    // 配置里填的可能是 `claude`，也可能是绝对路径或者 Windows 上的 .cmd
    CHECK(!llm::default_command_args("/Users/x/.local/bin/claude").empty());
    CHECK(!llm::default_command_args("C:\\tools\\Claude.CMD").empty());
    CHECK(!llm::default_command_args("codex").empty());
    // 认不出的不猜：交给用户自己在 command_args 里写
    CHECK(llm::default_command_args("my-own-llm").empty());
}
