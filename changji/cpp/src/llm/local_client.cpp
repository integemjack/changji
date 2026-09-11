#include "llm/local_client.hpp"

#include "config/runtime.hpp"
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

#include "infer/llama_chat.hpp"
#include "infer/scheduler.hpp"
#include "models/hardware.hpp"

namespace changji::llm {

namespace {

std::mutex g_mu;
std::shared_ptr<infer::LlamaChat> g_chat;

std::shared_ptr<infer::LlamaChat> current() {
    std::lock_guard lg(g_mu);
    return g_chat;
}

}  // namespace

LocalLlmStatus local_llm_status() {
    LocalLlmStatus st;
    auto chat = current();
    if (!chat) return st;
    st.loaded = true;
    st.slots = chat->slots();
    st.context_tokens = chat->context_tokens();
    return st;
}

std::string LocalClient::complete(const Request& req,
                                  pipeline::CancelToken& tok) {
    if (tok.cancelled()) throw LlmError("已取消");

    // **并发在 LlamaChat 里管**：同一份权重上开了几个上下文，几路就能同时
    // 跑，池满了才在那儿等（见 LlamaChat::load 和 Impl::Lease）。
    //
    // 这里不能再加一把全局互斥——加了就等于把并发按回 1，同时编两个项目时
    // 第二个人干等十几秒。而这一层看不到显存够开几个上下文，那个判断只有
    // LlamaChat 做得了。
    //
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
    // 常驻：装上之后就不主动卸，显存真不够时才被驱逐。
    //
    // **注意 Cached 不等于"启动就装"**：调度器是借出时才加载的
    // （见 Scheduler::acquire）。用户要的"默认加载 llm"靠的是
    // warm_llm_in_background，不是这一行。这条注释以前写成
    // "常驻：用户要的默认加载 llm"，把两件事混成一件了。
    spec.residency = infer::Residency::Cached;
    // 估值按整份预算算，和出图出片一致——"同时只装得下一个"是保守但安全的
    // 假设。真装得下的时候由下面那个老实数救回来（不会白卸）。
    const double budget = profile.vram_gb > 0 ? profile.vram_gb * 0.9 : 0.0;
    spec.vram_estimate = static_cast<std::size_t>(budget * 1024) * 1024 * 1024;
    // **老实数：真正要占的显存。** 只在问到了卡上空闲显存时才拿来比。
    // 不给的话这条"够就不卸"是单向的——出片时保住了大模型，回头写剧本
    // 借 LLM 槽走的还是整份预算，反过来把图像模型卸掉，两边来回踢。
    // 每次借槽时现算，不存定值：大模型也能在初始化页换掉，
    // 而槽一个进程只注册一次。见 SlotSpec::live_vram。
    spec.live_vram = [provider]() -> std::size_t {
        const config::Settings s = provider();
        std::error_code ec;
        const auto p = s.models.resolve(s.models.llm, s.workspace_path());
        const auto bytes = p.empty() ? 0 : std::filesystem::file_size(p, ec);
        const double model_gb = (!ec && bytes > 0)
                                    ? static_cast<double>(bytes) / (1024.0 * 1024 * 1024)
                                    : 0.0;
        const double live = s.models.llm_live_vram_gb(model_gb);
        return live > 0 ? static_cast<std::size_t>(live * 1024) * 1024 * 1024 : 0;
    };
    // **优先级最低，腾地方时先卸它。** 写剧本一集只跑一次，
    // 出图出片每镜都要——重装大模型的代价摊在一集上，比每镜重装小得多。
    spec.evict_priority = 1;
    spec.load = [provider] {
        const config::Settings s = provider();
        const auto path = s.models.resolve(s.models.llm, s.workspace_path());
        // **装之前先记一眼显存。** 装完再记一次，差值就是这份权重实际
        // 占了多少——比"总量减空闲"准得多，那个会把别的槽的账也算进来。
        // 加载这一步本来就要几秒，多问一次无所谓（NVML 那条其实是微秒级）。
        //
        // **只在没有别的槽同时在装的时候才认这个差值。**
        // acquire 是标完 is_loaded 就放锁、再去调 load 的，所以两个槽
        // 完全可能同时在装（出首帧是几镜并发的，配音也可能同时起）。
        // 那时候这段窗口里的显存变化里混着别人的账，差值会偏大——
        // 而它是只往上记的高水位，记错一次就一直错下去，此后每次判
        // "还剩多少空闲"都把大模型算得比实际大，白卸别的模型。
        //
        // 开机默认装大模型那次通常正好是独占（别的都还没装），够用了。
        const auto alone_now = [] {
            const auto v = infer::scheduler().loaded_slots();
            return v.size() == 1 && v.front() == infer::Slot::LLM;
        };
        const bool alone_before = alone_now();
        const auto before = models::free_vram_gb();
        std::string why;
        auto chat = std::shared_ptr<infer::LlamaChat>(
            infer::LlamaChat::load(path, /*use_gpu=*/true, why, s.llm.parallel));
        if (!chat) {
            // **上不了 GPU 就退回 CPU，别整个失败。**
            //
            // use_gpu 传下去是 n_gpu_layers = 999，也就是"所有层都放显存"。
            // 这张卡装得下就最好，装不下 llama.cpp 直接返回失败——而
            // "大模型载不起来"对用户等于整条流水线没了，其实放内存跑就行，
            // 只是慢。
            //
            // **不预先估一个阈值来决定放不放。** 今天已经证过估算会错到
            // 五倍（见 Scheduler::record_measured_vram），拿它去卡这一步，
            // 会在本来放得下的机器上白白降到 CPU。试一次、不行再退，
            // 结果由这台机器自己说了算，换机器不用改任何东西。
            std::fprintf(stderr,
                         "[llm] 权重上不了显存（%s），退回内存跑。慢一些，"
                         "但不影响出片。\n",
                         why.c_str());
            std::string why_cpu;
            chat = std::shared_ptr<infer::LlamaChat>(
                infer::LlamaChat::load(path, /*use_gpu=*/false, why_cpu, s.llm.parallel));
            if (!chat) {
                throw std::runtime_error("大模型载不起来：显存那次是「" + why +
                                         "」，内存那次是「" + why_cpu + "」");
            }
        }
        {
            const auto after = models::free_vram_gb();
            if (alone_before && alone_now() && before.has_value() &&
                after.has_value() && *before > *after) {
                const double used_gb = *before - *after;
                infer::scheduler().record_measured_vram(
                    infer::Slot::LLM,
                    static_cast<std::size_t>(used_gb * 1024) * 1024 * 1024);
            }
        }
        std::lock_guard lg(g_mu);
        g_chat = std::move(chat);
    };
    spec.unload = [] {
        std::lock_guard lg(g_mu);
        g_chat.reset();
    };
    infer::scheduler().register_slot(std::move(spec));
}

std::thread warm_llm_in_background(
    const std::function<config::Settings()>& provider) {
    const config::Settings s = provider();
    // 外接 API 那条没有本地权重，没什么可预热的。
    if (s.llm.backend != "local") return {};
    // 没配模型就别装了：装不上会在 stderr 上留一条吓人的错，而"还没配模型"
    // 是全新安装的正常状态，体检那一项已经在说了。
    const auto path = s.models.resolve(s.models.llm, s.workspace_path());
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) return {};

    return std::thread([] {
        try {
            // 借一下就放。Residency::Cached 会让它留在显存里。
            auto lease = infer::scheduler().acquire(infer::Slot::LLM);
        } catch (const std::exception& e) {
            // **预热失败不该影响起服务。** 用户可能只是想看看分镜表，
            // 而大模型装不上的原因（文件坏了、显存不够）体检里都能查到。
            //
            // 没编进 llama 的构建上这一条**注定**失败，而 make_client 起来
            // 时已经说过一次"这个二进制没编进程内大模型"了。再来一句措辞
            // 不同的错，只会让人以为是两个毛病。
            if (infer::llama_chat_available()) {
                std::fprintf(stderr, "[llm] 预热没成功：%s\n", e.what());
            }
        } catch (...) {
            std::fputs("[llm] 预热没成功（未知异常）\n", stderr);
        }
    });
}

}  // namespace changji::llm
