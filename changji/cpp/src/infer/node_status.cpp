#include "infer/node_status.hpp"

#include <filesystem>
#include <system_error>

#include "infer/llama_tts.hpp"
#include "infer/local_exec.hpp"
#include "infer/sd_backend.hpp"
#include "media/ffmpeg.hpp"
#include "util/paths.hpp"

namespace changji::infer {

namespace {

using nlohmann::json;

/// `[models]` 里这些键会被查一遍。**顺序无所谓，缺项才要紧**：
/// 少查一个键，能力判断就会说"能干"，而派过去是一镜失败。
std::vector<std::pair<std::string, std::string>> model_entries(
    const config::ModelsConfig& m) {
    return {
        {"llm", m.llm},
        {"tts", m.tts},
        {"tts_decoder", m.tts_decoder},
        {"image", m.image},
        {"image_vae", m.image_vae},
        {"video", m.video},
        {"video_vae", m.video_vae},
    };
}

/// 这台机器叫什么。身份不靠它（那是密钥的事），它只是给人看的。
std::string node_name() {
    for (const char* key : {"CHANGJI_NODE_NAME", "COMPUTERNAME", "HOSTNAME"}) {
        const std::string v = paths::env(key);
        if (!v.empty()) return v;
    }
    return "未命名";
}

/// 这个卷还剩多少字节。问不到回 0。
std::uint64_t free_bytes_of(const std::filesystem::path& dir) {
    std::error_code ec;
    // **问的是这个目录所在的卷。** 目录还不存在时往上退一级再问——
    // 模型目录常常是"还没建"的状态，而那时候恰恰最需要知道装不装得下。
    std::filesystem::path p = dir;
    while (!p.empty() && !std::filesystem::exists(p, ec)) {
        const auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    if (p.empty()) return 0;
    const auto info = std::filesystem::space(p, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(info.available);
}

}  // namespace

NodeFacts probe_facts(const config::Settings& s) {
    NodeFacts f;
    f.built_with_sd = sd_available();
    // 只问配音那条（mtmd）。写文那条 2026-09-14 删了，见 NodeFacts 的注释。
    f.built_with_llama_tts = llama_tts_available();

    try {
        const media::FFmpeg ff(s.assembly.ffmpeg_path, s.assembly.ffprobe_path,
                               media::default_runner());
        ff.check();
        f.ffmpeg_ok = true;
    } catch (const std::exception&) {
        f.ffmpeg_ok = false;
    }

    const auto ws = s.workspace_path();
    for (const auto& [key, name] : model_entries(s.models)) {
        if (name.empty()) {
            f.models[key] = false;
            continue;
        }
        std::error_code ec;
        f.models[key] =
            std::filesystem::is_regular_file(s.models.resolve(name, ws), ec);
    }

    f.llm_remote = s.llm.backend == "remote" && !s.llm.base_url.empty();
    f.tts_remote = s.tts.backend == "http" && s.tts.base_url.has_value() &&
                   !s.tts.base_url->empty();
    return f;
}

std::string can_produce_line(const NodeFacts& f) {
    std::string out;
    for (const auto& r : capabilities_of(f)) {
        if (!r.able) continue;
        if (!out.empty()) out += "、";
        out += label_of(r.cap);
    }
    return out.empty() ? "什么都产不了" : out;
}

json node_status_json(const config::Settings& s,
                      const models::HardwareProfile& profile) {
    const NodeFacts f = probe_facts(s);

    json caps = json::array();
    for (const auto& r : capabilities_of(f)) {
        caps.push_back({{"cap", to_string(r.cap)},
                        {"label", label_of(r.cap)},
                        {"able", r.able},
                        {"why", r.why}});
    }

    json gpus = json::array();
    if (profile.gpu) {
        // **报的是卡 0 的显存乘以卡数吗？不是。** vram_mb 是单卡的，
        // count 是张数——加起来去查档位表会算出一张卡根本跑不动的分辨率，
        // 见 GPUInfo 那两条注释。这里两个数分开报，谁要用谁自己判。
        gpus.push_back({{"name", profile.gpu->name},
                        {"vram_mb", profile.gpu->vram_mb},
                        {"count", profile.gpu->count},
                        {"unified_mb", profile.gpu->unified_mb}});
    }

    const auto models_dir = s.models.dir_path(s.workspace_path());
    return json{
        {"name", node_name()},
        {"version", CHANGJI_VERSION},
        {"built_with", {{"sd", f.built_with_sd},
                        {"llama_tts", f.built_with_llama_tts},
                        {"ffmpeg", f.ffmpeg_ok}}},
        {"gpus", gpus},
        {"capabilities", caps},
        {"models_dir", paths::to_utf8(models_dir)},
        {"models_dir_free_bytes", free_bytes_of(models_dir)},
        // 忙不忙问的是本机那个执行位——它才是"这张卡此刻有没有人用"
        // 的唯一真相，`Scheduler` 那边的 loaded 只说明模型还在显存里。
        {"busy", local_exec().busy()},
        {"waiting", local_exec().waiting()},
    };
}

}  // namespace changji::infer
