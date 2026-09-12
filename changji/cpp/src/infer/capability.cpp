#include "infer/capability.hpp"

namespace changji::infer {

namespace {

/// 这个键配了而且文件在盘上吗。
bool has(const NodeFacts& f, const std::string& key) {
    const auto it = f.models.find(key);
    return it != f.models.end() && it->second;
}

}  // namespace

const char* to_string(Capability c) {
    switch (c) {
        case Capability::Llm: return "llm";
        case Capability::Tts: return "tts";
        case Capability::Frame: return "frame";
        case Capability::Video: return "video";
        case Capability::Assemble: return "assemble";
    }
    return "unknown";
}

std::optional<Capability> capability_from(const std::string& s) {
    for (const Capability c : all_capabilities()) {
        if (s == to_string(c)) return c;
    }
    return std::nullopt;
}

const char* label_of(Capability c) {
    switch (c) {
        case Capability::Llm: return "写文";
        case Capability::Tts: return "配音";
        case Capability::Frame: return "首帧";
        case Capability::Video: return "出片";
        case Capability::Assemble: return "装配";
    }
    return "";
}

const std::vector<Capability>& all_capabilities() {
    // 顺序就是流水线的顺序，那张表按它排列。
    static const std::vector<Capability> kAll = {
        Capability::Llm, Capability::Tts, Capability::Frame,
        Capability::Video, Capability::Assemble};
    return kAll;
}

std::string missing_for(Capability c, const NodeFacts& f) {
    switch (c) {
        case Capability::Llm:
            // **指到远端就不看本机有什么**：那条路上这台只是个转发的。
            if (f.llm_remote) return {};
            if (!f.built_with_llama) {
                return "这个二进制没编 llama.cpp，写文只能指到远端服务";
            }
            if (!has(f, "llm")) return "[models].llm 没配，或者文件不在";
            return {};

        case Capability::Tts:
            if (f.tts_remote) return {};
            if (!f.built_with_llama) {
                return "这个二进制没编 llama.cpp，配音只能指到外部服务";
            }
            // 两个都要：骨干和解码器分开放，缺一个出不了声。
            // **缺了不会报错**，只会退回估算后端出一段静音——所以这里
            // 必须拦住，别让它"成功"。
            if (!has(f, "tts")) return "[models].tts 没配，或者文件不在";
            if (!has(f, "tts_decoder")) {
                return "[models].tts_decoder 没配，或者文件不在";
            }
            return {};

        case Capability::Frame:
            if (!f.built_with_sd) {
                return "这个二进制没编 sd.cpp，出不了图";
            }
            if (!has(f, "image")) return "[models].image 没配，或者文件不在";
            // VAE 两个键都认：图像专用的没配就退回视频那份，
            // 和 cannot_do 那边的规矩一致。
            if (!has(f, "image_vae") && !has(f, "video_vae")) {
                return "VAE 没配，或者文件不在（[models].image_vae "
                       "不填就退回 video_vae）";
            }
            return {};

        case Capability::Video:
            if (!f.built_with_sd) {
                return "这个二进制没编 sd.cpp，出不了片";
            }
            if (!has(f, "video")) return "[models].video 没配，或者文件不在";
            if (!has(f, "video_vae")) {
                return "[models].video_vae 没配，或者文件不在";
            }
            // **出片要 ffmpeg 把帧编成 mp4。** 这一条是实机烧出来的：
            // 扩散 8 步全跑完，到最后编码那一步才报找不到 ffmpeg。
            if (!f.ffmpeg_ok) return "没有 ffmpeg，出的帧编不成 mp4";
            return {};

        case Capability::Assemble:
            if (!f.ffmpeg_ok) return "没有 ffmpeg，装配和烧字幕都做不了";
            return {};
    }
    return "认不出这个能力";
}

std::vector<CapabilityReport> capabilities_of(const NodeFacts& f) {
    std::vector<CapabilityReport> out;
    out.reserve(all_capabilities().size());
    for (const Capability c : all_capabilities()) {
        CapabilityReport r;
        r.cap = c;
        r.why = missing_for(c, f);
        r.able = r.why.empty();
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace changji::infer
