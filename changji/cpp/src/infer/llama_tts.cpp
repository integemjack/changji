#include "infer/llama_tts.hpp"

#include <memory>

#ifdef CHANGJI_HAVE_LLAMA
#include <cstdio>

#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

#include "util/paths.hpp"
#endif

namespace changji::infer {

#ifdef CHANGJI_HAVE_LLAMA

namespace {

/// llama.cpp 的后端只需要初始化一次，而且**不在这里 free**。
///
/// `llama_backend_free()` 是全局的。配音后端被销毁时调它，会把
/// 同一个进程里 sd.cpp 那边正在用的 ggml 后端一起拆掉——两个库共用
/// 同一份 ggml，这不是理论风险。进程退出时操作系统会收，够了。
void ensure_backend() {
    static const bool once = [] {
        llama_backend_init();
        return true;
    }();
    (void)once;
}

}  // namespace

bool llama_tts_available() { return true; }

LlamaTtsProbe probe_llama_tts() {
    LlamaTtsProbe out;

    // 两个纯函数，没有副作用，也不碰模型文件。
    // 叫得动就说明 mtmd 真在这个二进制里。
    const mtmd_context_params p = mtmd_context_params_default();
    const char* marker = mtmd_default_marker();

    if (marker == nullptr) {
        out.detail = "mtmd 链进来了，但 mtmd_default_marker() 返回空";
        return out;
    }
    out.ok = true;
    out.detail = std::string("mtmd 已链入，媒体标记 ") + marker + "，默认 " +
                 std::to_string(p.n_threads) + " 线程";
    return out;
}

struct LlamaTts::Impl {
    llama_model* model = nullptr;
    llama_context* lctx = nullptr;
    mtmd_context* mctx = nullptr;
    llama_sampler* smpl = nullptr;
    int sample_rate = 0;

    ~Impl() {
        if (smpl != nullptr) llama_sampler_free(smpl);
        if (mctx != nullptr) mtmd_free(mctx);
        if (lctx != nullptr) llama_free(lctx);
        if (model != nullptr) llama_model_free(model);
        // 有意不调 llama_backend_free()，见 ensure_backend 上面那段。
    }
};

LlamaTts::LlamaTts() : impl_(std::make_unique<Impl>()) {}
LlamaTts::~LlamaTts() = default;

int LlamaTts::sample_rate() const { return impl_->sample_rate; }

std::unique_ptr<LlamaTts> LlamaTts::load(const std::filesystem::path& backbone,
                                         const std::filesystem::path& mmproj,
                                         bool use_gpu, std::string& why) {
    ensure_backend();

    std::error_code ec;
    if (!std::filesystem::is_regular_file(backbone, ec)) {
        why = "找不到骨干模型：" + paths::to_utf8(backbone);
        return nullptr;
    }
    if (!std::filesystem::is_regular_file(mmproj, ec)) {
        why = "找不到解码器：" + paths::to_utf8(mmproj);
        return nullptr;
    }

    // unique_ptr 而不是裸 new：下面每一步都可能失败，
    // 而每一步之后都已经有句柄要收。
    std::unique_ptr<LlamaTts> self(new LlamaTts());
    Impl& im = *self->impl_;

    llama_model_params mp = llama_model_default_params();
    // 骨干模型只有 1.7B，全放显卡上；放不下时 llama.cpp 自己会退。
    mp.n_gpu_layers = use_gpu ? 999 : 0;
    im.model = llama_model_load_from_file(paths::to_utf8(backbone).c_str(), mp);
    if (im.model == nullptr) {
        why = "骨干模型载不起来：" + paths::to_utf8(backbone);
        return nullptr;
    }

    llama_context_params cp = llama_context_default_params();
    // **必须开 embeddings。** 生成的每一帧都要把骨干的隐状态喂给解码器，
    // 关着的话 llama_get_embeddings_ith 返回空，症状是第一帧就失败。
    cp.embeddings = true;

    // **n_ctx 必须自己设成 0。**
    //
    // `llama_context_default_params()` 给的是 **512**，而参考实现
    // （`tools/tts/tts.cpp` 走 common）用的 `common_params.n_ctx` 默认是
    // **0**，注释写着 "0 == context the model was trained with"。
    // llama.cpp 里 0 表示"用模型训练时的长度"（llama-context.cpp 那句
    // `params.n_ctx == 0 ? hparams.n_ctx_train : params.n_ctx`）。
    //
    // 512 是不够的：骨干是自回归跑的，提示词的 token 加上最多 512 帧
    // 都要占位置。一句长一点的台词就会撑爆，而症状是生成中途失败，
    // 指不到"上下文开小了"。
    cp.n_ctx = 0;

    // **要逐 token 的隐状态，所以不能池化。**
    //
    // 默认是 UNSPECIFIED，llama.cpp 会去看模型的 hparams，拿不到才退成
    // NONE。Qwen3 骨干上大概率就是 NONE，但这里依赖的是
    // `llama_get_embeddings_ith(ctx, -1)` 拿最后一个 token 的隐状态——
    // 一旦哪个模型的 hparams 带了池化类型，这条路就悄悄取到别的东西。
    // 显式写死，不赌默认值。
    cp.pooling_type = LLAMA_POOLING_TYPE_NONE;
    im.lctx = llama_init_from_model(im.model, cp);
    if (im.lctx == nullptr) {
        why = "建不出 llama context";
        return nullptr;
    }

    mtmd_context_params mtp = mtmd_context_params_default();
    mtp.use_gpu = use_gpu;
    im.mctx = mtmd_init_from_file(paths::to_utf8(mmproj).c_str(), im.model, mtp);
    if (im.mctx == nullptr) {
        why = "解码器载不起来：" + paths::to_utf8(mmproj);
        return nullptr;
    }

    const mtmd_gen_audio_info info = mtmd_gen_audio_get_info(im.mctx);
    if (info.type == MTMD_GEN_AUDIO_TYPE_NONE) {
        // 拿错文件了——比如把两份 GGUF 的位置对调。这句话要说得具体，
        // 否则用户看到的是"配音失败"，然后去查显卡。
        why = "这份 mmproj 不支持音频生成：" + paths::to_utf8(mmproj) +
              "。配音要的是 tokenizer/解码器那一份，不是骨干那一份。";
        return nullptr;
    }
    im.sample_rate = info.sample_rate;

    return self;
}

bool LlamaTts::synthesize(const LlamaTtsRequest& req, double& out_duration_s,
                          std::string& why) {
    Impl& im = *impl_;
    if (req.text.empty()) {
        why = "台词是空的";
        return false;
    }

    // 采样器每句重建：种子要按请求来，而链是不可变的。
    if (im.smpl != nullptr) llama_sampler_free(im.smpl);
    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    im.smpl = llama_sampler_chain_init(sp);
    llama_sampler_chain_add(im.smpl, llama_sampler_init_top_k(req.top_k));
    llama_sampler_chain_add(im.smpl, llama_sampler_init_top_p(req.top_p, 1));
    llama_sampler_chain_add(im.smpl, llama_sampler_init_dist(req.seed));

    mtmd::bitmap_ptr speaker;
    if (req.speaker_ref.has_value()) {
        auto wrapper = mtmd_helper_bitmap_init_from_file(
            im.mctx, paths::to_utf8(*req.speaker_ref).c_str(), false,
            mtmd_helper_init_opt_default());
        if (wrapper.bitmap == nullptr) {
            // **把认得的格式列出来。** mtmd 那边走 miniaudio，只认这三种
            // （mtmd-helper.h 里写着 "audio: formats supported by miniaudio:
            // wav, mp3, flac"）。拿一个 m4a 或者 ogg 过来是很常见的事，
            // 而只说"读不了"的话，用户会去查路径对不对、文件坏没坏。
            why = "参考音色读不了：" + paths::to_utf8(*req.speaker_ref) +
                  "（只认 wav / mp3 / flac）";
            return false;
        }
        speaker.reset(wrapper.bitmap);
    }

    mtmd_helper::gen_audio gen(im.lctx, im.mctx);
    mtmd_helper_gen_audio_inp inp{};
    inp.seq_id = 0;
    inp.prompt = req.text.c_str();
    inp.prompt_len = req.text.size();
    inp.speaker_ref = speaker.get();
    inp.lang = req.lang.empty() ? nullptr : req.lang.c_str();
    inp.top_k = req.top_k;
    inp.top_p = req.top_p;
    inp.seed = req.seed;
    inp.out_type = MTMD_HELPER_GEN_AUDIO_OUTTYPE_WAV;

    if (gen.set_input(&inp) != 0) {
        why = "喂不进去这句台词";
        return false;
    }

    // 第一阶段：把提示词过一遍骨干模型。
    for (;;) {
        const int32_t left = gen.step_prompt(512);
        if (left < 0) {
            why = "跑提示词时失败";
            return false;
        }
        if (left == 0) break;
    }

    const auto sample = [&] {
        const llama_token t = llama_sampler_sample(im.smpl, im.lctx, -1);
        llama_sampler_accept(im.smpl, t);
        return t;
    };

    // 第二、三阶段：语义码 → 声学细节 → 波形。
    // **循环由调用方驱动**——mtmd 那套 API 是无状态的，头文件里写着
    // "caller must handle state management and audio frame accumulation"。
    llama_token sampled = sample();
    const float* h_state = llama_get_embeddings_ith(im.lctx, -1);
    if (h_state == nullptr) {
        // 几乎一定是 context 没开 embeddings。上面开了，所以走到这儿
        // 说明上游改了行为——把话说明白，别让人去查模型文件。
        why = "拿不到骨干模型的隐状态（context 的 embeddings 没开？）";
        return false;
    }

    int frames = 0;
    bool stop = false;
    while (!stop && frames < req.max_frames) {
        const float* h_next = nullptr;
        if (gen.step_gen(sampled, h_state, &h_next, &stop) != 0) {
            why = "第 " + std::to_string(frames) + " 帧生成失败";
            return false;
        }
        if (h_next == nullptr) break;  // 停了，且这一帧没产出
        ++frames;
        h_state = h_next;
        sampled = sample();
    }
    if (frames == 0) {
        why = "一帧都没生成出来";
        return false;
    }

    int32_t rate = 0;
    const char* data = nullptr;
    size_t len = 0;
    std::int64_t samples = 0;
    if (gen.get_output(&rate, &data, &len, &samples) != 0) {
        why = "取不到合成结果";
        return false;
    }
    if (data == nullptr || len == 0) {
        why = "合成结果是空的";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(req.out.parent_path(), ec);
    // 二进制写。文本模式在 Windows 上会把 0x0A 换成 0x0D0A，
    // 把 wav 写坏——而坏在哪里要用十六进制看才知道。
    std::FILE* f = std::fopen(paths::to_utf8(req.out).c_str(), "wb");
    if (f == nullptr) {
        why = "写不了 " + paths::to_utf8(req.out);
        return false;
    }
    const size_t wrote = std::fwrite(data, 1, len, f);
    std::fclose(f);
    if (wrote != len) {
        why = "只写进去 " + std::to_string(wrote) + " / " + std::to_string(len) +
              " 字节（磁盘满了？）";
        return false;
    }

    out_duration_s = rate > 0 ? static_cast<double>(samples) / rate : 0.0;
    return true;
}

#else

bool llama_tts_available() { return false; }

LlamaTtsProbe probe_llama_tts() {
    LlamaTtsProbe out;
    out.ok = true;  // 没编进来不算故障，配音走外部后端
    out.detail = "没编进来（CHANGJI_LLAMA=OFF），配音走外部后端";
    return out;
}

struct LlamaTts::Impl {};
LlamaTts::LlamaTts() : impl_(std::make_unique<Impl>()) {}
LlamaTts::~LlamaTts() = default;
int LlamaTts::sample_rate() const { return 0; }

std::unique_ptr<LlamaTts> LlamaTts::load(const std::filesystem::path&,
                                         const std::filesystem::path&, bool,
                                         std::string& why) {
    why = "这个二进制没编进程内配音（CHANGJI_LLAMA=OFF）";
    return nullptr;
}

bool LlamaTts::synthesize(const LlamaTtsRequest&, double&, std::string& why) {
    why = "这个二进制没编进程内配音（CHANGJI_LLAMA=OFF）";
    return false;
}

#endif

}  // namespace changji::infer
