#include "infer/llama_chat.hpp"

#include <string>
#include <vector>

#include "util/paths.hpp"

#ifdef CHANGJI_HAVE_LLAMA
#include "json-schema-to-grammar.h"
#include "llama.h"
#endif

namespace fs = std::filesystem;

namespace changji::infer {

#ifdef CHANGJI_HAVE_LLAMA

bool llama_chat_available() { return true; }

namespace {

/// llama.cpp 的后端只该初始化一次。配音那边也有一份同样的哨兵——
/// 两处各自守着自己，重复调 llama_backend_init 是未定义行为。
struct BackendGuard {
    BackendGuard() { llama_backend_init(); }
    ~BackendGuard() { llama_backend_free(); }
};
void ensure_backend() { static BackendGuard g; }

}  // namespace

struct LlamaChat::Impl {
    llama_model* model = nullptr;
    llama_context* lctx = nullptr;
    int n_ctx = 0;

    ~Impl() {
        if (lctx != nullptr) llama_free(lctx);
        if (model != nullptr) llama_model_free(model);
    }
};

LlamaChat::LlamaChat() : impl_(std::make_unique<Impl>()) {}
LlamaChat::~LlamaChat() = default;

std::unique_ptr<LlamaChat> LlamaChat::load(const fs::path& model, bool use_gpu,
                                           std::string& why) {
    if (model.empty()) {
        why = "[models].llm 没填。进程内跑大模型要一份 GGUF 权重";
        return nullptr;
    }
    std::error_code ec;
    if (!fs::is_regular_file(model, ec)) {
        why = "大模型权重不在：" + paths::to_utf8(model);
        return nullptr;
    }
    ensure_backend();

    std::unique_ptr<LlamaChat> self(new LlamaChat());
    Impl& im = *self->impl_;

    llama_model_params mp = llama_model_default_params();
    // 999 = 全放显卡；放不下时 llama.cpp 自己会把剩下的留在内存。
    // 调用方传 false 就是纯 CPU——显存被出图出片占满时的退路。
    mp.n_gpu_layers = use_gpu ? 999 : 0;
    im.model = llama_model_load_from_file(paths::to_utf8(model).c_str(), mp);
    if (im.model == nullptr) {
        why = "大模型载不起来：" + paths::to_utf8(model);
        return nullptr;
    }

    llama_context_params cp = llama_context_default_params();
    // **n_ctx = 0 表示"用模型训练时的长度"**，而默认值是 512。
    // 写剧本的提示词轻松过千 token，512 会被静默截断——症状是模型
    // 答非所问，指不到"上下文开小了"。配音那边栽过同一条，见 llama_tts.cpp。
    cp.n_ctx = 0;
    im.lctx = llama_init_from_model(im.model, cp);
    if (im.lctx == nullptr) {
        why = "建不出 llama context";
        return nullptr;
    }
    im.n_ctx = static_cast<int>(llama_n_ctx(im.lctx));
    return self;
}

int LlamaChat::context_tokens() const { return impl_->n_ctx; }

bool LlamaChat::complete(const std::string& prompt, const std::string& schema,
                         double temperature, int max_tokens,
                         pipeline::CancelToken& tok, std::string& out,
                         std::string& why) {
    out.clear();
    Impl& im = *impl_;
    const llama_vocab* vocab = llama_model_get_vocab(im.model);

    // ---- 提示词切词 ----
    const int n_prompt = -llama_tokenize(vocab, prompt.c_str(),
                                         static_cast<int32_t>(prompt.size()),
                                         nullptr, 0, true, true);
    if (n_prompt <= 0) {
        why = "提示词切不出 token";
        return false;
    }
    std::vector<llama_token> toks(static_cast<std::size_t>(n_prompt));
    if (llama_tokenize(vocab, prompt.c_str(),
                       static_cast<int32_t>(prompt.size()), toks.data(),
                       n_prompt, true, true) < 0) {
        why = "提示词切词失败";
        return false;
    }
    // **先查长度再跑。** 超了的话 llama.cpp 会截断而不是报错，
    // 出来的东西看着像模型没听懂，其实是提示词根本没喂全。
    if (n_prompt + max_tokens > im.n_ctx) {
        why = "提示词太长：" + std::to_string(n_prompt) + " 个 token 加上要生成的 " +
              std::to_string(max_tokens) + " 个，超过这个模型的上下文 " +
              std::to_string(im.n_ctx) +
              "。换一个上下文更长的模型，或者把这一步拆小。";
        return false;
    }

    // ---- 采样链 ----
    //
    // **语法采样要放在最前面。** 它是硬约束（把不合法的 token 概率清零），
    // 温度和 top-p 是在剩下的分布里挑——顺序反了的话先按温度挑完再被语法
    // 否掉，采样会退化甚至死循环。
    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    llama_sampler* chain = llama_sampler_chain_init(sp);
    struct ChainGuard {
        llama_sampler* p;
        ~ChainGuard() { llama_sampler_free(p); }
    } guard{chain};

    if (!schema.empty()) {
        std::string gbnf;
        try {
            // 用 llama 自己那份 json（common_json），不是我们仓库里的
            // nlohmann——两个类型不通用，混着传编不过。
            gbnf = json_schema_to_grammar(common_json::parse(schema), true);
        } catch (const std::exception& e) {
            why = std::string("这个 JSON Schema 转不成语法：") + e.what();
            return false;
        }
        llama_sampler* g =
            llama_sampler_init_grammar(vocab, gbnf.c_str(), "root");
        if (g == nullptr) {
            why = "语法采样器建不出来。schema 转出来的 GBNF 可能不合法";
            return false;
        }
        llama_sampler_chain_add(chain, g);
    }
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(
        chain, llama_sampler_init_temp(static_cast<float>(temperature)));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    // ---- 跑 ----
    llama_memory_clear(llama_get_memory(im.lctx), true);
    llama_batch batch = llama_batch_get_one(toks.data(), n_prompt);

    for (int produced = 0; produced < max_tokens; ++produced) {
        // 取消在**每个 token 之间**查一次。写一集剧本要几分钟，
        // 不查的话点了停止要等它自己写完。
        if (tok.cancelled()) return true;

        if (llama_decode(im.lctx, batch) != 0) {
            why = "llama_decode 失败（第 " + std::to_string(produced) + " 个 token）";
            return false;
        }
        // **别再 accept 一次。** `llama_sampler_sample` 内部已经调过
        // `llama_sampler_accept`（llama-sampler.cpp 两条返回路径上都有）。
        // 再手动接一次的话语法状态被推进两遍，第一个 token 就炸：
        //   Unexpected empty grammar stack after accepting piece: \n (515)
        // 而报错指向"语法/schema 有问题"，和真因隔着好几层。
        // 不是 const：llama_batch_get_one 要非 const 指针（它不会改，
        // 但接口没标 const）。
        llama_token id = llama_sampler_sample(chain, im.lctx, -1);
        if (llama_vocab_is_eog(vocab, id)) break;

        char buf[256];
        const int n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        if (n < 0) {
            why = "token 转不回文字";
            return false;
        }
        out.append(buf, static_cast<std::size_t>(n));
        batch = llama_batch_get_one(&id, 1);
    }
    return true;
}

#else

bool llama_chat_available() { return false; }

struct LlamaChat::Impl {};
LlamaChat::LlamaChat() : impl_(std::make_unique<Impl>()) {}
LlamaChat::~LlamaChat() = default;

std::unique_ptr<LlamaChat> LlamaChat::load(const fs::path&, bool,
                                           std::string& why) {
    why = "这个二进制没编进程内大模型（构建时 CHANGJI_LLAMA=OFF）。"
          "用 [llm].base_url 指向一个兼容 OpenAI 接口的服务";
    return nullptr;
}
int LlamaChat::context_tokens() const { return 0; }
bool LlamaChat::complete(const std::string&, const std::string&, double, int,
                         pipeline::CancelToken&, std::string&,
                         std::string& why) {
    why = "这个二进制没编进程内大模型";
    return false;
}

#endif

}  // namespace changji::infer
