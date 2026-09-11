#include "infer/llama_chat.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
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

/// 把提示词套进模型自带的对话模板。
///
/// **不套模板等于把 instruct 模型当补全模型用。** Qwen3 这种指令模型训练时
/// 每一轮都裹着 `<|im_start|>role ... <|im_end|>`，裸喂一段中文它只是在
/// "续写"，时好时坏——实测症状是把 JSON Schema 里的字段说明原样当内容吐
/// 回来（title 填成"标题"），或者整串 XXXX。挂了语法采样也救不回来，
/// 因为语法只管形状不管内容。
///
/// 模板从 GGUF 元数据里取。llama_chat_apply_template **不跑 jinja**，它按
/// 特征串认一批内置模板（Qwen 走 chatml 那条）；认不出来返回负数，那就
/// 退回裸提示词——比硬套一个错模板强。
std::string apply_chat_template(llama_model* model, const std::string& user) {
    const char* tmpl = llama_model_chat_template(model, nullptr);
    if (tmpl == nullptr) return user;

    const llama_chat_message msg{"user", user.c_str()};
    // 文档建议的大小是所有消息字符数的两倍，再加一截给模板自己的标记。
    std::vector<char> buf(user.size() * 2 + 1024);
    int32_t n = llama_chat_apply_template(tmpl, &msg, 1, /*add_ass=*/true,
                                          buf.data(),
                                          static_cast<int32_t>(buf.size()));
    if (n > static_cast<int32_t>(buf.size())) {
        buf.resize(static_cast<std::size_t>(n) + 1);
        n = llama_chat_apply_template(tmpl, &msg, 1, /*add_ass=*/true,
                                      buf.data(),
                                      static_cast<int32_t>(buf.size()));
    }
    if (n <= 0) return user;
    std::string out(buf.data(), static_cast<std::size_t>(n));

    // **Qwen3 默认开思考。** 模板里出现 enable_thinking 就是这一族。开着的话
    // 助手那一轮从 `<think>` 起头，而我们挂了 JSON 语法——语法把 `<think>`
    // 直接判非法，模型第一个 token 就被逼进死角。Qwen 官方的关法就是替它把
    // 思考段写成空的，和模板在 enable_thinking=false 时吐的一模一样。
    if (std::string(tmpl).find("enable_thinking") != std::string::npos) {
        out += "<think>\n\n</think>\n\n";
    }
    return out;
}

}  // namespace

struct LlamaChat::Impl {
    llama_model* model = nullptr;
    /// 同一份权重上的几个上下文。**权重共用，KV cache 各自一份。**
    ///
    /// 为什么不是一个上下文跑多路：llama.cpp 的单 context 不支持并发
    /// decode，两路同时进去会把进程带走（2026-09-11 端到端实跑时撞过一次，
    /// 日志停在一个请求上，没有任何错误，进程直接没了）。
    std::vector<llama_context*> ctxs;
    int n_ctx = 0;

    // 空闲上下文的取还。池空了就等——**等，不是失败**：同时编两个项目时，
    // 第二个人宁可多等十几秒，也不该看见一句"忙，稍后再试"。
    std::mutex mu;
    std::condition_variable cv;
    std::vector<llama_context*> idle;

    ~Impl() {
        for (llama_context* c : ctxs) {
            if (c != nullptr) llama_free(c);
        }
        if (model != nullptr) llama_model_free(model);
    }

    /// 借一个上下文，出作用域自动还回去。
    ///
    /// 用 RAII 而不是手动还：中间任何一条 return 或者抛异常忘了还，那个槽
    /// 就永久少一个，表现是并发度悄悄降到 0 然后全部卡死——而那时候没有
    /// 任何报错。放在 Impl 里面是因为 Impl 是 LlamaChat 的私有嵌套类型，
    /// 外面的类写不出它的名字。
    class Lease {
    public:
        Lease(Impl& im, pipeline::CancelToken& tok) : im_(im) {
            std::unique_lock lk(im_.mu);
            // 每 200ms 醒一次查取消：排在前面那一路可能要跑几十秒，
            // 用户按了停，界面上得真的停下来。
            while (im_.idle.empty()) {
                if (tok.cancelled()) return;
                im_.cv.wait_for(lk, std::chrono::milliseconds(200));
            }
            ctx_ = im_.idle.back();
            im_.idle.pop_back();
        }

        ~Lease() {
            if (ctx_ == nullptr) return;
            {
                std::lock_guard lg(im_.mu);
                im_.idle.push_back(ctx_);
            }
            im_.cv.notify_one();
        }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        llama_context* get() const { return ctx_; }

    private:
        Impl& im_;
        llama_context* ctx_ = nullptr;
    };
};



LlamaChat::LlamaChat() : impl_(std::make_unique<Impl>()) {}
LlamaChat::~LlamaChat() = default;

std::unique_ptr<LlamaChat> LlamaChat::load(const fs::path& model, bool use_gpu,
                                           std::string& why, int parallel) {
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

    const int want = parallel < 1 ? 1 : parallel;
    for (int i = 0; i < want; ++i) {
        llama_context* c = llama_init_from_model(im.model, cp);
        if (c == nullptr) {
            // **第一个开不出来才算失败。** 后面的开不出来只说明显存只够这么
            // 多路——那正是"显存不够就排队"该有的样子，不是错误。
            if (i == 0) {
                why = "建不出 llama context";
                return nullptr;
            }
            std::fprintf(stderr,
                         "[llm] 只开出 %d 个上下文（要 %d 个）：显存不够，"
                         "同时跑的路数按这个来
",
                         i, want);
            break;
        }
        im.ctxs.push_back(c);
    }
    im.idle = im.ctxs;
    im.n_ctx = static_cast<int>(llama_n_ctx(im.ctxs.front()));
    return self;
}

int LlamaChat::context_tokens() const { return impl_->n_ctx; }

int LlamaChat::slots() const { return static_cast<int>(impl_->ctxs.size()); }

bool LlamaChat::complete(const std::string& prompt, const std::string& schema,
                         double temperature, int max_tokens,
                         pipeline::CancelToken& tok, std::string& out,
                         std::string& why) {
    out.clear();
    Impl& im = *impl_;
    const llama_vocab* vocab = llama_model_get_vocab(im.model);

    // ---- 提示词切词 ----
    // 先套对话模板再切词。parse_special 必须是 true，否则 `<|im_start|>`
    // 会被当成普通文字切碎，模板等于白套。
    const std::string templated = apply_chat_template(im.model, prompt);

    const int n_prompt = -llama_tokenize(vocab, templated.c_str(),
                                         static_cast<int32_t>(templated.size()),
                                         nullptr, 0, true, true);
    if (n_prompt <= 0) {
        why = "提示词切不出 token";
        return false;
    }
    std::vector<llama_token> toks(static_cast<std::size_t>(n_prompt));
    if (llama_tokenize(vocab, templated.c_str(),
                       static_cast<int32_t>(templated.size()), toks.data(),
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
    //
    // 借一个空闲上下文。**同一个上下文同一时刻只有一路**——llama.cpp 的
    // 单 context 不支持并发 decode。池里有空的就直接开跑（真并发），
    // 全忙着就在这儿等（排队）。
    Impl::Lease lease(im, tok);
    if (lease.get() == nullptr) {
        why = "已取消";
        return false;
    }
    llama_context* lctx = lease.get();

    llama_memory_clear(llama_get_memory(lctx), true);
    llama_batch batch = llama_batch_get_one(toks.data(), n_prompt);

    for (int produced = 0; produced < max_tokens; ++produced) {
        // 取消在**每个 token 之间**查一次。写一集剧本要几分钟，
        // 不查的话点了停止要等它自己写完。
        if (tok.cancelled()) return true;

        if (llama_decode(lctx, batch) != 0) {
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
        llama_token id = llama_sampler_sample(chain, lctx, -1);
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
                                           std::string& why, int) {
    why = "这个二进制没编进程内大模型（构建时 CHANGJI_LLAMA=OFF）。"
          "用 [llm].base_url 指向一个兼容 OpenAI 接口的服务";
    return nullptr;
}
int LlamaChat::context_tokens() const { return 0; }
int LlamaChat::slots() const { return 0; }
bool LlamaChat::complete(const std::string&, const std::string&, double, int,
                         pipeline::CancelToken&, std::string&,
                         std::string& why) {
    why = "这个二进制没编进程内大模型";
    return false;
}

#endif

}  // namespace changji::infer
