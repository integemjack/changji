#pragma once

// 用 sd.cpp 出图。
//
// 这一层是**阶段 5 的核心**，也是模型调度器真正派上用场的地方：
// sd_ctx 的建立和销毁挂在 Scheduler 的槽位上，跨阶段的显存回收由它决定。
//
// ---
//
// 和 sd_backend.hpp 一样，这个头文件**不 include sd.cpp 的头，也不带 #ifdef**。
// 没链 sd.cpp 时 generate() 抛一个说人话的异常，调用方照常写。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <map>
#include <string>
#include <vector>

#include "config/settings.hpp"
#include "infer/scheduler.hpp"
#include "models/hardware.hpp"
#include "pipeline/jobs.hpp"

namespace changji::infer {

class SdError : public std::runtime_error {
public:
    explicit SdError(const std::string& what) : std::runtime_error(what) {}
};

/// 一次出图的参数。
struct ImageRequest {
    std::string positive;
    std::string negative;
    int width = 512;
    int height = 512;
    int steps = 20;
    /// 种子。**必须显式给**，不能让它随机——重跑同一个镜头要能得到
    /// 同一张图，否则"重试"和"换一张"就分不清了。
    std::int64_t seed = 0;
    /// **不再生效。** cfg 按角色从 [models].image_cfg / video_cfg 来，
    /// 留着这个字段只是不想动所有构造点。
    double cfg = 7.0;
    /// 参考图的绝对路径。图像编辑模型那条路会用，纯文生图忽略。
    std::vector<std::filesystem::path> reference_images;

    /// VAE 解码分块。**出片那条路早就有，出图这条路以前一个字没填**——
    /// 于是 sd.cpp 走的是整图解码：1280×704 一次要 6.6 GB 计算缓冲，
    /// fp8 扩散模型 19.5 GB 常驻之后 32 GB 的卡挤不出来，报
    /// `vae decode compute failed`（分块那条路的文案是"…while processing
    /// a tile"，看文案就能分辨走的哪条路）。块大小按潜空间算，和出片那边
    /// 一致：16×11 个潜空间格子、重叠 1/4。
    bool vae_tiling = true;
    int vae_tile_x = 16;
    int vae_tile_y = 11;
    double vae_tile_overlap = 0.25;

    /// 这一次生成是给哪一镜的（shot_id）。**只给预览用**：sd.cpp 的预览回调
    /// 是全局的，靠它才知道推上来的小图该挂在墙上哪一格。空就不推预览。
    std::string tag;
};

/// 每一步的进度。
///
/// 采样一步在低配机器上要好几秒，不报的话界面上就是一条几分钟不动的进度条，
/// 用户分不清是在跑还是卡死了。
/// 出图过程中的进度回调。
///
/// `loading = true` 表示这一下报的是**加载权重**，不是采样。
/// sd.cpp 那个回调两件事都会调，不分开的话用户会看到
/// "第 1927/1927 步"（加载 1927 个张量）紧接着"第 1/8 步"（真的采样），
/// 像是跑到头又倒回去了——而 1927 这个数对他没有任何意义。
using StepCallback =
    std::function<void(int step, int total, double seconds, bool loading)>;

/// 采样中途的预览图。
///
/// `tag` 是请求里带的 shot_id；`data_url` 是一张 `data:image/png;base64,…`
/// 的小图——潜空间分辨率（704×1280 出来是 88×160），由 sd.cpp 的
/// PREVIEW_PROJ 把潜空间线性投影成 RGB 得来，**不走 VAE**，几乎不花时间。
/// 放大到格子大小自然是糊的，随着步数推进内容逐渐成形。
using PreviewSink =
    std::function<void(const std::string& tag, int step, std::string data_url)>;

/// 装（或传空函数卸）预览的落点。**进程一个**，跟 sd.cpp 的回调一样。
/// 没装或请求里没带 tag，就不编码也不推——一张不要的 PNG 也别编。
void set_preview_sink(PreviewSink sink);

/// 一次出视频的参数。
struct VideoRequest {
    std::string positive;
    std::string negative;
    int width = 448;
    int height = 768;
    int steps = 8;
    int frames = 49;
    int fps = 24;
    std::int64_t seed = 0;
    /// **不再生效。** cfg 按角色从 [models].image_cfg / video_cfg 来，
    /// 留着这个字段只是不想动所有构造点。
    double cfg = 7.0;
    /// 首帧。跨镜头一致性全靠它，没有的话退化成纯文生视频。
    std::optional<std::filesystem::path> start_image;

    /// VAE 分块解码。**6GB 卡上出视频的必要条件**，不是可选的优化。
    ///
    /// 实测（verify/RESULTS.md 三点八，RTX 2060 6GB，Wan 2.2 TI2V-5B，
    /// 640x352 9 帧）：
    ///
    /// | 配置 | VAE 解码所需显存 | 结果 |
    /// |---|---|---|
    /// | 不分块 | 11747 MB | 失败（可用 5081 MB）|
    /// | 默认 32x32 分块 | 9610 MB | 失败 |
    /// | 再开时间维分块 | 10321 MB | 失败，**反而更高** |
    /// | 16x11、重叠 0.25 | — | 成功，44.7 秒 |
    ///
    /// 大显存的机器为它多付一点解码时间。反过来关掉的话，这台机器上
    /// 一段视频都出不来——而这个项目的立项理由就是"6GB 卡跑 16GB 模型"。
    bool vae_tiling = true;

    /// 分块大小，**单位是潜变量格子不是像素**。
    ///
    /// 用绝对值不用比例，是为了让每块的显存占用不随分辨率变：
    /// 比例分块在换到成片档分辨率时每块也跟着变大，峰值显存照样爆。
    /// 潜变量比这个还小的时候 sd.cpp 自己会夹住（退化成不分块），
    /// 那种情况本来也不需要分块。
    ///
    /// 默认 32x32 为什么不行：碰上 40x22 的潜变量切出来是 2x1 块、
    /// 重叠率 0.75——两块几乎完全重叠，只降了 18%。
    int vae_tile_x = 16;
    int vae_tile_y = 11;
    double vae_tile_overlap = 0.25;

    /// 时间维分块。**默认关**：9 帧只压成 3 个潜变量帧，而分块粒度是 4，
    /// 切不动还引入有状态分块自身的开销，实测显存从 9610 涨到 10321 MB。
    bool vae_temporal_tiling = false;

    /// 这一次要不要挂 [models].video_lora。**按档位分**：Turbo 那类
    /// 蒸馏 LoRA 拿画质换速度，草稿档挂着划算，成片档不该挂。
    /// 上下文是两档共用的，所以只能在每次请求上决定，不能在建上下文时定。
    bool use_lora = true;

    /// 同 ImageRequest::tag。视频取第一帧做预览。
    std::string tag;
};

/// 这个上下文装的是哪个模型。
///
/// 图像和视频是**两个不同的模型**（首帧用图像编辑模型，视频用 Wan），
/// 各自一个 sd_ctx、各自一个调度槽。合成一个的话，跑首帧时视频模型
/// 也占着显存，而 6GB 卡上那意味着两个都装不下。
enum class ModelRole {
    Image,
    Video,
};

/// 这份配置能不能拿来出图 / 出片。能就返回空串，不能就返回一句人话。
///
/// **纯判断，和有没有链上 sd.cpp 无关**，所以放在 `#ifdef` 外面——
/// 测试目标编的是没链上游那一支，判断逻辑要是写在 `#ifdef` 里面就测不到。
/// 和 CMakeLists 里说的"纯判断拆出来才测得到"是同一个理由。
///
/// 最要紧的一条：**没配 image 时不许拿 video 顶替**。
/// 2026-09-08 实测那么干会让整个进程崩掉（0xc0000094 整数除零），
/// 因为 `sd_img_gen_params_t` 没有 video_frames 字段，
/// 压根没法告诉 generate_image 出几帧。
std::string sd_model_problem(const config::Settings& settings, ModelRole role);

/// 中心裁剪矩形，单位像素。
struct CropBox {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    /// 不用裁：矩形就是整张图。
    bool whole(int src_w, int src_h) const {
        return x == 0 && y == 0 && w == src_w && h == src_h;
    }
};

/// 把 src 按 dst 的**长宽比**中心裁一刀。只裁不缩，尺寸交给下游。
///
/// **为什么要有这一步。** 出片时首帧是当 `init_image` 传给 sd.cpp 的，
/// 而它内部走 `sd_image_to_tensor(img, 目标宽, 目标高)`——那个函数在
/// 尺寸不一致时直接 `interpolate` 到目标，**不管长宽比**。也就是说
/// 喂一张比例不同的首帧进去，出来的片是被拉扁或拉长的，
/// 而且**不报错也不打日志**，只是人脸变宽了一点。
///
/// 这条路是真会走到的：首帧和成片是分开的两个动作，用户完全可以
/// 只重出成片、留着以前的首帧。2026-09-10 把画幅从 704×1280 改成
/// 544×928，两者比例 0.550 对 0.586，差 6%——够看出来了。
///
/// 先中心裁到目标比例，再让 sd.cpp 去缩，出来的就是正的。
/// 裁掉的是长边两头，构图中心不动。
///
/// 比例本来就一样（含尺寸完全相同）时返回整张图，
/// 让调用方能靠 `whole()` 跳过拷贝。
CropBox center_crop_box(int src_w, int src_h, int dst_w, int dst_h);

/// 实测显存的落盘格式：把 `{槽名: 字节数}` 转成一行 JSON，和反过来。
///
/// **单独拆出来是为了能测。** 读写文件那半没法在单元测试里跑，
/// 但"字段名对不对、坏数据会不会让程序崩"是能测也必须测的——
/// 这个文件在两次运行之间保存的是**决定要不要卸模型的依据**，
/// 解析出错的代价是要么白卸（慢），要么不该不卸（OOM）。
std::string serialize_measured_vram(
    const std::map<Slot, Scheduler::Measured>& m);

/// 坏行、缺字段、负数、不认得的槽名，一律**跳过那一条**，不影响别的。
/// 整个文件解不开就返回空——那等于"没量过"，回到保守那条，安全。
std::map<Slot, Scheduler::Measured> parse_measured_vram(const std::string& text);

/// sd.cpp 的上下文。**贵**：建一次要解析模型文件、建张量图、分配运行时缓冲。
///
/// 所以它不是每次出图新建一个，而是挂在调度器的槽位上复用。
/// 一集四十个镜头：复用是建一次用四十次，不复用是建四十次，
/// 在 6GB 卡上后者慢到不可用。
class SdContext {
public:
    /// 按配置建一个。模型路径从 [models] 来。
    ///
    /// vram_budget_gb 是给 sd.cpp 的 max_vram：0 表示"用当前空闲显存，
    /// 不设显式预算"。**这个数不是显卡有多少**，是留给推理多少。
    static std::shared_ptr<SdContext> create(const config::Settings& settings,
                                             double vram_budget_gb,
                                             ModelRole role);

    ~SdContext();
    SdContext(const SdContext&) = delete;
    SdContext& operator=(const SdContext&) = delete;

    /// 出一张图，写到 dest（PNG）。
    ///
    /// tok 会在每一步的回调里查。sd.cpp 的采样循环中途打得断
    /// （sd_cancel_generation），所以点了停止不用等这一镜跑完。
    void generate(const ImageRequest& req, const std::filesystem::path& dest,
                  pipeline::CancelToken& tok, const StepCallback& on_step);

    /// 出一段视频，把**裸 RGB24 帧**顺序写到 raw_dest。
    ///
    /// 不在这里编码成 mp4：编码是 ffmpeg 的事，而这一层不该知道
    /// 编码参数从哪儿来。见 sd_video.hpp。
    void generate_video(const VideoRequest& req,
                        const std::filesystem::path& raw_dest,
                        pipeline::CancelToken& tok,
                        const StepCallback& on_step);

private:
    SdContext() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 取配置的回调。**每次加载模型时现取**，不是注册时取一次。
///
/// 这条是有代价学来的：LLM 客户端当初在构造时把 settings.llm 存了下来，
/// 结果用户在设置页改了地址、接口回"已保存"，而请求还是发往老地址。
/// 模型文件这一路同样——改完 [models] 里的文件名之后不重启就不生效，
/// 而"不生效"的表现是加载出来的还是上一个模型，不报任何错。
using SettingsProvider = std::function<config::Settings()>;

/// 把 sd.cpp 的上下文注册到调度器的图像槽和视频槽上。
///
/// 注册之后调用方只管 `scheduler().acquire(Slot::Image)`，
/// 什么时候加载、什么时候为了腾地方被卸掉，由调度器决定。
///
/// **一个进程注册一次**，在启动时做。槽已经加载着的时候重新注册会抛异常
/// （新的 unload 会去卸一个不是它加载的东西），所以别放在每次开跑的路径上。
void register_sd_slots(SettingsProvider provider,
                       const models::HardwareProfile& profile);

/// 同上，配置固定不变的那种。测试和命令行用。
void register_sd_slots(const config::Settings& settings,
                       const models::HardwareProfile& profile);

/// 当前挂在图像槽 / 视频槽上的上下文。没加载时返回空。
///
/// 拿它之前要先 acquire 对应的槽，否则可能拿到一个正要被卸掉的。
std::shared_ptr<SdContext> current_image_context();
std::shared_ptr<SdContext> current_video_context();

}  // namespace changji::infer
