#include "llm/local_client.hpp"

#include "config/runtime.hpp"
#include <cstdio>
#include <filesystem>
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
    // 假设。真装得下的时候由下面那个老实数救回来（不会白卸）。
    const double budget = profile.vram_gb > 0 ? profile.vram_gb * 0.9 : 0.0;
    spec.vram_estimate = static_cast<std::size_t>(budget * 1024) * 1024 * 1024;
    // **老实数：真正要占的显存。** 只在问到了卡上空闲显存时才拿来比。
    // 不给的话这条"够就不卸"是单向的——出片时保住了大模型，回头写剧本
    // 借 LLM 槽走的还是整份预算，反过来把图像模型卸掉，两边来回踢。
    {
        const config::Settings s = provider();
        std::error_code ec;
        const auto p = s.models.resolve(s.models.llm, s.workspace_path());
        const auto bytes = p.empty() ? 0 : std::filesystem::file_size(p, ec);
        const double model_gb = (!ec && bytes > 0)
                                    ? static_cast<double>(bytes) / (1024.0 * 1024 * 1024)
                                    : 0.0;
        const double live = s.models.llm_live_vram_gb(model_gb);
        spec.live_vram_estimate =
            live > 0 ? static_cast<std::size_t>(live * 1024) * 1024 * 1024 : 0;
    }
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
