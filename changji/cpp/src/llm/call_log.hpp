#pragma once

// 每一次给大模型发了什么、收回什么，原样留在磁盘上。
//
// 用户 2026-09-18：「将所有的提交提示词给大模型的地方和获取结果的地方都以
// 日志保存下来放到程序统计目录里，我要研究。」
//
// **判据只有一条：看着日志能还原一次调用。** 哪一步、哪个模型、什么温度、
// 模型真正收到的是哪段字、它吐回来什么、想了什么、花了多久、对面回了什么
// 状态码。**成功和失败一样记**——失败那一半正是最想研究的（什么提示词换来
// 一个 400、什么提示词让 schema 本地校验没过）。
//
// ---- 落在哪儿、长什么样 ----
//
//   <paths::user_data_dir("changji")>/llm_log/<项目名>/
//       index.jsonl        一行一次调用，追加写，**永不删**
//       <id>.prompt.txt    模型真正收到的那段字（贴完 schema 之后的）
//       <id>.reply.txt     模型吐回来的原始正文
//       <id>.thinking.txt  有思考才写
//       <id>.tools.json    chat 那条且真发了工具表才写
//       <id>.error.txt     失败才写
//
// **为什么是「索引 JSONL + 每次一对纯文本」**：统计只读 index.jsonl，一行
// 几百字节，jq / pandas 直接吃；提示词和回复**原样**存成 .txt，想比两次提示
// 词差在哪儿就 diff，想找哪次提到过某个词就 grep。塞进 JSON 的话，几千字正文
// 会被转义成一行没法看的东西。
//
// ---- 三条定死的，别改 ----
//
// 1. **`Authorization` 一个字节都不落盘。** 密钥在请求头里，payload 里没有；
//    所以整个 headers 都不记，只记 base_url。命令行那条记命令名和参数。
// 2. **`ReplayClient` 不接。** 它只给测试和回放用，接上去的话跑一次单元测试
//    就往用户的数据目录里写一千多份文件。
// 3. **写盘出问题不许影响生成。** 记不下就算了——本来会成功的那一次，不许
//    因为记日志而失败。这一条落在 `CallLog` 的析构里：全程吞异常。
//
// ⚠️ **不许 include `util/httplib.hpp`**：这一层和 llm/client.cpp 一样进测试
// 目标，而那个目标一个网络库都不链（见 cpp/CMakeLists.txt 里那段注释）。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "llm/client.hpp"   // Request

namespace changji::llm {

/// 这一次记到哪儿、记不记。
struct CallLogOptions {
    /// 不记就整个不动手：不建目录、不取 id、不读线程局部的那两样。
    bool enabled = true;

    /// 落盘的根（就是 `<数据目录>/llm_log` 那一层，项目子目录在它底下开）。
    ///
    /// **空 = 生产路径** `paths::user_data_dir("changji") / "llm_log"`。
    /// 测试往这儿塞临时目录——**这是可测性的前提，别去掉**：不能注入的话，
    /// 跑一次单元测试就往这台机器上真实的数据目录里写文件。
    std::filesystem::path root;

    /// 正文文件的总字节上限，超了从最旧的那一次开始删。
    ///
    /// ⚠️ **`index.jsonl` 不在此列，它永远不删、也不计入这个数。** 一行几百
    /// 字节，一百万次调用也才几百 MB，而它是统计的依据；剪过之后那些行还在，
    /// 只是点不开原文。做成"统计数据也跟着没了"的话，这个功能就只剩最近那
    /// 几天能用。**这是最容易被下一个人"顺手优化"掉的一条。**
    std::uintmax_t max_bytes = 1024ull * 1024 * 1024;   // 1 GB
};

/// 从配置里取一份。看的是 `[llm].call_log` / `[llm].call_log_max_mb`。
///
/// 还认一个环境变量 `CHANGJI_LLM_LOG_DIR`：非空时盖掉 `root`。
/// **给研究用的**——把一批日志单独收到一个目录里跑对比，不必去动设置页，
/// 也不会和平时那一堆混在一起。
CallLogOptions call_log_options(const config::LLMConfig& cfg);

/// 一次调用的记录器。往栈上一放，析构那一下一次性写完。
///
/// **RAII + 显式 `fail()`**：构造即开始计时，析构即落盘；出入口那头在 catch
/// 里调一次 `fail()` 把翻译过的那句话和状态码交进来，然后**原样 `throw;`**
/// 往外走。中途从哪条路径抛出去都会落盘——这正是"失败那一半"能留下来的原因。
///
/// **绝不抛。** 析构里任何一步失败（磁盘满、建不了目录、没权限）都吞掉。
///
/// ⚠️ **不是线程安全的，也不需要是。** 一个 `CallLog` 从头到尾只被一条线程
/// 碰，**包括 `append_thinking` / `mark_first_token` 这两句**：流式那条的
/// `content_receiver` 跑在 `client_http.cpp` 里那次**阻塞** `cli.send()` 的
/// 调用线程上，和构造、析构是同一条。哪天流式换成真异步，这句话就不成立了
/// ——`impl_->thinking += piece` 和析构里读它就是 data race，症状是偶发的
/// `.thinking.txt` 截断或者崩，而且只在流式长活上出现。那时候要给 Impl 加锁。
/// （多个 `CallLog` 同时往 index.jsonl 追加是另一回事，那一处有锁。）
class CallLog {
public:
    /// `backend` 是 "remote" / "command"；`kind` 是 "complete" /
    /// "complete_stream" / "chat"。`req` 只读 `schema_name`。
    CallLog(std::string backend, std::string kind, const Request& req,
            CallLogOptions opt);
    ~CallLog();

    CallLog(const CallLog&) = delete;
    CallLog& operator=(const CallLog&) = delete;
    CallLog(CallLog&&) = delete;
    CallLog& operator=(CallLog&&) = delete;

    // ---- 发出去之前填 ----

    /// 远端传 `cfg.base_url`，命令行传 `命令名 参数 参数…`。
    /// **不许传带密钥的整串**，也不许传 headers。
    void set_endpoint(std::string endpoint);

    /// **分流之后**的那三样。远端在 `cfg.model_for` / `cfg.temperature_for`
    /// 算完之后调；`temperature` 传真正写进 payload 的那个数。
    ///
    /// ⚠️ **命令行那条路上这三个数是"配置里写着什么"，不是"发出去了什么"**
    /// ——`CommandClient` 一个参数都不发，用哪个模型、什么温度由那个命令行
    /// 自己定。研究时别把它当真值。（那也不能记成空串：空着的话按 `model`
    /// 分组就把命令行这条全归进一个空桶，而它其实是按 schema_name 分流过的。）
    void set_model(std::string model, double temperature,
                   std::string reasoning_effort);

    /// 模型真正收到的那段字。`CommandClient` 走这条（喂进 stdin 的那一份）。
    void set_prompt(std::string prompt);

    /// 同上，但从 `build_payload` / chat 拼出来的 payload 里抽。
    /// 内部就是 `set_prompt(render_prompt(payload["messages"]))`；
    /// 没有 `messages` 或它不是数组时留空，不抛。
    void set_prompt_from_payload(const nlohmann::ordered_json& payload);

    /// 工具表。空数组 / 非数组 = 不记，`tools_count` 也不出现。
    void set_tools(const nlohmann::ordered_json& tools);

    // ---- 收回来之后填 ----

    /// 每一次拿到 HTTP 响应就调一次（成功的、400 的、都调）。
    /// 记下状态码、留住原始回包备 `error.txt` 用、并从回包里抽 `usage`。
    /// 抽不到 `usage` 就不记，不抛。
    void note_response(int http_status, const std::string& body);

    void set_finish_reason(std::string reason);

    /// 模型吐回来的原始正文。**流式那条在收完之后一次性交**。
    ///
    /// ⚠️ 交进来的时机要在那几条 `throw` **之前**：「schema 本地校验没过」和
    /// 「服务在流里报错，而前面已经吐过半份正文」正是最想研究的两种，正文不
    /// 留下来就白记了。
    void set_reply(std::string reply);

    void set_thinking(std::string thinking);

    /// 流式那条专用：思考是一段一段来的，接起来。
    /// **只在内存里拼，一个字节都不落盘**——这一句跑在收流线程上，每来一小段
    /// 就一次，在这儿开文件等于把生成拖慢。落盘只在析构那一下。
    void append_thinking(const std::string& piece);

    /// 流式那条专用：第一段**正文**到的那一下。第二次起是空操作。
    /// 里面只有一次 `steady_clock::now()` 和一个 bool，没有 I/O。
    void mark_first_token();

    /// 砸了。`message` 是**翻给用户的那句**（`LlmError::what()`），
    /// `http_status` 是 `LlmError::status()`（不是 HTTP 那条路就传 0）。
    /// 调完照旧 `throw;`——**异常原样往外走，一个字都不改**。
    ///
    /// 传 0 时**不会**盖掉 `note_response` 已经记下的状态码：对面好好地回了
    /// 200、是我们这头按 schema 校验没过的那一种，账上要同时看得见
    /// `http_status=200` 和 `ok=false`。
    void fail(const std::string& message, int http_status);

    /// 这一次的 id（形如 `20260918-174233-518-0007`）。给用例看的；
    /// 生产代码用不上。没开记录时是空串。
    const std::string& id() const;

    /// 这会儿手上攒了多少**字**的思考（析构时落进 `thinking_chars` 那一列的
    /// 就是它）。给用例看的：关掉记录之后这儿必须是 0——出入口照旧会在收流
    /// 回调里一段段交进来，而「不记就整个不动手」意味着原地扔掉。
    std::size_t thinking_chars() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 把一段 OpenAI 格式的 `messages` 渲染成人能读的一份。
///
/// **只有一条、role 是 user、没有 tool_calls 时原样返回它的 content**
/// ——`complete` 那两条走的就是这一支，出来的正是模型收到的那段字，不多一个
/// 抬头，能直接和另一次的 `diff`。
///
/// 别的情况一条一个抬头，块与块之间空一行：
///
///     === system ===
///     你是……
///
///     === user ===
///     从网上找找今天有什么热点
///
///     === assistant (tool_calls: hot_topics, read_page) ===
///     → hot_topics {"n":10}
///
///     === tool (call_1) ===
///     {"items":[…]}
///
/// `tool_calls` 的参数原样跟在那一块的 content 后面，每个工具一行
/// （`→ 工具名 参数`）——它是 JSON 文本，不是模型说的话，混进正文里会让
/// 「模型这一轮说了什么」不再准。content 为空就只有抬头和这几行，
/// 抬头已经说清这一条在干什么。
std::string render_prompt(const nlohmann::ordered_json& messages);

/// 从一份回包里抽 `usage`。抽不到回 `nullptr`（json 的 null），不抛。
/// 只收 prompt_tokens / completion_tokens / total_tokens 三个整数键，
/// 有哪个收哪个。
nlohmann::json extract_usage(const std::string& raw_body);

}  // namespace changji::llm
