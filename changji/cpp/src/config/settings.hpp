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
    std::string backend = "comfy";  ///< comfy 或 http
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

/// 全部配置。
struct Settings {
    ComfyConfig comfy;
    LLMConfig llm;
    TTSConfig tts;
    GateConfig gates;
    AssemblyConfig assembly;
    ModelsConfig models;

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

/// 生成一份带注释的配置模板。首次安装时用。
std::filesystem::path write_default_config(
    const std::optional<std::filesystem::path>& path = std::nullopt);

}  // namespace changji::config
