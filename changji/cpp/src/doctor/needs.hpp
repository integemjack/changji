#pragma once

// 「当前这份配置到底用得上哪些外部依赖」——纯判断，不碰网络。
//
// 拆出来的理由和 CMakeLists 里写的是同一条：`doctor.cpp` 链 httplib，
// 测试目标不收它，所以**放在那里面的判断逻辑没法测**。
// 而这里的判断错了后果不轻：体检报告底下那句「有 N 项必须先解决才能出片」
// 会被当成待办照做，说错了等于把人支去装一个根本不需要的依赖。
//
// 2026-09-08 就是这么出的错：`[models].engine = "sd"` 时报告仍然说
// 「出图和出视频都要用 ComfyUI」并判 FAIL。而那条路走进程内的 sd.cpp，
// `run_deps.cpp` 里 `if (engine != "comfy") return b;` 在碰任何
// ComfyUI 工作流之前就返回了。

#include <string>

#include "config/settings.hpp"

namespace changji::doctor {

/// ComfyUI 在当前配置下是不是硬依赖。
struct ComfyNeed {
    /// 出图出片要用它（`[models].engine == "comfy"`）
    bool for_video = false;
    /// 配音要用它（`[tts].backend == "comfy"`）
    bool for_tts = false;

    bool required() const { return for_video || for_tts; }

    /// 给用户看的那半句话："……要用它"。
    ///
    /// 分开写而不是一句通用的"要用它"，是因为人拿着这句话去查问题：
    /// 说成"出图和出视频"而实际是配音在用，方向就找反了。
    std::string who() const {
        if (for_video && for_tts) return "出图出片和配音都要用它";
        if (for_video) return "出图和出视频要用它";
        if (for_tts) return "配音要用它";
        return "当前配置用不到它";
    }
};

ComfyNeed comfy_need(const config::Settings& s);

/// 配音走的是不是进程内那条路。
///
/// 走这条时体检不该去问 ComfyUI——两份 GGUF 就在盘上，判得出来，
/// 和 ComfyUI 一点关系没有。原来没有这一支，`backend = "local"`
/// 会掉进 ComfyUI 那条路，报"推理服务连不上，无法判断"。
bool tts_is_local(const config::Settings& s);

}  // namespace changji::doctor
