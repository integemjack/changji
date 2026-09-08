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
    if (cb) cb(step, steps, static_cast<double>(time));

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

    ~Impl() {
        if (ctx) ::free_sd_ctx(ctx);
    }
};

std::shared_ptr<SdContext> SdContext::create(const config::Settings& settings,
                                             double vram_budget_gb,
                                             ModelRole role) {
    const fs::path ws = settings.workspace_path();
    const auto& m = settings.models;
    if (role == ModelRole::Video && m.video.empty()) {
        throw SdError("没配视频模型。在 changji.toml 的 [models] 里填 video，"
                      "或者把出片交给推理服务");
    }
    if (role == ModelRole::Image && m.image.empty() && m.video.empty()) {
        throw SdError(
            "没配出图模型。在 changji.toml 的 [models] 里填 image 或 video，"
            "或者把出图交给推理服务");
    }

    auto self = std::shared_ptr<SdContext>(new SdContext());
    self->impl_ = std::make_unique<Impl>();
    Impl& impl = *self->impl_;

    // 出视频只能用视频模型。出首帧优先用图像模型（能吃参考图，
    // 跨镜头一致性远好于视频模型），没配就退回视频模型出单帧——
    // 这个选择和 Python 的 build_backend 是同一个判断。
    const std::string& which =
        role == ModelRole::Video ? m.video
                                 : (m.image.empty() ? m.video : m.image);
    impl.diffusion = paths::to_utf8(m.resolve(which, ws));
    impl.vae = m.video_vae.empty() ? "" : paths::to_utf8(m.resolve(m.video_vae, ws));
    impl.text_encoder = m.video_text_encoder.empty()
                            ? ""
                            : paths::to_utf8(m.resolve(m.video_text_encoder, ws));
    impl.max_vram = vram_arg(vram_budget_gb);
    // 权重放系统内存，用到才搬进显存。**这是 6GB 卡上能跑的关键**——
    // 见方案里"跨模型调度"那一节。默认（权重直接进显存）在这台机器上
    // 加载阶段就 OOM。
    impl.params_backend = "cpu";

    sd_ctx_params_t p{};
    ::sd_ctx_params_init(&p);
    p.diffusion_model_path = impl.diffusion.c_str();
    if (!impl.vae.empty()) p.vae_path = impl.vae.c_str();
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
    g.sample_params.guidance.txt_cfg = static_cast<float>(req.cfg);
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
    g.sample_params.guidance.txt_cfg = static_cast<float>(req.cfg);
    if (has_start) g.init_image = start;

    // VAE 分块。**不设的话默认是关的**，而关着在 6GB 卡上解码要 11.7GB，
    // 直接失败。见 VideoRequest 里那张实测表。
    g.vae_tiling_params.enabled = req.vae_tiling;
    g.vae_tiling_params.temporal_tiling = req.vae_temporal_tiling;
    g.vae_tiling_params.tile_size_x = req.vae_tile_x;
    g.vae_tiling_params.tile_size_y = req.vae_tile_y;
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

void register_sd_slots(SettingsProvider provider,
                       const models::HardwareProfile& profile) {
    // 预算取探测到的显存，留一成给驱动上下文和别的程序。
    //
    // 估高了是 OOM 直接崩，估低了只是多分段（慢）。所以往低了取——
    // 这条和 Scheduler 里那个 vram_estimate 的取舍是同一个道理。
    const double budget = profile.vram_gb > 0 ? profile.vram_gb * 0.9 : 0.0;
    const std::size_t estimate =
        static_cast<std::size_t>(budget * 1024) * 1024 * 1024;

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
        spec.load = [provider, budget] {
            auto ctx = SdContext::create(provider(), budget, ModelRole::Image);
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
        spec.load = [provider, budget] {
            auto ctx = SdContext::create(provider(), budget, ModelRole::Video);
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
