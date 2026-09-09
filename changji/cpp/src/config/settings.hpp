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

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace changji::config {

inline constexpr const char* kAppName = "changji";
inline constexpr const char* kEnvPrefix = "CHANGJI_";

/// 推理服务连接。默认本机，但可以指向任意一台机器。
struct ComfyConfig {
    std::string base_url = "http://127.0.0.1:8188";
    double timeout_s = 60.0;
    /// 提交后等待单个任务完成的上限。成片档一个镜头可能要好几分钟
    double job_timeout_s = 1800.0;
    int max_retries = 3;

    std::string ws_url() const;
    std::vector<std::string> validate() const;
};

/// 剧本和分镜用的大模型。默认走本地 Ollama。
struct LLMConfig {
    std::string base_url = "http://127.0.0.1:11434/v1";
    std::string model = "qwen3:14b";
    std::string api_key = "ollama";  ///< 本地服务通常不校验
    double timeout_s = 300.0;
    double temperature = 0.7;

    std::vector<std::string> validate() const;
};

/// 配音。
struct TTSConfig {
    /// comfy / http / local。
    ///
    /// **local 是 C++ 侧独有的取值**（进程内跑 Qwen3-TTS，要
    /// CHANGJI_LLAMA=ON 编出来的二进制）。Python 那边这个字段没有枚举校验，
    /// 填 local 它不会报错、只是认不出来然后退回估算后端——
    /// 也就是说这一项是**加法而不是破坏**：原有的两个取值行为一个字没变。
    /// 模型路径放在 [models].tts / [models].tts_decoder，
    /// 那一节本来就是 C++ 侧独有的（见 ModelsConfig 上面那段）。
    std::string backend = "comfy";
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
    /// 出图出片走哪个引擎："sd"（进程内 sd.cpp）或 "comfy"（外部 ComfyUI）。
    ///
    /// **放在 [models] 里而不是单开一节**，是因为这一节本来就是 C++ 侧独有的
    /// （Python 的 Settings 是 extra="forbid"，见方案的"配置隔离"）。
    /// C++ 独有的键集中在一处，两个后端各用一份配置时最容易分辨。
    ///
    /// 选 comfy 时视频一定走 ComfyUI；首帧要项目里有 workflows/image.json
    /// 才走它，没有就退回 sd.cpp——图像工作流是用户提供的，不能假定存在。
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
    std::string weights = "cpu";

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
    double image_cfg = 2.5;
    double image_flow_shift = 3.0;

    /// 首帧按哪个档位出：`draft`（默认）还是 `final`。**C++ 独有。**
    ///
    /// 默认草稿档，和 Python 一样（`spec = self.profile.tiers[Tier.DRAFT]`）。
    /// 但首帧是**跨镜头一致性的锚点**，而且会当起始图喂给出片那一步——
    /// 草稿档 352×640 的首帧配成片档 704×1280 的视频，等于把锚点放大两倍
    /// 再用。锚点糊了后面每一镜都糊。大卡上填 final。
    std::string frame_tier = "draft";

    /// `weights = "auto"` 时给**计算缓冲**留多少显存（GB）。**C++ 独有。**
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
    std::vector<std::string> endpoints;

    /// 协调者自己用哪张卡跑配音。**-1 表示不管**（保持现状）。
    ///
    /// 为什么要管：llama.cpp **不设 `CUDA_VISIBLE_DEVICES` 就默认把模型摊到
    /// 所有卡上**。实测 1.7B 的配音模型摊在 8 张 L20 上比只用 1 张
    /// **慢 39%**（8.99 秒 vs 6.46 秒）——卡间走 PCIe，没有 NVLink，
    /// 通信开销比省下的算力多。这不是"要不要开跨卡"，是"要不要关掉它"。
    ///
    /// 还有一层：协调者摊开会和每一个 worker 抢显存。多卡部署时通常
    /// 让它绑最后一张（worker 从 0 开始编号，最后一张最可能空着）。
    int gpu = -1;
};

/// 全部配置。
struct Settings {
    ComfyConfig comfy;
    LLMConfig llm;
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
