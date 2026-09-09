#include "infer/sd_image.hpp"

#include <atomic>
#include <fstream>
#include <mutex>
#include <vector>

#include "infer/scheduler.hpp"
#include "util/paths.hpp"

#ifdef CHANGJI_HAVE_SD
#include <stable-diffusion.h>
// **两个 STATIC 不能删。** 它们让 stb 的所有函数变成内部链接，
// 只在这个 .cpp 里可见。
//
// 不加的话：开了 CHANGJI_LLAMA 之后 mtmd 也带一份 stb（vendor::stb），
// 链接时几十个 stbi_* 符号重复定义，直接 LNK1169。两份 stb 同时存在
// 是常态而不是意外——sd.cpp 和 llama.cpp 各自 vendor 了一份，
// 这正是方案风险二说的那类问题，只是这次撞在第三方小库上。
//
// 用 STATIC 而不是"改成用 mtmd 那份"：两份 stb 的版本不一定一样，
// 借用别人的实现等于把自己的行为绑在对方的升级上。
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <thirdparty/stb_image_write.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <thirdparty/stb_image.h>
#endif

namespace fs = std::filesystem;

namespace changji::infer {

// **这一段在 #ifdef 外面**：它只看配置，和有没有链上 sd.cpp 无关，
// 而测试目标编的是没链上游那一支。写在 #ifdef 里面就测不到了。
std::string sd_model_problem(const config::Settings& settings, ModelRole role) {
    const auto& m = settings.models;
    if (role == ModelRole::Video) {
        if (m.video.empty()) {
            return "没配视频模型。在 changji.toml 的 [models] 里填 video，"
                   "或者把出片交给推理服务";
        }
        return {};
    }
    if (!m.image.empty()) return {};
    if (!m.video.empty()) {
        // 拿视频模型走 generate_image 会整个进程崩，见头文件里的说明。
        return "没配出图模型（[models].image）。\n"
               "**不能用视频模型顶替**：sd.cpp 的 generate_image 没有帧数"
               "参数，拿 Wan 这类视频模型进去会让进程直接崩掉（实测）。\n"
               "要么在 [models] 里填一个图像模型（比如 "
               "Qwen-Image-Edit-Q4_K_M.gguf），\n"
               "要么把出图交给推理服务（[models].engine = \"comfy\"）";
    }
    return "没配出图模型。在 changji.toml 的 [models] 里填 image，"
           "或者把出图交给推理服务";
}

#ifdef CHANGJI_HAVE_SD

namespace {

/// 正在跑的那一次生成。
///
/// sd.cpp 的进度回调是**全局的**（sd_set_progress_callback 不带 ctx），
/// 所以这里只能用一个全局的当前状态。这意味着**同一时刻只能跑一次生成**——
/// 但那本来就是事实：显卡只有一张，并发只会更慢。
struct ActiveGeneration {
    std::mutex mu;
    sd_ctx_t* ctx = nullptr;
    const StepCallback* on_step = nullptr;
    pipeline::CancelToken* tok = nullptr;
    std::atomic<bool> cancel_sent{false};
    /// 这一次生成**要采样几步**。sd.cpp 的进度回调加载权重和采样共用一个，
    /// 靠"报上来的总数是不是等于我们要的步数"把两者分开。
    int want_steps = 0;
};

ActiveGeneration& active() {
    static ActiveGeneration a;
    return a;
}

void progress_trampoline(int step, int steps, float time, void* /*data*/) {
    ActiveGeneration& a = active();
    StepCallback cb;
    sd_ctx_t* ctx = nullptr;
    pipeline::CancelToken* tok = nullptr;
    {
        std::lock_guard lg(a.mu);
        if (a.on_step) cb = *a.on_step;
        ctx = a.ctx;
        tok = a.tok;
    }
    int want = 0;
    {
        std::lock_guard lg(a.mu);
        want = a.want_steps;
    }
    // 总数对不上我们要的步数，就说明这一轮回调不是采样。
    //
    // **但它不一定是"加载模型"。** sd.cpp 拿同一个回调报好几种阶段，
    // 总数各不相同：分段搬权重（权重放内存时每镜都要搬一遍）、
    // VAE 分块解码（每块一格）、首次从磁盘载权重。上层只看得到
    // (step, steps)，分不出是哪一种，所以文案不能写死成"加载模型"——
    // 写死之后每镜都冒出来，看着像"模型被重载了 22 次"，
    // 而实际整轮只从磁盘载过一次。
    const bool loading = want > 0 && steps != want;
    if (cb) cb(step, steps, static_cast<double>(time), loading);

    // 取消是**在这里**发出去的。sd.cpp 的采样循环没有别的插手点，
    // 不在回调里发的话，点了停止要等这一镜跑完——低配机器上那是好几分钟。
    if (tok && tok->cancelled() && ctx &&
        !a.cancel_sent.exchange(true, std::memory_order_relaxed)) {
        ::sd_cancel_generation(ctx, SD_CANCEL_ALL);
    }
}

/// 把 PNG 写到磁盘。
///
/// **不能用 stbi_write_png**：它内部走 fopen(const char*)，Windows 上
/// 那个路径按 ANSI 代码页解释，中文项目名直接写不进去，而且报的错是
/// "写文件失败"，看不出是编码问题。所以先编码到内存再自己落盘。
void write_png(const fs::path& dest, const sd_image_t& img) {
    std::vector<unsigned char> buf;
    const auto sink = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* p = static_cast<const unsigned char*>(data);
        out->insert(out->end(), p, p + size);
    };
    const int ok = ::stbi_write_png_to_func(
        sink, &buf, static_cast<int>(img.width), static_cast<int>(img.height),
        static_cast<int>(img.channel), img.data,
        static_cast<int>(img.width * img.channel));
    if (!ok || buf.empty()) throw SdError("PNG 编码失败");

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    if (!f) throw SdError("写不了 " + paths::to_utf8(dest));
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    f.close();
    if (!f) throw SdError("写 " + paths::to_utf8(dest) + " 时出错");
}

/// 读一张参考图。同样不能让 stb 自己开文件，理由同上。
sd_image_t load_image(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw SdError("读不了参考图 " + paths::to_utf8(p));
    const std::string bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    int w = 0, h = 0, c = 0;
    unsigned char* data = ::stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(bytes.data()),
        static_cast<int>(bytes.size()), &w, &h, &c, 3);
    if (!data) throw SdError("参考图解不开：" + paths::to_utf8(p));
    sd_image_t img{};
    img.width = static_cast<uint32_t>(w);
    img.height = static_cast<uint32_t>(h);
    img.channel = 3;
    img.data = data;
    return img;
}

std::string vram_arg(double gb) {
    if (gb <= 0.0) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", gb);
    return std::string(buf);
}

}  // namespace

struct SdContext::Impl {
    sd_ctx_t* ctx = nullptr;
    // 路径要活到 sd_ctx 建完：sd_ctx_params_t 存的是 const char*，
    // 不拷贝。传临时 string 的 c_str() 的话，new_sd_ctx 读到的是野指针。
    std::string diffusion, vae, text_encoder, max_vram, params_backend;
    /// 双专家模型的高噪声那一份（Wan 2.2 A14B）。空 = 单模型。
    /// 和上面几个一样，路径要活到 sd_ctx 建完。
    std::string high_noise;
    /// 音频 VAE（MiniMax-H3 那类音画一起出的模型）。
    std::string audio_vae;
    /// 出片挂的 LoRA。sd_lora_t 存的是 const char*，不拷贝，所以路径
    /// 要活到 generate_video 调完，不能用临时 string 的 c_str()。
    std::string lora;
    float lora_strength = 1.0f;
    /// VAE 分块大小的覆盖值，0 = 用请求里带的默认。
    int vae_tile = 0;
    /// Qwen-Image 那一路的文本编码器（sd.cpp 的 llm_path）和它的视觉塔。
    /// **和 text_encoder 互斥**：一次只填其中一边——Wan 走 t5xxl，
    /// Qwen-Image 走 llm，两个参数位不是一回事。
    std::string llm, llm_vision;
    /// 这个角色的采样旋钮，建上下文时按 [models] 里的角色值定下来。
    double cfg = 7.0;
    double flow_shift = 3.0;
    /// 两个专家交班的 sigma 阈值。只在 high_noise 非空时有意义。
    double moe_boundary = 0.875;

    ~Impl() {
        if (ctx) ::free_sd_ctx(ctx);
    }
};

std::shared_ptr<SdContext> SdContext::create(const config::Settings& settings,
                                             double vram_budget_gb,
                                             ModelRole role) {
    const fs::path ws = settings.workspace_path();
    const auto& m = settings.models;
    if (const std::string why = sd_model_problem(settings, role); !why.empty()) {
        throw SdError(why);
    }

    auto self = std::shared_ptr<SdContext>(new SdContext());
    self->impl_ = std::make_unique<Impl>();
    Impl& impl = *self->impl_;

    // 出视频用视频模型，出首帧用图像模型。
    //
    // **原来这里写着"没配就退回视频模型出单帧"，那条退路是坏的**，
    // 上面那个 throw 里记了为什么（sd.cpp 的 generate_image 没有帧数参数，
    // 拿视频模型进去整个进程崩）。走到这里时 image 一定非空。
    const bool is_video = role == ModelRole::Video;
    const std::string& which = is_video ? m.video : m.image;
    impl.cfg = is_video ? m.video_cfg : m.image_cfg;
    impl.flow_shift = is_video ? m.video_flow_shift : m.image_flow_shift;
    impl.diffusion = paths::to_utf8(m.resolve(which, ws));
    // 双专家的高噪声那一份。只有视频那条路有——Qwen-Image 不是 MoE。
    if (is_video && !m.video_high_noise.empty()) {
        impl.high_noise = paths::to_utf8(m.resolve(m.video_high_noise, ws));
        impl.moe_boundary = m.video_moe_boundary;
    }

    // **VAE 和文本编码器要按角色挑，不能两边共用一套。**
    // 之前这里写死了 video_vae + video_text_encoder，图像那条路也拿它俩用
    // ——因为图像那条路从来没真跑过，没人发现。
    // Wan 的 VAE 和 Qwen-Image 的不是一回事；UMT5-XXL 和 Qwen2.5-VL 更不是，
    // 而且在 sd.cpp 里根本不是同一个参数位（t5xxl_path vs llm_path，
    // 见上游 docs/qwen_image_edit.md）。
    // **喂错了不报错**：照常加载，然后出一张和提示词没关系的图。
    // 图像那几项留空就退回视频那套，只用 Wan 的人不必填两遍。
    const std::string& vae_key =
        (!is_video && !m.image_vae.empty()) ? m.image_vae : m.video_vae;
    impl.vae = vae_key.empty() ? "" : paths::to_utf8(m.resolve(vae_key, ws));

    // 出片的 LoRA（Turbo 那类蒸馏适配器）。
    if (is_video && !m.video_lora.empty()) {
        impl.lora = paths::to_utf8(m.resolve(m.video_lora, ws));
        impl.lora_strength = static_cast<float>(m.video_lora_strength);
    }
    if (is_video && m.video_vae_tile > 0) impl.vae_tile = m.video_vae_tile;

    // 音频 VAE：MiniMax-H3 那类画面和声音一起出的模型才有。
    if (is_video && !m.video_audio_vae.empty()) {
        impl.audio_vae = paths::to_utf8(m.resolve(m.video_audio_vae, ws));
    }

    if (is_video && !m.video_llm.empty()) {
        // **视频模型也可能用 llm_path 而不是 t5xxl_path。**
        // Wan 那一路是 UMT5-XXL → t5xxl；MiniMax-H3 用裁过的 Qwen3-VL-32B
        // → llm。校验那边保证这两项不会同时填。
        impl.llm = paths::to_utf8(m.resolve(m.video_llm, ws));
        if (!m.video_llm_vision.empty()) {
            impl.llm_vision = paths::to_utf8(m.resolve(m.video_llm_vision, ws));
        }
    } else if (!is_video && !m.image_text_encoder.empty()) {
        impl.llm = paths::to_utf8(m.resolve(m.image_text_encoder, ws));
        if (!m.image_text_encoder_vision.empty()) {
            impl.llm_vision =
                paths::to_utf8(m.resolve(m.image_text_encoder_vision, ws));
        }
    } else {
        impl.text_encoder = m.video_text_encoder.empty()
                                ? ""
                                : paths::to_utf8(
                                      m.resolve(m.video_text_encoder, ws));
    }
    impl.max_vram = vram_arg(vram_budget_gb);
    // 权重放哪，见 ModelsConfig::weights。
    //
    // cpu：权重放系统内存，用到才搬进显存。**这是 6GB 卡上能跑的关键**——
    // 见方案里"跨模型调度"那一节。不管的话（权重直接进显存）在那台机器上
    // 加载阶段就 OOM。
    //
    // auto：params_backend 留空 + auto_fit。sd.cpp 只在两个后端 spec 都为空时
    // 才启用 auto_fit（stable-diffusion.cpp 里那一行 `&& params_backend_spec.empty()`），
    // 所以这里**不能**填 "" 之外的任何东西。它按这张卡真实的空闲显存逐组件放。
    // auto 时留空（sd.cpp 只在两个 spec 都为空时才启用 auto_fit）；
    // cpu 就是 "cpu"；别的原样传下去当组件规格。
    const bool auto_fit = m.weights == "auto";
    impl.params_backend = auto_fit ? "" : m.weights;

    sd_ctx_params_t p{};
    ::sd_ctx_params_init(&p);
    p.auto_fit = auto_fit;
    p.diffusion_model_path = impl.diffusion.c_str();
    if (!impl.high_noise.empty()) {
        p.high_noise_diffusion_model_path = impl.high_noise.c_str();
    }
    if (!impl.vae.empty()) p.vae_path = impl.vae.c_str();
    if (!impl.audio_vae.empty()) p.audio_vae_path = impl.audio_vae.c_str();
    // 随机数发生器。**sd.cpp 的默认是 cuda**，而上游给 MiniMax-H3 的命令行
    // 是 --rng cpu。发生器不同则初始噪声不同，同一个种子出的画面就不一样，
    // 而且没有任何报错。只在出片这条路上认这一项。
    if (is_video && m.video_rng != "cuda") {
        p.rng_type = ::str_to_rng_type(m.video_rng.c_str());
    }
    if (!impl.text_encoder.empty()) {
        // **是 t5xxl_path，不是 embeddings_connectors_path。**
        //
        // 原来写的是后者。sd.cpp 里那是另一条加载路径（日志写的是
        // "loading embeddings connectors from ..."），而且**加载失败只 warn
        // 不报错**——于是 umt5-xxl 被静默忽略、t5xxl_path 是空的，
        // Wan 拿不到任何文本条件。症状会是"出的片和提示词没关系"，
        // 而日志里只有一行 warn，指不到这儿。
        //
        // 依据是 sd.cpp 自己的 docs/wan.md，那上面的命令行是：
        //   --t5xxl ...\models\text_encoders\umt5-xxl-encoder-Q8_0.gguf
        p.t5xxl_path = impl.text_encoder.c_str();
    }
    // 上游 docs/wan.md 给 Wan 的命令行带着 --diffusion-fa，而
    if (!impl.llm.empty()) p.llm_path = impl.llm.c_str();
    if (!impl.llm_vision.empty()) p.llm_vision_path = impl.llm_vision.c_str();

    // sd_ctx_params_init 的默认是 false。6 GB 卡上这一项直接影响塞不塞得下。
    p.diffusion_flash_attn = m.diffusion_flash_attn;
    p.max_vram = impl.max_vram.c_str();
    p.params_backend = impl.params_backend.c_str();
    p.n_threads = -1;   // -1 = 物理核数

    impl.ctx = ::new_sd_ctx(&p);
    if (impl.ctx == nullptr) {
        throw SdError("出图模型加载失败：" + impl.diffusion +
                      "\n看一眼上面 sd.cpp 打的日志，多半是文件不对或者显存不够");
    }
    return self;
}

SdContext::~SdContext() = default;

void SdContext::generate(const ImageRequest& req, const fs::path& dest,
                         pipeline::CancelToken& tok,
                         const StepCallback& on_step) {
    if (tok.cancelled()) throw SdError("已取消");

    std::vector<sd_image_t> refs;
    for (const auto& p : req.reference_images) refs.push_back(load_image(p));
    // 上面任何一张读失败都会抛，此时前面几张的内存还没释放。
    // 用一个哨兵在退出时收拾。
    struct RefGuard {
        std::vector<sd_image_t>& v;
        ~RefGuard() {
            for (auto& i : v) ::stbi_image_free(i.data);
        }
    } guard{refs};

    sd_img_gen_params_t g{};
    ::sd_img_gen_params_init(&g);
    g.prompt = req.positive.c_str();
    g.negative_prompt = req.negative.c_str();
    g.width = req.width;
    g.height = req.height;
    g.seed = req.seed;
    g.batch_count = 1;
    g.sample_params.sample_steps = req.steps;
    // cfg / flow_shift 按角色从 [models] 来，不用请求里那个 7.0——
    // 见 ModelsConfig::image_cfg 上面那段。
    g.sample_params.guidance.txt_cfg = static_cast<float>(impl_->cfg);
    g.sample_params.flow_shift = static_cast<float>(impl_->flow_shift);
    // VAE 分块解码——和 generate_video 那边同一套写法。不填的话 sd.cpp
    // 整图解码，1280×704 要 6.6 GB 缓冲，fp8 常驻的 32 GB 卡上出不来图。
    g.vae_tiling_params.enabled = req.vae_tiling;
    g.vae_tiling_params.tile_size_x = req.vae_tile_x;
    g.vae_tiling_params.tile_size_y = req.vae_tile_y;
    g.vae_tiling_params.target_overlap = static_cast<float>(req.vae_tile_overlap);
    g.vae_tiling_params.rel_size_x = 0.0f;
    g.vae_tiling_params.rel_size_y = 0.0f;
    if (!refs.empty()) {
        g.ref_images = refs.data();
        g.ref_images_count = static_cast<int>(refs.size());
    }

    ActiveGeneration& a = active();
    {
        std::lock_guard lg(a.mu);
        a.ctx = impl_->ctx;
        a.on_step = &on_step;
        a.tok = &tok;
        a.want_steps = req.steps;
    }
    a.cancel_sent.store(false, std::memory_order_relaxed);
    ::sd_set_progress_callback(progress_trampoline, nullptr);

    sd_image_t* out = nullptr;
    int count = 0;
    const bool ok = ::generate_image(impl_->ctx, &g, &out, &count);

    {
        std::lock_guard lg(a.mu);
        a.ctx = nullptr;
        a.on_step = nullptr;
        a.tok = nullptr;
        a.want_steps = 0;
    }

    if (tok.cancelled()) {
        if (out) ::free_sd_images(out, count);
        throw SdError("已取消");
    }
    if (!ok || out == nullptr || count <= 0) {
        if (out) ::free_sd_images(out, count);
        throw SdError("出图失败，看一眼上面 sd.cpp 打的日志");
    }

    try {
        write_png(dest, out[0]);
    } catch (...) {
        ::free_sd_images(out, count);
        throw;
    }
    ::free_sd_images(out, count);
}

void SdContext::generate_video(const VideoRequest& req, const fs::path& raw_dest,
                               pipeline::CancelToken& tok,
                               const StepCallback& on_step) {
    if (tok.cancelled()) throw SdError("已取消");
    if (!::sd_ctx_supports_video_generation(impl_->ctx)) {
        throw SdError("这个模型不支持出视频。[models].video 要填一个视频模型"
                      "（比如 Wan），填成图像模型是不行的");
    }

    sd_image_t start{};
    bool has_start = false;
    if (req.start_image.has_value()) {
        start = load_image(*req.start_image);
        has_start = true;
    }
    struct StartGuard {
        sd_image_t& img;
        bool& has;
        ~StartGuard() {
            if (has) ::stbi_image_free(img.data);
        }
    } sguard{start, has_start};

    sd_vid_gen_params_t g{};
    ::sd_vid_gen_params_init(&g);
    g.prompt = req.positive.c_str();
    g.negative_prompt = req.negative.c_str();
    g.width = req.width;
    g.height = req.height;
    g.video_frames = req.frames;
    g.fps = req.fps;
    g.seed = req.seed;
    g.sample_params.sample_steps = req.steps;
    // cfg / flow_shift 按角色从 [models] 来，不用请求里那个 7.0——
    // 见 ModelsConfig::image_cfg 上面那段。
    g.sample_params.guidance.txt_cfg = static_cast<float>(impl_->cfg);
    g.sample_params.flow_shift = static_cast<float>(impl_->flow_shift);
    // **高噪声专家的旋钮要单独填一遍。** sd_vid_gen_params_init 给它的是
    // 另一套默认值（cfg 7.0、flow_shift 无穷），不填的话前几步会在一个
    // 和低噪声那份完全不同的 cfg 上跑——而这**不会报错**，只是出来的片
    // 前后不搭。上游 docs/wan.md 的 A14B 命令行两边给的就是同一个 cfg。
    //
    // 步数保持 init 给的 -1：那是"按 moe_boundary 自动分"的意思
    // （sd.cpp 扫 sigma 序列，第一个小于阈值的下标就是交班点）。
    // 填成具体数字的话两段步数是**相加**的，总步数会翻倍。
    g.high_noise_sample_params.guidance.txt_cfg =
        static_cast<float>(impl_->cfg);
    g.high_noise_sample_params.flow_shift =
        static_cast<float>(impl_->flow_shift);
    g.moe_boundary = static_cast<float>(impl_->moe_boundary);

    // LoRA。**这个数组要活到 generate_video 返回**——sd_vid_gen_params_t
    // 存的是指针，不拷贝。放在这一层的局部变量里正好（下面就调用了）。
    sd_lora_t lora{};
    if (!impl_->lora.empty() && req.use_lora) {
        lora.path = impl_->lora.c_str();
        lora.multiplier = impl_->lora_strength;
        // H3 不是混合专家，高噪声那份不存在；A14B 挂 LoRA 的话这里要分两条。
        lora.is_high_noise = false;
        g.loras = &lora;
        g.lora_count = 1;
    }

    if (has_start) g.init_image = start;

    // VAE 分块。**不设的话默认是关的**，而关着在 6GB 卡上解码要 11.7GB，
    // 直接失败。见 VideoRequest 里那张实测表。
    g.vae_tiling_params.enabled = req.vae_tiling;
    g.vae_tiling_params.temporal_tiling = req.vae_temporal_tiling;
    // 配置里给了就盖掉请求带的默认：调小分块换显存，好让 VAE 权重常驻。
    g.vae_tiling_params.tile_size_x =
        impl_->vae_tile > 0 ? impl_->vae_tile : req.vae_tile_x;
    g.vae_tiling_params.tile_size_y =
        impl_->vae_tile > 0 ? impl_->vae_tile : req.vae_tile_y;
    g.vae_tiling_params.target_overlap = static_cast<float>(req.vae_tile_overlap);
    // 显式清零：rel_size 非零时 sd.cpp 优先用比例、忽略上面的绝对块大小
    // （见 vae.hpp 的 get_tile_size）。依赖 init 把它清成 0 的话，
    // 上游哪天改了默认值，这里会静默地换成另一套分块策略。
    g.vae_tiling_params.rel_size_x = 0.0f;
    g.vae_tiling_params.rel_size_y = 0.0f;

    ActiveGeneration& a = active();
    {
        std::lock_guard lg(a.mu);
        a.ctx = impl_->ctx;
        a.on_step = &on_step;
        a.tok = &tok;
        a.want_steps = req.steps;
    }
    a.cancel_sent.store(false, std::memory_order_relaxed);
    ::sd_set_progress_callback(progress_trampoline, nullptr);

    sd_image_t* frames = nullptr;
    int count = 0;
    sd_audio_t* audio = nullptr;
    const bool ok = ::generate_video(impl_->ctx, &g, &frames, &count, &audio);

    {
        std::lock_guard lg(a.mu);
        a.ctx = nullptr;
        a.on_step = nullptr;
        a.tok = nullptr;
        a.want_steps = 0;
    }

    struct FrameGuard {
        sd_image_t*& f;
        int& n;
        ~FrameGuard() {
            if (f) ::free_sd_images(f, n);
        }
    } fguard{frames, count};

    if (tok.cancelled()) throw SdError("已取消");
    if (!ok || frames == nullptr || count <= 0) {
        throw SdError("出视频失败，看一眼上面 sd.cpp 打的日志");
    }

    // 裸 RGB24 顺序写出去。**逐帧写不是一次性拼成一个大 buffer**：
    // 121 帧 448x768 是 125 MB，先攒在内存里等于在出视频最吃内存的
    // 那一刻再要 125 MB。
    std::error_code ec;
    fs::create_directories(raw_dest.parent_path(), ec);
    std::ofstream f(raw_dest, std::ios::binary | std::ios::trunc);
    if (!f) throw SdError("写不了 " + paths::to_utf8(raw_dest));
    for (int i = 0; i < count; ++i) {
        const sd_image_t& img = frames[i];
        if (img.channel != 3) {
            throw SdError("sd.cpp 吐的帧不是 RGB24（channel=" +
                          std::to_string(img.channel) + "），编码参数对不上");
        }
        const std::size_t n =
            static_cast<std::size_t>(img.width) * img.height * img.channel;
        f.write(reinterpret_cast<const char*>(img.data),
                static_cast<std::streamsize>(n));
        if (!f) throw SdError("写裸帧时出错，多半是磁盘满了");
    }
    f.close();
    if (!f) throw SdError("写 " + paths::to_utf8(raw_dest) + " 时出错");
}

#else   // 没链 sd.cpp

struct SdContext::Impl {};

std::shared_ptr<SdContext> SdContext::create(const config::Settings&, double,
                                             ModelRole) {
    throw SdError("这个二进制没有编进出图后端。出图请走推理服务，"
                  "或者用带 sd.cpp 的构建");
}
SdContext::~SdContext() = default;
void SdContext::generate(const ImageRequest&, const fs::path&,
                         pipeline::CancelToken&, const StepCallback&) {
    throw SdError("这个二进制没有编进出图后端");
}
void SdContext::generate_video(const VideoRequest&, const fs::path&,
                               pipeline::CancelToken&, const StepCallback&) {
    throw SdError("这个二进制没有编进出图后端");
}

#endif

// ---- 挂到调度器上 ----
//
// 这一段两种构建都要有：没链 sd.cpp 时槽照样注册，
// acquire 的时候 create() 抛异常，调度器会把账退干净。
// 不注册的话调用方拿到的是"槽还没注册"，那句话对用户没意义。

namespace {

std::mutex g_ctx_mu;
std::shared_ptr<SdContext> g_image_ctx;
std::shared_ptr<SdContext> g_video_ctx;

}  // namespace

void register_sd_slots(const config::Settings& settings,
                       const models::HardwareProfile& profile) {
    register_sd_slots([settings] { return settings; }, profile);
}

void register_sd_slots(SettingsProvider raw_provider,
                       const models::HardwareProfile& profile) {
    // **weights = "smart" 在这里展开成 sd.cpp 认的组件规格。**
    // 只有这一层拿得到这张卡真实的显存（profile），而 SdContext::create
    // 只看 Settings。包一层 provider，下面所有地方看到的都是展开后的值——
    // 包括 budget_for 和 set_budget，它们都按 weights 的取值分支。
    const double card_gb = profile.gpu.has_value() ? profile.gpu->vram_gb()
                                                   : profile.vram_gb;
    const SettingsProvider provider = [raw_provider, card_gb] {
        config::Settings s = raw_provider();
        s.models.weights = s.models.weights_for(card_gb);
        return s;
    };
    // 预算取探测到的显存，留一成给驱动上下文和别的程序。
    //
    // 估高了是 OOM 直接崩，估低了只是多分段（慢）。所以往低了取——
    // 这条和 Scheduler 里那个 vram_estimate 的取舍是同一个道理。
    const double budget = profile.vram_gb > 0 ? profile.vram_gb * 0.9 : 0.0;
    // **auto 模式的预算按物理显存算，不按 vram_gb_override。**
    // override 是拿来挑档位的（44 GB 的卡想要 1280×704 的成片档就填 20），
    // 拿它当显存预算的话 auto_fit 会把本来装得下的编码器赶去内存。
    // 探测不到卡（没有 nvidia-smi）就退回上面那个数。
    // **先减计算缓冲的余量，再乘系数。**
    //
    // 0.9 那一成是给驱动上下文和别的程序留的，**不是给计算缓冲留的**——
    // 缓冲比它大一个量级：1280×704 的 VAE 解码实测 6576 MB。
    // 不减的话权重会把显存占满，然后 decode_first_stage 失败，
    // 而在接上 sd.cpp 的日志之前，上层只看得到一句"出图失败"。
    const double reserve = provider().models.vram_reserve_gb;
    const double physical =
        profile.gpu.has_value()
            ? std::max(1.0, profile.gpu->vram_gb() - reserve) * 0.9
            : budget;
    // 显式给了组件规格（比如 te=cpu,vae=cpu）的时候，权重放哪已经由用户
    // 定了，余量那 6 GB 就别再扣——扣了预算只剩 23 GB，扩散模型 19.5 GB
    // 加上它自己的计算缓冲就超了，5090 上表现是
    // "segment 56/62 failed during weight preparation"。整卡按 0.9 给。
    const double whole =
        profile.gpu.has_value() ? profile.gpu->vram_gb() * 0.9 : budget;
    const auto budget_for = [budget, physical, whole](const config::Settings& s) {
        if (s.models.weights == "auto") return physical;
        if (s.models.weights == "cpu") return budget;
        return whole;
    };
    const std::size_t estimate =
        static_cast<std::size_t>(budget * 1024) * 1024 * 1024;

    // **预算要真的设上。** 不设的话 Scheduler::make_room 第一行就
    // `budget_ == 0 → return true`，谁也不驱逐谁——下面"同时只装得下一个"
    // 那句注释描述的行为从来没生效过。
    //
    // 权重放系统内存时看不出来（显存里只有计算缓冲）；换成 weights = "auto"
    // 之后两套权重都常驻显存，出完首帧切去出片时一张 45 GB 的卡上是
    // 图像模型 + 它的编码器 + 视频模型 + umt5，直接 CUDA OOM，进程 abort。
    //
    // 预算按 auto 那条路的数取：它才是真占显存的那个。
    // **把实时显存的问法装上。** 不装的话调度器只信静态估算，
    // 每次切阶段都卸一个再装一个——一次重装几十秒到几分钟，
    // 而卡上可能一直空着一大半（权重放内存时显存里只有计算缓冲）。
    scheduler().set_free_vram_probe([] { return models::free_vram_gb(); });

    scheduler().set_budget(
        static_cast<std::size_t>(
            (provider().models.weights == "auto" ? physical : budget) * 1024) *
        1024 * 1024);

    // 两个槽的估值都按整个预算算，也就是**同时只装得下一个**。
    // 这不是保守，是事实：6GB 卡上图像模型和视频模型任意一个都要占满，
    // 让调度器知道这件事，它才会在切阶段时先卸掉另一个。
    {
        SlotSpec spec;
        spec.slot = Slot::Image;
        spec.residency = Residency::Cached;   // 每个镜头都要，别反复卸
        spec.vram_estimate = estimate;
        // 视频模型重新加载更贵（文件大得多），所以图像的优先级更低，
        // 腾地方时先卸它。
        spec.evict_priority = 5;
        spec.load = [provider, budget_for] {
            const config::Settings s = provider();
            auto ctx = SdContext::create(s, budget_for(s), ModelRole::Image);
            std::lock_guard lg(g_ctx_mu);
            g_image_ctx = std::move(ctx);
        };
        spec.unload = [] {
            std::lock_guard lg(g_ctx_mu);
            g_image_ctx.reset();   // 析构里 free_sd_ctx
        };
        scheduler().register_slot(std::move(spec));
    }
    {
        SlotSpec spec;
        spec.slot = Slot::Video;
        spec.residency = Residency::Cached;
        spec.vram_estimate = estimate;
        spec.evict_priority = 9;
        spec.load = [provider, budget_for] {
            const config::Settings s = provider();
            auto ctx = SdContext::create(s, budget_for(s), ModelRole::Video);
            std::lock_guard lg(g_ctx_mu);
            g_video_ctx = std::move(ctx);
        };
        spec.unload = [] {
            std::lock_guard lg(g_ctx_mu);
            g_video_ctx.reset();
        };
        scheduler().register_slot(std::move(spec));
    }
}

std::shared_ptr<SdContext> current_image_context() {
    std::lock_guard lg(g_ctx_mu);
    return g_image_ctx;
}

std::shared_ptr<SdContext> current_video_context() {
    std::lock_guard lg(g_ctx_mu);
    return g_video_ctx;
}

}  // namespace changji::infer
