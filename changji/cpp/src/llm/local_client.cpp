#include "llm/local_client.hpp"

#include "config/runtime.hpp"
#include <cstdio>
#include <mutex>

#include "infer/llama_chat.hpp"
#include "infer/scheduler.hpp"

namespace changji::llm {

namespace {

std::mutex g_mu;
std::shared_ptr<infer::LlamaChat> g_chat;

std::shared_ptr<infer::LlamaChat> current() {
    std::lock_guard lg(g_mu);
    return g_chat;
}

}  // namespace

std::string LocalClient::complete(const Request& req,
                                  pipeline::CancelToken& tok) {
    if (tok.cancelled()) throw LlmError("已取消");

    // 借槽。**调度器可能在这一步把出图或出片的模型卸掉腾地方**，
    // 也可能什么都不做（实时空闲显存够的时候）——见
    // Scheduler::set_free_vram_probe。借不到时抛的是那条带出路的消息。
    auto lease = infer::scheduler().acquire(infer::Slot::LLM);
    auto chat = current();
    if (!chat) throw LlmError("大模型没准备好（槽借到了但上下文是空的）");

    // schema 原样传下去：LlamaChat 会把它转成 GBNF 挂到采样链上。
    // **这是 local 和 remote 唯一在语义上不一样的地方**——远端靠服务端的
    // response_format，本地靠语法采样，两条路都是硬约束，出来的都是合法 JSON。
    const std::string schema =
        req.schema.is_null() || req.schema.empty() ? "" : req.schema.dump();

    std::string out;
    std::string why;
    // 上限给得宽：写一集剧本本来就长。真正的护栏是上下文长度，
    // LlamaChat 里会先查提示词加这个数超没超。
    constexpr int kMaxTokens = 8192;
    if (!chat->complete(req.prompt, schema, req.temperature, kMaxTokens, tok,
                        out, why)) {
        throw LlmError("进程内大模型失败：" + why);
    }
    if (tok.cancelled()) throw LlmError("已取消");
    return out;
}

std::shared_ptr<Client> make_client(HttpPost post) {
    const config::Settings s = config::runtime().snapshot();
    if (s.llm.backend == "local") {
        if (infer::llama_chat_available()) {
            return std::make_shared<LocalClient>();
        }
        std::fputs(
            "[llm] 配的是 backend = \"local\"，但这个二进制没编进程内"
            "大模型（构建时 CHANGJI_LLAMA=OFF）。退回 [llm].base_url 那条。\n",
            stderr);
    }
    return std::make_shared<RemoteClient>(
        ConfigProvider([] { return config::runtime().snapshot().llm; }),
        std::move(post));
}

void register_llm_slot(std::function<config::Settings()> provider,
                       const models::HardwareProfile& profile) {
    if (provider().llm.backend != "local") return;

    infer::SlotSpec spec;
    spec.slot = infer::Slot::LLM;
    // 常驻：用户要的"默认加载 llm"。显存真不够时才被驱逐。
    spec.residency = infer::Residency::Cached;
    // 估值按整份预算算，和出图出片一致——"同时只装得下一个"是保守但安全的
    // 假设，而真装得下的时候实时显存那条会救回来（不会白卸）。
    const double budget = profile.vram_gb > 0 ? profile.vram_gb * 0.9 : 0.0;
    spec.vram_estimate = static_cast<std::size_t>(budget * 1024) * 1024 * 1024;
    // **优先级最低，腾地方时先卸它。** 写剧本一集只跑一次，
    // 出图出片每镜都要——重装大模型的代价摊在一集上，比每镜重装小得多。
    spec.evict_priority = 1;
    spec.load = [provider] {
        const config::Settings s = provider();
        const auto path = s.models.resolve(s.models.llm, s.workspace_path());
        std::string why;
        auto chat = std::shared_ptr<infer::LlamaChat>(
            infer::LlamaChat::load(path, /*use_gpu=*/true, why));
        if (!chat) throw std::runtime_error("大模型载不起来：" + why);
        std::lock_guard lg(g_mu);
        g_chat = std::move(chat);
    };
    spec.unload = [] {
        std::lock_guard lg(g_mu);
        g_chat.reset();
    };
    infer::scheduler().register_slot(std::move(spec));
}

}  // namespace changji::llm
