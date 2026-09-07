#include "infer/sd_image.hpp"

#include <atomic>
#include <fstream>
#include <mutex>
#include <vector>

#include "infer/scheduler.hpp"
#include "util/paths.hpp"

#ifdef CHANGJI_HAVE_SD
#include <stable-diffusion.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
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
                                             double vram_budget_gb) {
    const fs::path ws = settings.workspace_path();
    const auto& m = settings.models;
    if (m.image.empty() && m.video.empty()) {
        throw SdError(
            "没配出图模型。在 changji.toml 的 [models] 里填 image 或 video，"
            "或者把出图交给推理服务");
    }

    auto self = std::shared_ptr<SdContext>(new SdContext());
    self->impl_ = std::make_unique<Impl>();
    Impl& impl = *self->impl_;

    // 有图像模型就用它出首帧，一致性更好；没有就用视频模型出单帧。
    // 这个选择和 Python 的 build_backend 是同一个判断。
    const std::string& which = m.image.empty() ? m.video : m.image;
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
        p.embeddings_connectors_path = impl.text_encoder.c_str();
    }
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

#else   // 没链 sd.cpp

struct SdContext::Impl {};

std::shared_ptr<SdContext> SdContext::create(const config::Settings&, double) {
    throw SdError("这个二进制没有编进出图后端。出图请走推理服务，"
                  "或者用带 sd.cpp 的构建");
}
SdContext::~SdContext() = default;
void SdContext::generate(const ImageRequest&, const fs::path&,
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

}  // namespace

void register_sd_slots(const config::Settings& settings,
                       const models::HardwareProfile& profile) {
    // 预算取探测到的显存，留一成给驱动上下文和别的程序。
    //
    // 估高了是 OOM 直接崩，估低了只是多分段（慢）。所以往低了取——
    // 这条和 Scheduler 里那个 vram_estimate 的取舍是同一个道理。
    const double budget = profile.vram_gb > 0 ? profile.vram_gb * 0.9 : 0.0;

    SlotSpec spec;
    spec.slot = Slot::Image;
    spec.residency = Residency::Cached;   // 每个镜头都要，别反复卸
    // 估个数就行，真实占用由 sd.cpp 自己的预算管。这个数只用来决定
    // "再加载一个装不装得下"。
    spec.vram_estimate = static_cast<std::size_t>(budget * 1024) * 1024 * 1024;
    spec.evict_priority = 5;
    spec.load = [settings, budget] {
        auto ctx = SdContext::create(settings, budget);
        std::lock_guard lg(g_ctx_mu);
        g_image_ctx = std::move(ctx);
    };
    spec.unload = [] {
        std::lock_guard lg(g_ctx_mu);
        g_image_ctx.reset();   // 析构里 free_sd_ctx
    };
    scheduler().register_slot(std::move(spec));
}

std::shared_ptr<SdContext> current_image_context() {
    std::lock_guard lg(g_ctx_mu);
    return g_image_ctx;
}

}  // namespace changji::infer
