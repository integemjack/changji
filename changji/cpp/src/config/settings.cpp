#include "config/settings.hpp"

#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

#include <toml++/toml.hpp>

#include "util/paths.hpp"

namespace changji::config {

namespace fs = std::filesystem;

// ---- 校验 ----
//
// 每一条都对应 Python 侧 config.py 里的一个 Field 约束或 field_validator。
// 改这里的时候必须同步改那边，反之亦然，直到 Python 删除为止。

namespace {

/// 去掉首尾空白和末尾斜杠。对应 Python 的 _strip_slash。
std::string strip_trailing_slash(std::string v) {
    size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = v.find_last_not_of(" \t\r\n");
    v = v.substr(b, e - b + 1);
    while (!v.empty() && v.back() == '/') v.pop_back();
    return v;
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

void check_range(std::vector<std::string>& errs, const char* name,
                 double v, double lo, double hi) {
    if (v < lo || v > hi) {
        std::ostringstream os;
        os << name << " 必须在 " << lo << " 到 " << hi << " 之间，当前是 " << v;
        errs.push_back(os.str());
    }
}

void check_gt(std::vector<std::string>& errs, const char* name, double v, double lo) {
    if (v <= lo) {
        std::ostringstream os;
        os << name << " 必须大于 " << lo << "，当前是 " << v;
        errs.push_back(os.str());
    }
}

void check_ge(std::vector<std::string>& errs, const char* name, double v, double lo) {
    if (v < lo) {
        std::ostringstream os;
        os << name << " 不能小于 " << lo << "，当前是 " << v;
        errs.push_back(os.str());
    }
}

}  // namespace

std::string ComfyConfig::ws_url() const {
    std::string u = base_url;
    if (starts_with(u, "http://")) return "ws://" + u.substr(7) + "/ws";
    if (starts_with(u, "https://")) return "wss://" + u.substr(8) + "/ws";
    return u + "/ws";
}

std::vector<std::string> ComfyConfig::validate() const {
    std::vector<std::string> errs;
    if (!starts_with(base_url, "http://") && !starts_with(base_url, "https://")) {
        errs.push_back("推理服务地址必须以 http:// 或 https:// 开头");
    }
    check_gt(errs, "comfy.timeout_s", timeout_s, 0);
    check_gt(errs, "comfy.job_timeout_s", job_timeout_s, 0);
    check_ge(errs, "comfy.max_retries", max_retries, 0);
    return errs;
}

std::vector<std::string> LLMConfig::validate() const {
    std::vector<std::string> errs;
    check_gt(errs, "llm.timeout_s", timeout_s, 0);
    check_range(errs, "llm.temperature", temperature, 0.0, 2.0);
    return errs;
}

std::vector<std::string> TTSConfig::validate() const {
    std::vector<std::string> errs;
    if (backend != "comfy" && backend != "http" && backend != "local") {
        errs.push_back("tts.backend 只能是 comfy、http 或 local，当前是 " +
                       backend);
    }
    // Python 侧这一条在 doctor 里查而不是在模型里查，这里保持一致，
    // 避免配置加载阶段就因为还没填地址而整个起不来。
    check_ge(errs, "tts.tolerance_s", tolerance_s, 0);
    check_range(errs, "tts.max_tempo_shift", max_tempo_shift, 0.0, 0.2);
    return errs;
}

std::vector<std::string> GateConfig::validate() const {
    std::vector<std::string> errs;
    check_ge(errs, "gates.min_pixel_std", min_pixel_std, 0);
    check_ge(errs, "gates.min_pixel_mean", min_pixel_mean, 0);
    check_ge(errs, "gates.max_pixel_mean", max_pixel_mean, 0);
    check_range(errs, "gates.min_frame_similarity", min_frame_similarity, 0.0, 1.0);
    check_gt(errs, "gates.max_audio_drift_s", max_audio_drift_s, 0);
    check_ge(errs, "gates.max_attempts_per_shot", max_attempts_per_shot, 1);
    return errs;
}

std::vector<std::string> AssemblyConfig::validate() const {
    std::vector<std::string> errs;
    check_range(errs, "assembly.fps", fps, 1, 120);
    check_range(errs, "assembly.crf", crf, 0, 51);
    check_range(errs, "assembly.audio_sample_rate", audio_sample_rate, 8000, 192000);
    check_range(errs, "assembly.audio_channels", audio_channels, 1, 2);
    check_range(errs, "assembly.scene_transition_s", scene_transition_s, 0.0, 2.0);
    check_range(errs, "assembly.subtitle_max_chars_per_line",
                subtitle_max_chars_per_line, 6, 30);
    check_range(errs, "assembly.subtitle_max_lines", subtitle_max_lines, 1, 3);
    return errs;
}

fs::path ModelsConfig::dir_path(const fs::path& workspace) const {
    if (dir && !dir->empty()) {
        std::error_code ec;
        fs::path p = paths::expand_user(*dir);
        fs::path abs = fs::absolute(p, ec);
        return ec ? p : abs;
    }
    // 回落到项目库旁边。模型和项目放一起，整个 workspace 拷到另一台机器
    // 就能直接跑——不然还得记得单独拷模型。
    return workspace / "models";
}

fs::path ModelsConfig::resolve(const std::string& entry,
                               const fs::path& workspace) const {
    if (entry.empty()) return {};
    fs::path p = paths::expand_user(entry);
    // 绝对路径原样用。多台机器共享一个网络盘时会这么填。
    if (p.is_absolute()) return p;
    std::error_code ec;
    fs::path abs = fs::absolute(dir_path(workspace) / p, ec);
    return ec ? dir_path(workspace) / p : abs;
}

std::vector<std::string> ModelsConfig::validate() const {
    // **只查 engine，模型文件一个都不查。**
    //
    // engine 是枚举，写错了不是"文件缺了"而是"整条出片的路走岔了"，
    // 而走岔的表现是连不上 ComfyUI 或者报"没有编进出图后端"——
    // 两句话都指不到真正的原因（拼写错误）。
    //
    // 文件这边能查的只有"在不在"，而那件事不该在配置加载阶段做：
    // 模型动辄好几个 G，装好程序还没下模型是常态。那时候如果加载直接失败，
    // 用户连界面都进不去，也就没法在界面里看到到底缺哪个文件。
    // 存在性检查在 doctor 里，报警告，程序照常起来。
    std::vector<std::string> errs;
    if (engine != "sd" && engine != "comfy") {
        errs.push_back("models.engine 只能是 sd 或 comfy，当前是 " + engine);
    }
    return errs;
}

std::string ModelsConfig::weights_for(double vram_gb) const {
    if (weights != "smart") return weights;
    // **文本编码器永远放内存。** 它每镜只跑一次（H3 的 Qwen3-VL-32B 实测
    // 8 到 9 秒），而它是这一套里最大的一块（18.9 GB）。放显存换来的那几秒
    // 远不如把地方让给扩散模型。
    //
    // VAE 看卡：放内存时每镜解码 71 秒（5.5 GB 搬过 PCIe），放显存 8 秒，
    // 一镜差 63 秒；但 32 GB 的卡上放不下（见 vae_vram_min_gb 那段的账）。
    return vram_gb >= vae_vram_min_gb ? "te=cpu" : "te=cpu,vae=cpu";
}

std::vector<std::string> Settings::validate() const {
    std::vector<std::string> errs;
    auto merge = [&errs](std::vector<std::string> more) {
        errs.insert(errs.end(), std::make_move_iterator(more.begin()),
                    std::make_move_iterator(more.end()));
    };
    merge(comfy.validate());
    merge(llm.validate());
    merge(tts.validate());
    merge(gates.validate());
    merge(assembly.validate());
    merge(models.validate());
    if (vram_gb_override && *vram_gb_override <= 0) {
        errs.push_back("vram_gb_override 必须大于 0");
    }
    if (!models.video_high_noise.empty() && models.video.empty()) {
        // 只填高噪声那份是配错了，而症状会是"出的片和以前一样"——
        // 高噪声那份被静默忽略，看不出来。
        errs.push_back(
            "[models].video_high_noise 填了但 [models].video 是空的："
            "双专家要两份，video 填低噪声那份");
    }
    if (models.video_lora_tiers != "draft" &&
        models.video_lora_tiers != "final" &&
        models.video_lora_tiers != "both") {
        errs.push_back("[models].video_lora_tiers 只能是 draft / final / both，现在是 " +
                       models.video_lora_tiers);
    }
    if (models.vae_vram_min_gb < 0) {
        errs.push_back("[models].vae_vram_min_gb 不能是负的");
    }
    if (models.video_vae_tile < 0) {
        errs.push_back("[models].video_vae_tile 不能是负的（0 = 用内置值）");
    }
    if (models.video_rng != "cuda" && models.video_rng != "cpu" &&
        models.video_rng != "std") {
        errs.push_back("[models].video_rng 只能是 cuda / cpu / std，现在是 " +
                       models.video_rng);
    }
    if (!models.video_llm.empty() && !models.video_text_encoder.empty()) {
        // 两个参数位只能填一个。都填了的话下面按 llm 走、t5xxl 那份被
        // 静默丢掉——症状是"出的片和提示词没关系"，没有任何报错指到这儿。
        errs.push_back(
            "[models].video_llm 和 video_text_encoder 只能填一个："
            "前者是 llm_path（MiniMax-H3 那类），后者是 t5xxl_path（Wan 那类）");
    }
    if (models.video_moe_boundary <= 0 || models.video_moe_boundary >= 1) {
        errs.push_back("[models].video_moe_boundary 要在 0 和 1 之间，现在是 " +
                       std::to_string(models.video_moe_boundary));
    }
    if (models.vram_reserve_gb < 0) {
        errs.push_back("[models].vram_reserve_gb 不能是负的");
    }
    if (models.frame_tier != "draft" && models.frame_tier != "final") {
        errs.push_back("[models].frame_tier 只能是 draft 或 final，现在是 " +
                       models.frame_tier);
    }
    if (models.weights.empty()) {
        errs.push_back("[models].weights 不能是空的（要 cpu / auto / 组件规格）");
    }
    return errs;
}

fs::path Settings::workspace_path() const {
    if (workspace && !workspace->empty()) {
        std::error_code ec;
        fs::path p = paths::expand_user(*workspace);
        fs::path abs = fs::absolute(p, ec);
        return ec ? p : abs;
    }
    return paths::user_data_dir(kAppName) / "projects";
}

// ---- 加载 ----

fs::path user_config_path() {
    return paths::user_config_dir(kAppName) / "config.toml";
}

namespace {

/// 环境变量后缀 -> 配置路径。与 Python 的 _ENV_MAPPING 一一对应。
const std::vector<std::pair<const char*, const char*>>& env_mapping() {
    static const std::vector<std::pair<const char*, const char*>> m = {
        {"COMFY_BASE_URL", "comfy_base_url"},
        {"COMFY_TIMEOUT_S", "comfy_timeout_s"},
        {"LLM_BASE_URL", "llm_base_url"},
        {"LLM_MODEL", "llm_model"},
        {"LLM_API_KEY", "llm_api_key"},
        {"TTS_BASE_URL", "tts_base_url"},
        // C++ 独有。Python 的 TTSConfig.backend 只有 comfy / http，
        // 没有 local（进程内那条路是这边加的），所以它那边也没有
        // 这个环境变量。和 MODELS_* 那九个是同一类扩展。
        // 有它才能不改配置文件就切到进程内配音——跑判据、跑 CI
        // 都要这个，改用户的配置文件是不该干的事。
        {"TTS_BACKEND", "tts_backend"},
        {"WORKSPACE", "workspace"},
        {"VRAM_GB", "vram_gb_override"},
        {"FFMPEG_PATH", "assembly_ffmpeg_path"},
        // 模型这几项是 C++ 独有的，Python 侧没有对应。
        // 容器里最常用的就是 MODELS_DIR——镜像不打包模型，
        // 挂个卷进来然后用它指过去。
        {"MODELS_DIR", "models_dir"},
        {"MODELS_LLM", "models_llm"},
        {"MODELS_VIDEO", "models_video"},
        {"MODELS_VIDEO_VAE", "models_video_vae"},
        {"MODELS_VIDEO_TEXT_ENCODER", "models_video_text_encoder"},
        {"MODELS_IMAGE", "models_image"},
        // **engine 原来漏了。** 别的 [models] 键都有环境变量，
        // 偏偏这个开关没有——而它决定出图出片走进程内还是走 ComfyUI，
        // 正是容器里和对拍时最需要临时翻的一个。
        // 对拍推理层就卡在这上面：两边默认走的不是同一条路，没法比。
        {"MODELS_ENGINE", "models_engine"},
        {"MODELS_TTS", "models_tts"},
        {"MODELS_IMAGE_VAE", "models_image_vae"},
        {"MODELS_IMAGE_TEXT_ENCODER", "models_image_text_encoder"},
        {"MODELS_IMAGE_TEXT_ENCODER_VISION", "models_image_text_encoder_vision"},
        {"MODELS_TTS_DECODER", "models_tts_decoder"},
    };
    return m;
}

/// 从 toml 表里取值，键不存在就保持原样。
///
/// 全部走「存在才覆盖」而不是「取值或默认」，是因为配置是分层合并的：
/// 用户配置里没写的项要留给下一层，不能被默认值顶掉。
template <typename T>
void take(const toml::table* tbl, const char* key, T& dest) {
    if (!tbl) return;
    if (auto node = tbl->get(key)) {
        if (auto v = node->value<T>()) dest = *v;
    }
}

void take_path_str(const toml::table* tbl, const char* key,
                   std::optional<std::string>& dest) {
    if (!tbl) return;
    if (auto node = tbl->get(key)) {
        if (auto v = node->value<std::string>()) dest = *v;
    }
}

void apply_table(const toml::table& doc, Settings& s) {
    if (auto t = doc["comfy"].as_table()) {
        take(t, "base_url", s.comfy.base_url);
        take(t, "timeout_s", s.comfy.timeout_s);
        take(t, "job_timeout_s", s.comfy.job_timeout_s);
        take(t, "max_retries", s.comfy.max_retries);
    }
    if (auto t = doc["llm"].as_table()) {
        take(t, "base_url", s.llm.base_url);
        take(t, "model", s.llm.model);
        take(t, "api_key", s.llm.api_key);
        take(t, "timeout_s", s.llm.timeout_s);
        take(t, "temperature", s.llm.temperature);
    }
    if (auto t = doc["workers"].as_table()) {
        if (auto v = (*t)["gpu"].value<std::int64_t>()) {
            s.workers.gpu = static_cast<int>(*v);
        }
        if (auto arr = (*t)["endpoints"].as_array()) {
            s.workers.endpoints.clear();
            for (const auto& v : *arr) {
                if (auto sv = v.value<std::string>()) {
                    s.workers.endpoints.push_back(*sv);
                }
            }
        }
    }
    if (auto t = doc["tts"].as_table()) {
        take(t, "backend", s.tts.backend);
        take_path_str(t, "base_url", s.tts.base_url);
        take(t, "engine", s.tts.engine);
        take(t, "tolerance_s", s.tts.tolerance_s);
        take(t, "max_tempo_shift", s.tts.max_tempo_shift);
    }
    if (auto t = doc["gates"].as_table()) {
        take(t, "enabled", s.gates.enabled);
        take(t, "min_pixel_std", s.gates.min_pixel_std);
        take(t, "min_pixel_mean", s.gates.min_pixel_mean);
        take(t, "max_pixel_mean", s.gates.max_pixel_mean);
        take(t, "min_frame_similarity", s.gates.min_frame_similarity);
        take(t, "max_audio_drift_s", s.gates.max_audio_drift_s);
        take(t, "target_lufs", s.gates.target_lufs);
        take(t, "max_true_peak_db", s.gates.max_true_peak_db);
        take(t, "max_attempts_per_shot", s.gates.max_attempts_per_shot);
        take(t, "fallback_on_exhausted", s.gates.fallback_on_exhausted);
    }
    if (auto t = doc["assembly"].as_table()) {
        take(t, "fps", s.assembly.fps);
        take(t, "pix_fmt", s.assembly.pix_fmt);
        take(t, "video_codec", s.assembly.video_codec);
        take(t, "crf", s.assembly.crf);
        take(t, "audio_codec", s.assembly.audio_codec);
        take(t, "audio_bitrate", s.assembly.audio_bitrate);
        take(t, "audio_sample_rate", s.assembly.audio_sample_rate);
        take(t, "audio_channels", s.assembly.audio_channels);
        take(t, "scene_transition_s", s.assembly.scene_transition_s);
        take(t, "subtitle_max_chars_per_line", s.assembly.subtitle_max_chars_per_line);
        take(t, "subtitle_max_lines", s.assembly.subtitle_max_lines);
        take(t, "subtitle_font", s.assembly.subtitle_font);
        take(t, "ffmpeg_path", s.assembly.ffmpeg_path);
        take(t, "ffprobe_path", s.assembly.ffprobe_path);
    }
    if (auto t = doc["models"].as_table()) {
        take(t, "engine", s.models.engine);
        take_path_str(t, "dir", s.models.dir);
        take(t, "llm", s.models.llm);
        take(t, "video", s.models.video);
        take(t, "video_vae", s.models.video_vae);
        take(t, "video_text_encoder", s.models.video_text_encoder);
        take(t, "image", s.models.image);
        take(t, "image_vae", s.models.image_vae);
        take(t, "image_text_encoder", s.models.image_text_encoder);
        take(t, "image_text_encoder_vision", s.models.image_text_encoder_vision);
        take(t, "tts", s.models.tts);
        take(t, "tts_decoder", s.models.tts_decoder);
        take(t, "diffusion_flash_attn", s.models.diffusion_flash_attn);
        take(t, "weights", s.models.weights);
        take(t, "video_cfg", s.models.video_cfg);
        take(t, "video_flow_shift", s.models.video_flow_shift);
        take(t, "image_cfg", s.models.image_cfg);
        take(t, "image_flow_shift", s.models.image_flow_shift);
        take(t, "frame_tier", s.models.frame_tier);
        take(t, "vram_reserve_gb", s.models.vram_reserve_gb);
        take(t, "video_high_noise", s.models.video_high_noise);
        take(t, "video_moe_boundary", s.models.video_moe_boundary);
        take(t, "video_llm", s.models.video_llm);
        take(t, "video_llm_vision", s.models.video_llm_vision);
        take(t, "video_audio_vae", s.models.video_audio_vae);
        take(t, "video_rng", s.models.video_rng);
        take(t, "video_lora", s.models.video_lora);
        take(t, "video_lora_strength", s.models.video_lora_strength);
        take(t, "video_lora_tiers", s.models.video_lora_tiers);
        take(t, "video_vae_tile", s.models.video_vae_tile);
        take(t, "vae_vram_min_gb", s.models.vae_vram_min_gb);
    }
    take_path_str(&doc, "workspace", s.workspace);
    if (auto node = doc.get("vram_gb_override")) {
        if (auto v = node->value<double>()) s.vram_gb_override = *v;
    }
}

void read_toml_into(const fs::path& path, Settings& s) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    try {
        auto doc = toml::parse_file(paths::to_utf8(path));
        apply_table(doc, s);
    } catch (const toml::parse_error& e) {
        // 配置坏了要说清楚是哪个文件。只说「配置解析失败」的话，
        // 用户手上有用户级和项目级两份，只能挨个翻。
        std::ostringstream os;
        os << "配置文件解析失败：" << paths::to_utf8(path) << "\n" << e.description()
           << "（第 " << e.source().begin.line << " 行）";
        throw std::runtime_error(os.str());
    }
}

/// 环境变量覆盖。
///
/// 环境变量都是字符串，数值项要转换。转换失败就忽略这一项而不是报错——
/// 容器里注入了一个格式不对的值，整个服务起不来比用默认值更糟。
void apply_env(Settings& s) {
    auto get = [](const char* suffix) {
        return paths::env((std::string(kEnvPrefix) + suffix).c_str());
    };
    auto as_double = [](const std::string& v, double& dest) {
        try { dest = std::stod(v); } catch (...) {}
    };

    std::string v;
    if (!(v = get("COMFY_BASE_URL")).empty()) s.comfy.base_url = v;
    if (!(v = get("COMFY_TIMEOUT_S")).empty()) as_double(v, s.comfy.timeout_s);
    if (!(v = get("LLM_BASE_URL")).empty()) s.llm.base_url = v;
    if (!(v = get("LLM_MODEL")).empty()) s.llm.model = v;
    if (!(v = get("LLM_API_KEY")).empty()) s.llm.api_key = v;
    if (!(v = get("TTS_BASE_URL")).empty()) s.tts.base_url = v;
    if (!(v = get("TTS_BACKEND")).empty()) s.tts.backend = v;
    if (!(v = get("WORKSPACE")).empty()) s.workspace = v;
    if (!(v = get("VRAM_GB")).empty()) {
        double d = 0;
        as_double(v, d);
        if (d > 0) s.vram_gb_override = d;
    }
    if (!(v = get("FFMPEG_PATH")).empty()) s.assembly.ffmpeg_path = v;
    if (!(v = get("MODELS_DIR")).empty()) s.models.dir = v;
    if (!(v = get("MODELS_LLM")).empty()) s.models.llm = v;
    if (!(v = get("MODELS_VIDEO")).empty()) s.models.video = v;
    if (!(v = get("MODELS_VIDEO_VAE")).empty()) s.models.video_vae = v;
    if (!(v = get("MODELS_VIDEO_TEXT_ENCODER")).empty()) {
        s.models.video_text_encoder = v;
    }
    if (!(v = get("MODELS_IMAGE")).empty()) s.models.image = v;
    // engine 只认那两个取值。写错了不静默接受——那会让整条出片的路
    // 悄悄走岔，而表现是"连不上 ComfyUI"或者"没编进出图后端"，
    // 两句话都指不到真正的原因（环境变量拼错了）。
    // 这里保持原值，随后 validate() 会拦住它并说清楚。
    if (!(v = get("MODELS_ENGINE")).empty()) s.models.engine = v;
    if (!(v = get("MODELS_IMAGE_VAE")).empty()) s.models.image_vae = v;
    if (!(v = get("MODELS_IMAGE_TEXT_ENCODER")).empty())
        s.models.image_text_encoder = v;
    if (!(v = get("MODELS_IMAGE_TEXT_ENCODER_VISION")).empty())
        s.models.image_text_encoder_vision = v;
    if (!(v = get("MODELS_TTS")).empty()) s.models.tts = v;
    if (!(v = get("MODELS_TTS_DECODER")).empty()) s.models.tts_decoder = v;
}

}  // namespace

Settings load_settings(const std::optional<fs::path>& project_dir) {
    Settings s;  // 内置默认值就是成员初始化器
    read_toml_into(user_config_path(), s);
    if (project_dir) read_toml_into(*project_dir / "changji.toml", s);
    apply_env(s);

    // 地址类的值统一规整，避免 http://x:8188/ 和 http://x:8188
    // 被当成两个不同的服务
    s.comfy.base_url = strip_trailing_slash(s.comfy.base_url);
    s.llm.base_url = strip_trailing_slash(s.llm.base_url);
    if (s.tts.base_url) s.tts.base_url = strip_trailing_slash(*s.tts.base_url);

    return s;
}

std::map<std::string, std::string> env_overridden() {
    std::map<std::string, std::string> out;
    for (const auto& [suffix, key] : env_mapping()) {
        std::string name = std::string(kEnvPrefix) + suffix;
        if (!paths::env(name.c_str()).empty()) out[key] = name;
    }
    return out;
}

// ---- 写模板 ----

namespace {

// 与 Python 的 _DEFAULT_TOML 保持一致。注释是给人看的，不能省。
constexpr const char* kDefaultToml = R"(# 场记配置文件
# 优先级：环境变量 > 项目目录下的 changji.toml > 本文件 > 内置默认值

# 项目库根目录。留空则用系统标准数据目录。
# 换机器时把项目目录整个拷走即可，程序装在哪都不影响。
# workspace = "D:/短剧项目"

# 显存覆盖。推理服务跑在另一台机器时本机探测不到显卡，用它手动指定。
# vram_gb_override = 16

[comfy]
# 推理服务地址。可以是本机，也可以是局域网里任意一台有显卡的机器。
base_url = "http://127.0.0.1:8188"
job_timeout_s = 1800
max_retries = 3

[llm]
# 剧本和分镜用的大模型。默认走本地 Ollama。
# 也可以填任何兼容 OpenAI 接口的服务。
base_url = "http://127.0.0.1:11434/v1"
model = "qwen3:14b"

[tts]
# backend 有三个值：
#   local —— **进程内配音，不需要装任何外部服务**。要填下面 [models] 里的
#            tts 和 tts_decoder 两个模型文件。想先听听效果的话，不必配也不必
#            建项目，直接：changji --say "雨下了一整夜。" --tts-model <骨干>
#            --tts-decoder <解码器>
#   comfy —— 通过推理服务（ComfyUI）的 TTS 节点调用
#   http  —— 独立的配音服务
backend = "comfy"
engine = "cosyvoice3"

[gates]
# 质量闸门。全自动模式下这些阈值决定废片能不能被拦住。
enabled = true
max_attempts_per_shot = 3
# 重试超限时降级为静帧加运镜，保证整集能出片而不是卡死。
fallback_on_exhausted = true

[assembly]
fps = 24
crf = 18
# 只在场景切换处用溶解，同场景内一律硬切。
scene_transition_s = 0.4
# 中文字幕单行上限，全角字符数。
subtitle_max_chars_per_line = 15
subtitle_font = "Source Han Sans SC"

# ⚠️ **迁移期间这一整节默认是注释掉的。**
#
# Python 引擎的 Settings 是 extra="forbid"，只要这份配置里出现 [models]，
# **它整份加载失败、后端根本起不来**，报的是
# "Extra inputs are not permitted"。而迁移期间两个后端共用这一份文件。
#
# 要用进程内推理（sd.cpp / 进程内配音）就把下面这些取消注释——
# 那之后 Python 引擎会起不来，阶段 8 之前请确认你不再需要它。
# 或者把这一节写进项目目录的 changji.toml，只影响那一个项目。
#
# [models]
# 出图出片走哪个引擎。sd = 进程内 sd.cpp，comfy = 外部 ComfyUI。
# 选 comfy 时视频一定走 ComfyUI；首帧要项目里有 workflows/image.json
# 才走它，没有就退回 sd.cpp——图像工作流是用户提供的，不能假定存在。
# engine = "sd"

# 进程内推理要用的模型文件。ComfyUI 那条路不需要这一节——
# 那边模型是它自己管的，工作流里按名字引用。
#
# 相对路径相对下面的 dir 解析，绝对路径原样用（多机共享网络盘时会这么填）。
# dir 留空则用项目库旁边的 models/ 目录。
#
# 这几项**必须自己填**，没有默认文件名。原因是同一个二进制要在配置差很多的
# 机器上跑：24G 显存的机器和 8G 内存的树莓派，能装下的量化档完全不同，
# 猜一个默认值只会让人以为配好了然后在加载时炸掉。
#
# dir = "~/models"
# llm = "Qwen3-14B-Q4_K_M.gguf"
# 进程内配音（[tts] backend = "local"）要这两个。骨干是自回归那半，
# 解码器把 token 变成波形，缺一个都出不了声。
# tts = "Qwen3-TTS-12Hz-1.7B-Base-Q4_K_M.gguf"
# tts_decoder = "mmproj-Qwen3-TTS-12Hz-1.7B-Base-f16.gguf"
# video = "Wan2.2-TI2V-5B-Q4_K_M.gguf"
# video_vae = "Wan2.2_VAE.safetensors"
# video_text_encoder = "umt5-xxl-encoder-Q5_K_M.gguf"
# image = "Qwen_Image_Edit-Q2_K.gguf"
# 图像模型自己的 VAE 和文本编码器。**别拿视频那套顶**——
# Wan 的 VAE 和 Qwen-Image 的不是一回事，UMT5-XXL 和 Qwen2.5-VL 更不是
# （在 sd.cpp 里连参数位都不同）。喂错了不报错，只是出来的图和提示词没关系。
# 这两项留空会退回 video_vae / video_text_encoder，只用 Wan 的人不必填。
# image_vae = "qwen_image_vae.safetensors"
# image_text_encoder = "Qwen2.5-VL-7B-Instruct-Q2_K.gguf"
# 2509 及以后的 Qwen-Image-Edit 还要视觉塔，初版可以不填
# image_text_encoder_vision = "Qwen2.5-VL-7B-Instruct-mmproj-BF16.gguf"
#
# 权重放哪。cpu（默认）= 放系统内存、用到才搬进显存，小卡上能跑全靠它，
# 代价是每一步都在等 PCIe。auto = 交给 sd.cpp 按这张卡真实的空闲显存决定，
# 装得下的常驻显存——大卡（≥ 24 GB）上用这个，实测出片阶段利用率从 35% 起飞。
# weights = "auto"
#
# 采样旋钮，按角色分开。默认值是 sd.cpp 上游文档给这两个模型的推荐值，
# 一般不用动。图像那条路 cfg 太高（比如 7）出来的就是噪点。
# video_cfg = 6.0
# video_flow_shift = 3.0
# image_cfg = 2.5
# image_flow_shift = 3.0
#
# 首帧按哪个档位出。默认 draft（和 Python 一样）；首帧是跨镜头一致性的锚点，
# 又会当起始图喂给出片那一步，草稿档的首帧配成片档的视频等于把锚点放大两倍
# 再用。显存够就填 final，慢一些但清楚。
# frame_tier = "final"
#
# 双专家视频模型的高噪声那一份（Wan 2.2 的 A14B 系列）。video 填低噪声那份，
# 这里填高噪声那份，留空就是单模型。高噪声专家跑前几步定构图和运动，
# 低噪声专家跑后几步出细节。
# **换 A14B 之后 video_cfg 要跟着改**：上游给 A14B 的是 3.5，5B 那边是 6.0。
# video_high_noise = "Wan2.2-I2V-A14B-HighNoise-Q8_0.gguf"
#
# 两个专家在哪个 sigma 交班，默认 0.875（sd.cpp 的默认）。
# 调大 = 高噪声专家跑得更久，运动更大、细节更少。
# video_moe_boundary = 0.875
#
# 视频模型的 LLM 类文本编码器。video_text_encoder 走 t5xxl_path（Wan 那一路的
# UMT5-XXL），这一项走 llm_path（MiniMax-H3 那类用大语言模型当编码器的）。
# **两个只能填一个**，填错了不报错，只是出来的片和提示词没关系。
# video_llm = "qwen3vl_32b_minimax_h3-Q4_K_M.gguf"
# video_llm_vision = ""
#
# 音频 VAE。给 MiniMax-H3 这类画面和声音一起生成的模型用；不填的话联合扩散
# 照跑，但出来的片没有解码好的音轨。
# video_audio_vae = "minimax_h3_audio_vae_fp32.safetensors"
#
# 出片用哪种随机数发生器：cuda（默认，Wan 那一路）/ cpu / std。
# 上游给 MiniMax-H3 的命令行是 --rng cpu；发生器不同则同一个种子出的画面不同，
# 而且不报错。只影响出片，出图那条不动。
# video_rng = "cpu"
#
# 出片挂一个 LoRA。Turbo 那类蒸馏适配器能把采样步数压到 6 步左右（约 5 倍）。
# **挂上之后步数要跟着改**，不改的话白挂。认不认这类给 ComfyUI 做的 LoRA
# 要实测：不认时只是加载不上、画面照出，判据得看耗时有没有真降下来。
# video_lora = "loras/minimax_h3_turbo_v4_step600_ema.safetensors"
# video_lora_strength = 1.0
#
# LoRA 挂在哪些档位：draft / final / both（默认）。Turbo 那类蒸馏 LoRA 拿
# 画质换速度，适合只挂草稿档——草稿看叙事和构图，6 步够；成片跑满步数不挂。
# video_lora_tiers = "draft"
#
# 出片时 VAE 解码的分块大小（潜空间格子），0 = 用内置的 16×11。
# 调小换显存：块的计算缓冲小了，VAE 权重才有机会常驻显存。5090 上实测
# VAE 放内存解码要 71 秒、放显存只要 8 秒，而按内置块大小放显存会差 112 MB。
# video_vae_tile = 12
#
# weights = "smart" 时，显存到多少才把 VAE 放显存（GB）。默认 40 是量出来的：
# 5090（32.6 GB）上扩散 17.9 + VAE 5.5 = 23.4 GB 权重，加扩散自己约 9 GB 的
# 计算缓冲就差 112 MB 装不下。VAE 放内存每镜解码 71 秒、放显存 8 秒，
# 所以大卡上一定要放进去。
# vae_vram_min_gb = 40.0
#
# weights = "auto" 时给计算缓冲留多少显存（GB）。auto 的预算是给权重的，
# 而生成时那块计算缓冲比"给驱动留一成"大一个量级：1280×704 的 VAE 解码
# 实测要 6.6 GB。留少了的症状是出图全失败、日志里 decode_first_stage failed。
# 出更大的图要调大它。
# vram_reserve_gb = 6.0
)";

}  // namespace

std::string default_config_template() { return kDefaultToml; }

fs::path write_default_config(const std::optional<fs::path>& path) {
    fs::path target = path ? *path : user_config_path();
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(target, std::ios::binary);
    if (!out) throw std::runtime_error("写不了配置文件：" + paths::to_utf8(target));
    out << kDefaultToml;
    return target;
}

}  // namespace changji::config
