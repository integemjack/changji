// 配置。
//
// 对齐 Python 侧 src/changji/config.py，三条硬规则原样继承：
//
// 一，安装目录和数据目录彻底分开。程序装在哪都行，项目数据跟着项目走。
// 二，推理服务是一个 URL，不是一个假设。可以在本机，也可以在局域网另一台机器上。
// 三，任何路径都不写死。配置文件里的相对路径一律相对项目根解析。
//
// 优先级从高到低：环境变量、项目配置、用户全局配置、内置默认值。
//
// ---
//
// 关于校验：C++ 没有 pydantic 的等价物，采用的模式是每个结构体带一个
// validate()，返回错误列表而不是抛异常。
//
// 约束：这里的每一条校验都必须和 Python 侧的 field_validator 一一对应。
// 对拍时靠这个发现漏移植——少一条校验，C++ 就会接受 Python 拒绝的配置，
// 而这类差异不会立刻报错，只会在跑到一半时以奇怪的方式炸掉。

#pragma once

#include <utility>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace changji::config {

inline constexpr const char* kAppName = "changji";
inline constexpr const char* kEnvPrefix = "CHANGJI_";

/// 剧本和分镜用的大模型。默认走本地 Ollama。
/// 画质档位的显式覆盖。
///
/// **默认全 0 = 用按显存推出来的那套**（见 models/hardware.cpp 的档位表）。
///
/// 这一节 2026-09-10 加的。以前档位只能通过 `POST /api/settings` 改，而且
/// **有意不写回文件**——当时的理由是"档位是按显存推的，写死等于把这台机器
/// 的显存刻进配置"。那个理由站不住：`[models]` 里全是这台机器的模型路径，
/// 这个文件本来就是机器专属的。真实后果是**设完重启就丢**：用户把成片档
/// 调成 1280×704 跑了一集，重启之后回到 960×544，而界面上没有任何提示。
///
/// 填了就以填的为准，没填的项照旧按显存推。
/// 这部剧的画面规格。**放在项目目录的 changji.toml 里**，一部剧一份。
///
/// 用户要决定的是"竖屏还是横屏、720p 还是 2K"，不是"1280 还是 1440"。
/// 宽高由这两项算出来（`models::VideoSpec::size()`），中间那层换算不该
/// 甩给用户——填错一个不是 32 倍数的数，报错要到出图那一步才出现。
///
/// **为什么在项目上而不是全局**：一台机器上可以同时有竖屏短剧和横屏
/// 片子，画幅是这部剧的属性，不是这台机器的属性。全局那份
/// `[tiers]` 还在，作为没配项目时的回落。
struct VideoConfig {
    /// portrait / landscape
    std::string orientation = "portrait";
    /// 720p / 2k
    std::string quality = "720p";

    std::vector<std::string> validate() const;

    /// 算出宽高。两边都是 32 的倍数。
    std::pair<int, int> size() const;
};

struct TiersConfig {
    int draft_width = 0;
    int draft_height = 0;
    int draft_steps = 0;
    int final_width = 0;
    int final_height = 0;
    int final_steps = 0;

    std::vector<std::string> validate() const;
};

struct LLMConfig {
    /// 大模型跑在哪：`local`（默认，进程内）或 `remote`（走 base_url）。
    /// **C++ 独有**——Python 那边只有远端一条路。
    ///
    /// **默认内置**：那是"一个程序跑所有"的那条，不用另起 llama-server，
    /// 而且归调度器管——出片要显存时它按实时空闲显存决定要不要让开。
    /// 权重路径在 `[models].llm`。
    ///
    /// **remote 那条一直留着**，不是过渡方案：本机跑不动大模型的、
    /// 想用云上更强模型的、团队共用一台推理机的，都走它。设置页上能切。
    /// 编译时没带 llama.cpp 的话 local 会自动退回 remote 并在日志里说一声。
    std::string backend = "local";

    std::string base_url = "http://127.0.0.1:11434/v1";
    std::string model = "qwen3:14b";
    std::string api_key = "ollama";  ///< 本地服务通常不校验
    double timeout_s = 300.0;
    double temperature = 0.7;

    std::vector<std::string> validate() const;
};

/// 配音。
struct TTSConfig {
    /// `local`（进程内跑）或 `http`（外部配音服务）。
    ///
    /// **原来还有 comfy 那一档，2026-09-10 随 ComfyUI 一起拆了。**
    /// 老配置填 comfy 会被校验拦下并给出改法——静默退回估算后端的话，
    /// 整集会是静音的，而那要到装配完才发现。
    ///
    /// local 要 CHANGJI_LLAMA=ON 编出来的二进制，模型路径放
    /// [models].tts / [models].tts_decoder。
    std::string backend = "local";
    std::optional<std::string> base_url;  ///< backend 为 http 时必填
    std::string engine = "cosyvoice3";
    /// 台词时长与镜头时长的允许偏差。超出就要靠尾帧冻结或音频微调吸收
    double tolerance_s = 0.25;
    /// 音频变速的安全区。有口型的镜头收得更紧
    double max_tempo_shift = 0.03;

    std::vector<std::string> validate() const;
};

/// 质量闸门的阈值。全自动模式下这些数字决定了废片能不能被拦住。
struct GateConfig {
    bool enabled = true;
    // 闸门一：画面不能是纯色或噪点
    double min_pixel_std = 12.0;
    double min_pixel_mean = 8.0;
    double max_pixel_mean = 247.0;
    // 闸门二：与首帧的结构相似度下限
    double min_frame_similarity = 0.55;
    // 闸门三：台词落点与分镜的最大偏差
    double max_audio_drift_s = 0.15;
    double target_lufs = -16.0;
    double max_true_peak_db = -1.5;
    // 重试策略
    int max_attempts_per_shot = 3;
    /// 重试超限时降级为静帧加运镜，保证整集能出片
    bool fallback_on_exhausted = true;

    std::vector<std::string> validate() const;
};

/// 成片装配。
struct AssemblyConfig {
    int fps = 24;
    /// 统一编码规格。拼接环节最容易踩的坑就是各镜头规格不齐
    std::string pix_fmt = "yuv420p";
    std::string video_codec = "libx264";
    int crf = 18;
    std::string audio_codec = "aac";
    std::string audio_bitrate = "192k";
    /// loudnorm 内部按 192k 跑，不显式收回来的话编码器会挑一个
    /// 96k 之类的怪采样率。文件白白变大，有些平台还不收。
    int audio_sample_rate = 48000;
    int audio_channels = 2;
    /// 只在场景切换处用溶解，同场景内一律硬切
    double scene_transition_s = 0.4;
    /// 中文字幕单行上限，全角字符数
    int subtitle_max_chars_per_line = 15;
    int subtitle_max_lines = 2;
    std::string subtitle_font = "Source Han Sans SC";
    std::string ffmpeg_path = "ffmpeg";
    std::string ffprobe_path = "ffprobe";

    std::vector<std::string> validate() const;
};

/// 本地推理要用的模型文件。
///
/// Python 侧**没有**这一节——那边模型是 ComfyUI 自己管的，工作流 JSON 里
/// 按名字引用，changji 根本不知道文件在哪。进程内推理之后没有这一层了，
/// 得自己说清楚每个模型是哪个文件。
///
/// 为什么要可配置而不是写死一套默认文件名：同一个二进制要在配置差很多的
/// 机器上跑。Windows 上是 24G 显存的全尺寸模型，Pi 5 上只有 8G 内存，
/// 能装下的是完全不同的量化档。写死的话每台机器都得改代码重编，
/// 而"一个二进制到处跑"是这个后端存在的理由之一。
///
/// 路径规则：绝对路径原样用；相对路径相对 dir 解析。
/// dir 支持 ~ 展开，为空时回落到项目库根目录下的 models/。
struct ModelsConfig {
    /// 出图出片走哪个引擎。**现在只有 `"sd"`**（进程内 sd.cpp）。
    ///
    /// comfy 那一档 2026-09-10 拆了。这个字段留着是为了**认得出老配置**：
    /// 填 comfy 时校验会说清楚该怎么改，而不是让它悄悄跑成别的样子。
    std::string engine = "sd";

    /// 模型目录。相对路径的基准。
    std::optional<std::string> dir;

    /// 写剧本、拆分镜用的语言模型（llama.cpp，GGUF）。
    std::string llm;
    /// 文生视频主模型（sd.cpp，GGUF）。
    std::string video;
    /// 视频模型的 VAE。和主模型分开是因为它常常单独换。
    std::string video_vae;
    /// 文本编码器（UMT5-XXL 之类）。
    std::string video_text_encoder;
    /// 首帧生成与图像编辑。
    std::string image;
    /// 图像模型的 VAE。
    ///
    /// **不能复用 `video_vae`。** 之前这里就是复用的，因为图像那条路一直没
    /// 真跑过。Wan 的 VAE 和 Qwen-Image 的不是一回事，喂错了 sd.cpp 不会报错
    /// ——它照常加载，然后出一张和提示词没关系的图。
    /// 留空就退回 `video_vae`，好让只用 Wan 的人不用填两遍。
    std::string image_vae;
    /// 图像模型的文本编码器。
    ///
    /// **走的是 sd.cpp 的 `llm_path`，不是 `t5xxl_path`。** Qwen-Image-Edit
    /// 用 Qwen2.5-VL 当编码器，和 Wan 用的 UMT5-XXL 不是一类东西，
    /// 参数位置也不同（见 sd.cpp 的 docs/qwen_image_edit.md）。
    /// 留空就退回 `video_text_encoder`，那时仍按 t5xxl 传。
    std::string image_text_encoder;
    /// 文本编码器的视觉塔（mmproj），走 `llm_vision_path`。
    /// Qwen-Image-Edit 2509 及以后要它；初版可以不填。
    std::string image_text_encoder_vision;

    /// 进程内配音的骨干（Qwen3-TTS 的 talker，GGUF）。
    ///
    /// **和 llm 分开是因为它们是两个模型，不是一个模型的两种用法。**
    /// talker 只有 1.7B，写剧本那个是 14B；共用一个键的话，
    /// 换写剧本的模型会把配音一起换掉。
    std::string tts;
    /// 出图出片时开不开 flash attention。
    ///
    /// **默认开。** sd.cpp 的 `sd_ctx_params_init` 把它设成 false，
    /// 而方案第二节选 sd.cpp 的理由里就列着 `--diffusion-fa`——
    /// 上游 `docs/wan.md` 给 Wan 的命令行也是带着它的。
    /// 6 GB 卡上这一项直接影响塞不塞得下，不该靠用户自己想起来加。
    ///
    /// 留一个开关是因为它会改数值路径：万一某个后端上出问题，
    /// 关掉它比重编一个二进制容易。
    bool diffusion_flash_attn = true;

    /// 权重放哪：`cpu`（默认）还是 `auto`。**C++ 独有。**
    ///
    /// cpu：权重放系统内存，用到才搬进显存。6 GB 卡上能跑全靠它，
    /// 但每一步都在等 PCIe——8 张 L20 上实测每张卡 1 秒忙 2 秒闲，
    /// 利用率 35%，工作进程 CPU 60～80%。
    ///
    /// auto：交给 sd.cpp 的 auto_fit。它按**这张卡真实的空闲显存**逐个组件放，
    /// 装得下的留显存，装不下的才放内存。44 GB 卡上视频模型 + 编码器 + VAE
    /// 全能常驻。预算给的是物理显存，不是 vram_gb_override——那个数是拿来
    /// 挑档位的，和这张卡实际有多少显存是两回事。
    /// 取值三种：
    ///   `cpu`  —— 全放系统内存，用到才搬。小卡唯一的选择，但每一步都等 PCIe。
    ///   `auto` —— 交给 sd.cpp 的 auto_fit 按空闲显存决定。
    ///   **别的都当成 sd.cpp 的组件规格原样传给 params_backend**，
    ///     比如 `te=cpu,vae=cpu`：只把文本编码器和 VAE 的权重放内存，
    ///     扩散模型常驻显存。
    ///
    /// 第三种是给"差一点就装得下"的卡准备的。5090（32 GB）上 fp8 图像模型
    /// 20 GB 用 auto 会连编码器一起塞进显存（28 GB），剩下的挤不下
    /// 1280×704 那 6.6 GB 的 VAE 解码缓冲；而编码器只在采样前跑一次，
    /// 放内存几乎不影响速度。auto 自己推出来的也正是这个规格
    /// （日志里 `auto-fit: --params-backend "te=cpu,vae=cpu"`），
    /// 只是它推的时候预算已经被自己占掉了。
    ///
    /// **默认 `smart`（2026-09-10 起），按视频模型文件多大和这张卡多大算**——
    /// 见 weights_for。用户的原话："都应该让程序自己算。"写死一个 cpu 的
    /// 后果已经见过：换了大卡还在走慢路，而且没有任何提示。
    std::string weights = "smart";

    /// **图像模型**的权重放哪。取值和 `weights` 一样，外加 `smart`（默认）。
    ///
    /// **和 `weights` 分开是量出来的。** 2026-09-10 为了让 18 GB 的 MiniMax
    /// 视频模型跑起来把 `weights` 改成了 "cpu"（权重全放内存）——而它是
    /// 全局的一个旋钮，图像模型也跟着每一步从内存往显卡搬权重。
    /// 5090 上实测：出首帧的采样阶段 GPU 利用率平均 **18%**、最高 37%，
    /// 一步 6.8 秒；同一个模型让扩散权重常驻显存时是 82%、一步 1.25 秒。
    /// 慢五倍，而且看起来像是"显卡没吃满，要不要并发"——其实是 PCIe 在等。
    ///
    /// 出首帧和出片是两个阶段，中间调度器会把上一个模型卸掉，所以图像模型
    /// 常驻显存**不和视频模型抢地方**。
    ///
    /// `smart`：**看模型文件多大**再决定。fp8 的 Qwen-Image 是 20 GB，
    /// 常驻还要给 1280×704 的解码缓冲 6.6 GB、采样缓冲和别的上下文留的
    /// 余量再加 4 GB——32.6 GB 的卡按九成算是 29.3，装不下（实测第 34/62 段
    /// OOM，而且 2026-09-09 装下过的那次峰值 31.9 GB，只是运气）。
    /// Q6_K 16 GB、Q4 12 GB 就装得下。装得下才 "te=cpu,vae=cpu"，否则 "cpu"。
    std::string image_weights = "smart";

    /// 把 `image_weights` 的 smart 按这张卡和这个模型展开。
    /// `model_gb` 是图像模型文件的大小，拿不到就传 0（按装不下处理）。
    std::string image_weights_for(double vram_gb, double model_gb) const;

    /// 这一路跑起来时**真正要占的显存**（GB）：常驻权重 + 计算缓冲。
    ///
    /// 和上面两个 `*_for` 是同一个数的两面——它们拿这个数判断"装不装得下"，
    /// 这两个把它直接说出来。所以常数只有一份，改一处两边一起动。
    ///
    /// **给谁用的：调度器的实时显存那条路。** `SlotSpec::vram_estimate` 是
    /// 按整份预算估的（"同时只装得下一个"），保守但不真实：卡上真空着的时候
    /// 拿它去问"够不够"，答案永远是不够——预算就是整卡的九成，另一个槽一装上
    /// 就再也凑不出第二份。结果是每次点出片都把大模型卸掉，
    /// 而用户要的是"如果显存够就不用清理"。
    ///
    /// 这两个给的是老实数，专门用在那条问卡的分支上。
    /// `placement` 传展开后的规格（`weights_for` / `image_weights_for` 的返回值）。
    double video_live_vram_gb(const std::string& placement, double model_gb) const;
    double image_live_vram_gb(const std::string& placement, double model_gb) const;

    /// 大模型载进显存要占多少（GB）。权重全在显存（`use_gpu=true`），
    /// 再加 KV 缓存、CUDA 上下文和计算缓冲。
    ///
    /// 实测（5090）：Qwen3-14B-Q4_K_M 文件 9 GB，载进去 15.4 GB，
    /// 也就是权重之外还要 ~6.4 GB。
    ///
    /// **这个也得有，否则那条"够就不卸"是单向的。** 只给出图出片配老实数
    /// 的话：点出片时保住了大模型，可回头去写剧本，借 LLM 槽走的还是
    /// 整份预算那个估值——于是反过来把图像模型卸掉，两边来回踢。
    double llm_live_vram_gb(double model_gb) const;

    /// 采样的两个旋钮，按角色分开。默认值照抄 sd.cpp 上游文档的推荐命令行：
    /// docs/wan.md 给 Wan2.2 TI2V-5B 的是 `--cfg-scale 6.0 --flow-shift 3.0`，
    /// docs/qwen_image.md 给 Qwen-Image 的是 `--cfg-scale 2.5 --flow-shift 3`。
    /// 以前两条路都是 cfg 7.0、flow_shift 不设（sd.cpp 给 Wan 的内置默认是 5）。
    /// **C++ 独有**（Python 那边这些在 ComfyUI 工作流里）。
    double video_cfg = 6.0;
    double video_flow_shift = 3.0;

    /// 双专家视频模型的**高噪声**那一份。**C++ 独有**，Python 没有这条路。
    ///
    /// Wan 2.2 的 A14B 系列（T2V-A14B / I2V-A14B）是混合专家：高噪声专家
    /// 跑前几步定构图和运动，低噪声专家跑后几步出细节。`video` 填低噪声
    /// 那份，这里填高噪声那份。留空就是单模型，和以前一样。
    ///
    /// 换它的理由：TI2V-5B 是 Wan 家族里最小的一档，多家评测点名它的
    /// 动作质量不如专门的 14B 图生视频模型。
    ///
    /// **A14B 的推荐旋钮和 5B 不一样**：上游 docs/wan.md 给的命令行是
    /// cfg 3.5（5B 那边我们用 6.0）、flow_shift 3.0、euler。换模型时
    /// `video_cfg` 要跟着改，否则前几步会在一个完全不对的 cfg 上跑。
    std::string video_high_noise;

    /// 两个专家在哪个 sigma 交班。默认 0.875 是 sd.cpp 的默认值。
    ///
    /// sd.cpp 的分步逻辑：高噪声步数留 -1（我们就是这么传的）时，它扫一遍
    /// sigma 序列，第一个小于这个值的下标就是交班点。调大 = 高噪声专家
    /// 跑得更久（运动更大、细节更少），调小反之。
    /// `video_high_noise` 为空时这一项没有意义。
    double video_moe_boundary = 0.875;

    /// 视频模型的 **LLM 类**文本编码器（sd.cpp 的 `llm_path`）。**C++ 独有。**
    ///
    /// `video_text_encoder` 走的是 `t5xxl_path`（Wan 那一路的 UMT5-XXL）。
    /// MiniMax-H3 这类用大语言模型当编码器的（它要裁过的 Qwen3-VL-32B）
    /// 在 sd.cpp 里是**另一个参数位**，填错了不报错——照常加载，
    /// 然后出一段和提示词没关系的片。填了这一项就不再填 t5xxl。
    std::string video_llm;

    /// `video_llm` 的视觉塔单独存放时填这里（sd.cpp 的 `llm_vision_path`）。
    /// H3 的 Qwen3-VL 视觉塔通常已经在同一份权重里，那就不用填。
    std::string video_llm_vision;

    /// 音频 VAE（sd.cpp 的 `audio_vae_path`）。**C++ 独有。**
    ///
    /// 给 MiniMax-H3 这种画面和声音一起生成的模型用。不填的话联合扩散
    /// 照跑，但出来的片没有解码好的音轨（上游 docs/minimax_h3.md 写的）。
    std::string video_audio_vae;

    /// 出片这条路用哪种随机数发生器。**C++ 独有。** 取值 cuda / cpu / std。
    ///
    /// sd.cpp 的默认是 `cuda`，Wan 那一路就用它。但**上游给 MiniMax-H3 的
    /// 命令行明写着 `--rng cpu`**（docs/minimax_h3.md）——不同的发生器
    /// 出的初始噪声不一样，同一个种子出来的画面就不一样，而且不报错。
    /// 只影响出片那条上下文，出图那条不动（改了会连首帧一起变）。
    std::string video_rng = "cuda";

    /// 出片挂一个 LoRA（相对 `dir` 或绝对路径）。**C++ 独有。**
    ///
    /// 用途是 Turbo 那类蒸馏适配器：MiniMax-H3 的 Turbo LoRA 能把采样从
    /// 28 步压到 6 步，约 5 倍。挂上之后**步数要跟着改**（走
    /// `POST /api/settings {"final_steps":6}` 或档位设置），不改的话
    /// 白挂——28 步跑 Turbo 只会更糊，资料说超过 8 步就开始过锐。
    ///
    /// 留空就是不挂。sd.cpp 有自己的张量名转换，**认不认这类给 ComfyUI
    /// 做的 LoRA 要实测**：不认时它只是加载不上，画面照出，所以判据得看
    /// 耗时有没有真的降下来，不能只看"没报错"。
    ///
    /// **默认就指着 Turbo 那份。** 用户 2026-09-10：“都使用 turbo 加速”。
    /// 文件不在就当没配（日志里说一声），不报错——没下过 LoRA 的机器
    /// 照样能出片，只是慢。
    std::string video_lora = "loras/minimax_h3_turbo_v4_step600_ema.safetensors";

    /// 上面那个 LoRA 的权重。1.0 是原样，调低减弱它的影响。
    double video_lora_strength = 1.0;

    /// 上面那个 LoRA 挂在哪些档位：`draft` / `final` / `both`（默认）。
    ///
    /// Turbo 那类蒸馏 LoRA 是拿画质换速度的，所以**天然适合只挂草稿档**：
    /// 草稿是用来看叙事和构图的，6 步足够；成片档想要的是最好的画面，
    /// 那就让它跑满步数、不挂 LoRA。
    /// 挂在哪一档，步数就要在哪一档跟着改（草稿 6 步、成片 48 步这种）。
    std::string video_lora_tiers = "both";

    /// 出片时 VAE 解码的分块大小（潜空间格子）。**C++ 独有。** 0 = 用内置的 16×11。
    ///
    /// 调小它换的是显存：每块的计算缓冲小一点，剩给权重的就多一点。
    ///
    /// **5090 + MiniMax-H3 上这个旋钮救不了场，别指望它。** 那台机器上
    /// VAE 放内存解码 71 秒（每次把 5.5 GB 搬过 PCIe），放显存只要 8 秒，
    /// 很想让它常驻；但放显存时 VAE 解码会失败，而且**从 16×11 调到 10×10
    /// 一点没变**。日志说明了原因：
    ///
    ///     cannot make enough memory available on CUDA0:
    ///       need 142.17 MB device / 638.80 MB budget,
    ///       available 29.75 MB device / 7248.04 MB budget
    ///
    /// **预算还剩 7.2 GB，显卡上只剩 29.75 MB。** 缺的不是分块的缓冲，是
    /// 扩散模型的计算缓冲一直占着没还——分块再小也腾不出那块地方。
    /// 留着这一项是给别的卡和别的分辨率用的。
    ///
    /// 太小会变慢（块多了重叠部分重复算），也别调到 0 以下。
    int video_vae_tile = 0;

    /// `weights = "smart"` 时，显存到多少才把 VAE 放显存（GB）。**C++ 独有。**
    ///
    /// 默认 40 是量出来的，不是拍的。5090（32.6 GB）跑 MiniMax-H3：
    /// 扩散模型 17.9 GB + VAE 5.5 GB = 23.4 GB 权重，加上扩散模型
    /// 自己那块约 9 GB 的计算缓冲就是 32.4 GB，**差 112 MB 装不下**
    /// （日志：`need 142.17 MB device, available 29.75 MB device`）。
    /// 40 GB 的卡才留得出余量，所以门槛定在这儿。
    ///
    /// 这笔账值不值得：VAE 放内存时每镜解码 71 秒（5.5 GB 搬过 PCIe），
    /// 放显存只要 8 秒——一镜省 63 秒。所以大卡上一定要放进去。
    double vae_vram_min_gb = 40.0;

    /// 按这张卡的显存把 `weights` 展开成 sd.cpp 认的组件规格。
    ///
    /// 只有 `"smart"` 需要展开，别的取值原样返回。
    /// `model_gb` 是视频扩散模型文件的大小；拿不到传 0，按装不下处理。
    ///
    /// 账（5090 上量的）：1280×704 的计算缓冲 ~14.6 GB——H3 18.8 GB 常驻时
    /// 差 788 MB 装不下就是这个数（32.6 − 18.8 = 13.8，还差 0.8）。缓冲已含
    /// 驱动余量，直接和整卡显存比。VAE 也常驻再加 5.5 GB。
    ///   32.6 GB + H3 18.8：18.8 + 14.6 = 33.4 > 32.6 → "cpu"（和之前手填的一样）
    ///   80 GB：18.8 + 14.6 + 5.5 = 38.9 ≤ 80 且 ≥ 40 → "te=cpu"（只文本编码器在内存）
    std::string weights_for(double vram_gb, double model_gb) const;
    double image_cfg = 2.5;
    double image_flow_shift = 3.0;

    /// 首帧按哪个档位出：`final`（默认）还是 `draft`。**C++ 独有。**
    ///
    /// **默认从 draft 改成 final（2026-09-10）。** 原来跟 Python 一致
    /// （`spec = self.profile.tiers[Tier.DRAFT]`），但那个默认现在会出错：
    /// 画幅搬到项目上之后，`[video]` 只盖成片档的宽高——草稿档还是档位表
    /// 里那个 512×288。于是新用户什么都不配就得到 512×288 的首帧配
    /// 704×1280 的视频。
    ///
    /// 首帧是**跨镜头一致性的锚点**，而且会当起始图喂给出片那一步。
    /// 锚点糊了后面每一镜都糊，而且这件事**不会报错**——出来的片子只是
    /// "看着不太行"，人会先去怀疑模型和提示词。
    ///
    /// 小卡上显存不够可以填 draft，那是有意的取舍。
    std::string frame_tier = "final";

    /// 首帧出几步。**0 = 跟 `frame_tier` 那一档的步数走。**
    ///
    /// 单独有这一项是因为**成片档的步数会被 Turbo LoRA 压到 6**，
    /// 而那个 LoRA 只挂在视频模型上——出图那一步没有它。首帧现在默认
    /// 也走成片档（为了画幅），步数要是一起跟过去，图像模型就变成 6 步
    /// 裸跑，出来的首帧糊。首帧是跨镜头一致性的锚点，糊了后面每一镜都糊。
    ///
    /// 平时不用填：`run.cpp` 在压步数之前会把原来那个数存进来。
    /// 填了就以填的为准。
    int frame_steps = 0;

    /// `weights = "auto"` 时给**计算缓冲**留多少显存（GB）。**C++ 独有。**
    ///
    ///
    /// auto 的预算是给权重的；生成时还要一块计算缓冲，那块比"给驱动留一成"
    /// 大一个量级。5090（32 GB）上出 1280×704 的首帧实测：VAE 解码要
    /// 6576 MB，而权重按 0.9×32=28.8 GB 全塞进去之后只剩 3447 MB，
    /// 于是 `decode_first_stage failed`——**22 个镜头全失败，而且在接上
    /// sd.cpp 的日志之前只报"出图失败，看一眼上面 sd.cpp 打的日志"。**
    ///
    /// 默认 6：上面那个数取整。大卡上减掉也无所谓（48 GB 减完还有 37 GB，
    /// 两套权重本来就装得下）；32 GB 上减完 23 GB，图像模型仍然常驻，
    /// 编码器有一部分回内存，慢一点但出得来图。
    /// 出更大的图要调大它——缓冲随分辨率涨。
    double vram_reserve_gb = 6.0;

    /// 配音的解码器（Qwen3-TTS 的 tokenizer，GGUF）。
    ///
    /// 这一份把 12.5 Hz 的码本还原成 24 kHz 波形。**必须和 tts 配套**，
    /// 拿错了 mtmd 会报"这份 mmproj 不支持音频生成"。
    std::string tts_decoder;

    /// 把一个配置项解析成绝对路径。空字符串返回空路径。
    ///
    /// workspace 用来算 dir 的回落值，所以要传进来——
    /// 这个函数不该自己去读全局配置。
    std::filesystem::path resolve(const std::string& entry,
                                  const std::filesystem::path& workspace) const;

    /// 模型目录的绝对路径。
    std::filesystem::path dir_path(const std::filesystem::path& workspace) const;

    /// 只校验形状，**不检查文件存在**。
    ///
    /// 存在性交给 doctor。理由是模型动辄几个 G，用户装好程序还没下模型是
    /// 常态；那时候如果配置加载直接失败，连界面都进不去，也就没法在界面里
    /// 看到"缺哪个文件"。doctor 报缺失，程序照常起来。
    std::vector<std::string> validate() const;
};

/// 工作进程。**空 = 进程内，行为和以前一模一样。**
///
/// 填了地址就把出图和出片派给那些进程去算。为什么要这样见方案
/// 「多卡和多机怎么用起来」：sd.cpp 的进度回调是全局的，
/// 同进程两个生成会互相串——同种模型的多实例只能靠多进程。
///
/// 这一节和 [models] 一样是 C++ 侧独有的，Python 的 Settings
/// 是 extra="forbid"，出现它就起不来。
struct WorkersConfig {
    /// `["http://127.0.0.1:9001", "http://127.0.0.1:9002"]`。
    /// 跨机就填别的机器的地址，协议一模一样。
    ///
    /// **本机多卡不用填**：留空时主进程会按显卡数自己拉起（见 auto_spawn）。
    std::vector<std::string> endpoints;

    /// 本机多卡时自己拉起每张卡一个工作进程。**C++ 独有，默认开。**
    ///
    /// "一个程序运行所有"不该等于"只能用一张卡"：用户启动的仍然是一个
    /// 命令，多卡编排由它自己做。只在 `endpoints` 为空、探到多于一张卡时
    /// 才动手；一张卡就进程内跑，少一次进程间的图片搬运。
    ///
    /// 关掉它就是老行为：要么单卡进程内，要么自己起工作进程并填 endpoints。
    bool auto_spawn = true;

    /// 自己拉起时，第 i 张卡用 `base_port + i`。
    ///
    /// 默认 9001 和手册里那套 systemd 模板对得上，两种起法能混着用。
    int base_port = 9001;

    /// 协调者自己用哪张卡跑配音。**-1 表示不管**（保持现状）。
    ///
    /// 为什么要管：llama.cpp **不设 `CUDA_VISIBLE_DEVICES` 就默认把模型摊到
    /// 所有卡上**。实测 1.7B 的配音模型摊在 8 张 L20 上比只用 1 张
    /// **慢 39%**（8.99 秒 vs 6.46 秒）——卡间走 PCIe，没有 NVLink，
    /// 通信开销比省下的算力多。这不是"要不要开跨卡"，是"要不要关掉它"。
    ///
    /// 还有一层：协调者摊开会和每一个工作进程抢显存，所以要绑住。
    ///
    /// **`auto_spawn` 打开时每张卡都有工作进程**，没有"空着的那张"可绑
    /// ——绑哪张都会和那张上的工作进程共用。这不算问题：主进程自己的
    /// 活（写剧本、配音）和渲染不在同一阶段，而且调度器会按**实时空闲
    /// 显存**决定要不要让开（见 Scheduler::set_free_vram_probe）。
    /// 绑住的意义仍然在于**别让 llama.cpp 摊到所有卡上**。
    ///
    /// 手动起工作进程、故意留一张卡的话，那就绑那张。
    int gpu = -1;
};

/// 全部配置。
struct Settings {
    LLMConfig llm;
    TiersConfig tiers;
    VideoConfig video;
    TTSConfig tts;
    GateConfig gates;
    AssemblyConfig assembly;
    ModelsConfig models;
    WorkersConfig workers;

    /// 显存覆盖。推理服务在别的机器上时本机探测不到，用它手动指定
    std::optional<double> vram_gb_override;
    /// 项目库根目录。为空则用系统标准数据目录
    std::optional<std::string> workspace;

    std::filesystem::path workspace_path() const;
    std::vector<std::string> validate() const;
};

/// 用户全局配置的位置。跨平台。
std::filesystem::path user_config_path();

/// 按优先级合并配置：环境变量 > 项目配置 > 用户全局配置 > 默认值。
///
/// 配置文件解析失败会抛 std::runtime_error，消息里带上是哪个文件——
/// 「配置坏了」而不指出哪一份，用户只能挨个翻。
Settings load_settings(const std::optional<std::filesystem::path>& project_dir = std::nullopt);

/// 这一轮实际会用的规格：画幅、出片步数、首帧步数。
///
/// **抽出来是因为它有两个读者，而它们必须给出同一个数。** 一个是
/// `run.cpp`（真去跑的那条路），一个是设置页（`/bff/settings/overview`）。
/// 各算各的必然会分叉，而分叉的表现是**界面上的数字和实际跑的不是一回事**：
/// 2026-09-10 就是这样——设置页照着档位表显示"成片步数 28"，
/// 而每一镜实际跑的是 Turbo 的 6 步。用户于是问"怎么没用 turbo"。
///
/// 显示错的数字比不显示更糟：它会让人去调一个根本没生效的东西。
struct EffectiveSpec {
    int width = 0;         ///< 成片画幅，来自项目的 [video]
    int height = 0;
    int final_steps = 0;   ///< 出视频跑几步
    int frame_steps = 0;   ///< 出首帧跑几步。**和上面不是一个数**
    bool turbo = false;    ///< 挂上 Turbo LoRA 了没有（文件真的在）
    /// 步数是用户在 [tiers].final_steps 里写死的（那样 Turbo 不改它）
    bool steps_pinned = false;
};

/// 按配置和档位表算出这一轮真正会用的规格。
///
/// `table_final_steps` 是档位表推出来的成片步数（`HardwareProfile` 里那个）。
/// 这一层不认识 HardwareProfile——config 不该反过来依赖 models。
EffectiveSpec effective_spec(const Settings& s, int table_final_steps);

/// 把已经拆掉的老取值换成现在的，返回每一处换了什么。
///
/// **拆掉一条路之后，老配置不能让程序起不来。** 拆 ComfyUI 时只在
/// `validate()` 里加了迁移说明，于是升级上来的用户遇到的是：程序直接退出，
/// 往 stderr 打一句"已经不支持了"。双击启动的人连那句都看不到。而那句话
/// 让他去改的 toml 文件，恰恰是他多半不知道在哪的那个——他本来会去设置页
/// 改，可设置页就是这个进程发的，起不来就打不开。
///
/// 只处理**有唯一像样去处**的取值（comfy → local / sd）。拿不准的仍然
/// 交给 `validate()` 去拦：猜错一个地址比起不来更糟。
///
/// `load_settings` 会自己调一遍并把返回的话打到 stderr。单独暴露是为了能测。
std::vector<std::string> migrate_legacy(Settings& s);

/// 哪些设置正被环境变量顶着。
///
/// 环境变量优先级最高。容器里用 compose 注入地址是常态，这时候在界面上
/// 改配置文件是没用的，重启还是环境变量那一套。界面必须把这件事说出来，
/// 否则用户会以为程序没保存。
///
/// 返回 {"comfy_base_url": "CHANGJI_COMFY_BASE_URL"} 这样的映射。
std::map<std::string, std::string> env_overridden();

/// 那份带注释的配置模板的原文。
///
/// 导出来是为了让用例**拿代码认的值去查它**：模板是注释，写漏一个后端
/// 或一个模型键，解析照样过、断言照样绿，而用户照着它配就永远不知道
/// 有那条路。漏 `[tts] backend = "local"` 那次，代价是把人推去装
/// 34 GB 的 ComfyUI，而这个二进制自己就能出声。
std::string default_config_template();

/// 生成一份带注释的配置模板。首次安装时用。
std::filesystem::path write_default_config(
    const std::optional<std::filesystem::path>& path = std::nullopt);

}  // namespace changji::config
