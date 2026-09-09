#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "pipeline/jobs.hpp"

namespace changji::infer {

/// 有没有把进程内文本大模型编进来（`CHANGJI_LLAMA=ON`）。
bool llama_chat_available();

/// 进程内跑文本大模型。写剧本、出分镜用它。
///
/// **存在的理由是"一个程序跑所有"。** 以前 LLM 是外面一个 llama-server，
/// 于是有两个进程要起、两份显存要算，而且**调度器管不着它**——
/// 出片要显存时没法让它先让开，只能靠人去把那个服务停掉。
/// 做成进程内的槽之后，它和出图出片一样归调度器管：平时常驻，
/// 显存真不够时被驱逐，够就不动（见 Scheduler::set_free_vram_probe）。
///
/// **输出按 JSON Schema 约束。** 流水线是无人值守跑的，一次解析失败
/// 就是一集卡住，而模型下一次未必吐得更好。约束走 llama.cpp 的语法采样：
/// schema 先转成 GBNF（common 里的 `json_schema_to_grammar`），
/// 再挂到采样链上。**这也是我们要链 llama-common 的唯一理由。**
class LlamaChat {
public:
    /// 载模型。失败回 nullptr 并把原因写进 `why`。
    ///
    /// `n_gpu_layers` 传 0 就是纯 CPU 跑。显存紧张时调用方可以这么退。
    static std::unique_ptr<LlamaChat> load(const std::filesystem::path& model,
                                           bool use_gpu, std::string& why);

    ~LlamaChat();
    LlamaChat(const LlamaChat&) = delete;
    LlamaChat& operator=(const LlamaChat&) = delete;

    /// 跑一次补全。
    ///
    /// `schema` 是 JSON Schema 的文本（空表示不约束）。
    /// `tok` 被取消时提前收工，返回已经生成的部分并把 `cancelled` 置位。
    /// 失败返回 false 并填 `why`。
    bool complete(const std::string& prompt, const std::string& schema,
                  double temperature, int max_tokens, pipeline::CancelToken& tok,
                  std::string& out, std::string& why);

    /// 这个模型的上下文长度。提示词超了要先知道，别等它自己截断。
    int context_tokens() const;

private:
    LlamaChat();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace changji::infer
