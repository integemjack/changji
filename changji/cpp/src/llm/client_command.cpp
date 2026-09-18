// 把本机装着的大模型命令行当后端（`[llm].backend = "command"`）。
//
// 用户 2026-09-18：「增加对 claudecode 和 codex 命令版的支持」。
//
// 这一条路只干一件事：**提示词从标准输入喂进去，把它印出来的当模型的回答。**
// 别的一概复用远端那条——schema 本来就是贴在提示词后面的文字
// （`schema_as_prompt`），本地校验也是同一份，所以结构化输出这边不用另写。

#include <algorithm>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "llm/client.hpp"
#include "pipeline/jobs.hpp"
#include "util/proc.hpp"

namespace changji::llm {

namespace {

/// 两个已知命令行的默认参数。
///
/// ⚠️ **写这段时只有 claude（2.1.272）能在手边验，codex 没装。**
/// 所以这张表是"预设"，不是"支持声明"：填错了用户在 `[llm].command_args`
/// 里改一行就能救，而硬编进逻辑里就得重发一个引擎。
///
/// claude 那一串每一项的理由：
///   -p                       非交互，印完就退（帮助里写的就是 useful for pipes）
///   --output-format text     只要正文。json 那档还得再解一层，而我们要的
///                            就是那段字
///   --no-session-persistence 别把每一次生成都存成一个会话；这条流水线
///                            一部剧要叫模型上百次
///   --restricted             **去掉会跑命令和改文件的那些工具**。我们要的
///                            只是"一段字进、一段字出"，而它默认是个带
///                            Bash 和文件编辑的 agent——提示词里全是剧本
///                            正文，不该有任何机会变成对这台机器的操作
///
/// ⚠️ **别加 --bare。** 它写着"OAuth 和钥匙串一概不读，只认 ANTHROPIC_API_KEY"，
/// 而走这条路的人图的正是订阅登录。实测加了就是
/// 「Not logged in · Please run /login」。
const std::vector<std::string>& claude_args() {
    static const std::vector<std::string> v{
        "-p", "--output-format", "text", "--no-session-persistence", "--restricted"};
    return v;
}

/// codex 的非交互子命令。**没在本机验过**，理由见上。
const std::vector<std::string>& codex_args() {
    static const std::vector<std::string> v{"exec", "-"};
    return v;
}

/// 路径里的文件名（不含扩展名），用来认命令。
/// 配置里填的可能是 `claude`，也可能是 `/Users/x/.local/bin/claude`
/// 或者 Windows 上的 `claude.cmd`。
std::string base_name(const std::string& command) {
    std::string s = command;
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const auto dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) s = s.substr(0, dot);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

std::vector<std::string> default_command_args(const std::string& command) {
    const std::string name = base_name(command);
    if (name == "claude") return claude_args();
    if (name == "codex") return codex_args();
    return {};
}

CommandClient::CommandClient(ConfigProvider cfg) : cfg_(std::move(cfg)) {}

CommandClient::CommandClient(config::LLMConfig cfg)
    : cfg_([c = std::move(cfg)] { return c; }) {}

std::string CommandClient::complete(const Request& req, pipeline::CancelToken& tok) {
    const config::LLMConfig cfg = cfg_();
    if (cfg.command.empty()) {
        throw LlmError("没填 llm.command：要跑哪个程序（比如 claude 或 codex）");
    }
    // 开跑前先看一眼有没有被取消。**命令行这条路中途打不断**——和远端那条
    // 一样，能查的只有请求前后两个点（见 Client::complete 那段注释）。
    if (tok.cancelled()) throw LlmError("已取消");

    std::vector<std::string> args = cfg.command_args;
    if (args.empty()) args = default_command_args(cfg.command);

    // schema 贴进提示词——和远端那条同一个函数，不另写一份。
    const std::string prompt =
        req.schema.is_null() || req.schema.empty()
            ? req.prompt
            : schema_as_prompt(req.prompt, req.schema);

    const int timeout_ms = static_cast<int>(cfg.command_timeout_s * 1000.0);
    const auto r = proc::run(cfg.command, args, timeout_ms, prompt);

    // **「没装这个程序」和「装了但报错」要分开说。** 这是 proc::Result
    // 特意留 launched 这个标志的原因（media/ffmpeg.cpp 也是这么用的）：
    // 两句话把人指向完全不同的方向——一个是去装、一个是去看它说了什么。
    if (!r.launched) {
        throw LlmError("找不到 " + cfg.command +
                       "：它不在 PATH 上。装好之后在设置页里把路径填全，"
                       "或者把 [llm].command 写成绝对路径");
    }
    if (r.timed_out) {
        throw LlmError(cfg.command + " 跑了 " +
                       std::to_string(static_cast<int>(cfg.command_timeout_s)) +
                       " 秒还没写完，被掐断了。要么这一步太大，"
                       "要么它在等什么（比如要登录）。把 llm.command_timeout_s 调大，"
                       "或者先在终端里手敲一次看看它说什么");
    }
    if (tok.cancelled()) throw LlmError("已取消");
    if (r.exit_code != 0) {
        // **把它自己那句话原样带出来。** 实测最常见的一条是
        // 「Failed to authenticate: OAuth session expired」——那正是用户
        // 需要知道的，换成"命令行后端失败"等于把理由吃掉。
        // 它走的是 stdout（实测），而 proc::Result 把两股流并在 out 里。
        std::string tail = r.out;
        constexpr std::size_t kMax = 500;
        if (tail.size() > kMax) tail = tail.substr(0, kMax) + "…";
        throw LlmError(cfg.command + " 退出码 " + std::to_string(r.exit_code) +
                       "：" + (tail.empty() ? "什么都没说" : tail));
    }

    // 跑通了但一个字没吐。**不能当成空回答往下走**：下游按 schema 解析
    // 空串会报一句和这件事毫无关系的解析错。
    std::string out = r.out;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (out.empty()) {
        throw LlmError(cfg.command + " 跑通了，但一个字都没印出来。"
                       "先在终端里手敲一次同样的命令看看");
    }
    return out;
}

}  // namespace changji::llm
