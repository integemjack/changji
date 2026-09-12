#pragma once

// 「这台机器能产什么」——**算出来的，不是填出来的**。
//
// 对等互联那张表（机器 × 能力）的第一列就是它。三层决定一个任务派到哪：
//
//   能不能  ← 这个文件。节点自检，量出来的事实
//   准不准  ← 用户在界面上勾的
//   该不该  ← 调度器：谁空着、谁手上有这一镜的首帧
//
// **为什么不让人填。** 编 build-all 那次漏了
// `-DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc`，**每一层都报成功**，
// 只是 ggml 的 CUDA 后端没编进去、整条流水线在 CPU 上跑：1280×704 一镜
// 跑了五小时才两步，而 nvidia-smi 上一个计算进程都没有。人填的是愿望，
// 量出来的才是事实。所以界面上只让关，不让开——关是用户的选择
// （"这台留着写剧本"），开是这台机器说了算。
//
// 判断逻辑和"去磁盘上看文件在不在"分开：**事实由调用方查好填进来**，
// 这一层只做推导。理由和 `worker_roster.hpp`、`exec_queue.hpp` 一样——
// 推导错了的表现是"某台机器明明能出片却一直不参与"，不报错、看不出来，
// 而真去摸文件系统的那半截没法在单元测试里反复撞。

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace changji::infer {

/// 一台机器可能提供的能力。
///
/// 粒度就是流水线上那几步，因为**派活是按步派的**：一集里写文只发生
/// 一次，出片每镜一次，两者该不该派给同一台是两个独立的决定。
enum class Capability {
    Llm,       ///< 写故事、大纲、章节、分镜
    Tts,       ///< 配音
    Frame,     ///< 出首帧
    Video,     ///< 出片
    Assemble,  ///< 装配、烧字幕（要 ffmpeg，不吃显卡）
};

const char* to_string(Capability c);

/// 反过来。认不出回 nullopt——**配置里写错一个能力名不能当没看见**：
/// 那会变成"我明明关了出片它还是派过去了"。
std::optional<Capability> capability_from(const std::string& s);
/// 界面上显示的名字。认不出来回空串。
const char* label_of(Capability c);

/// 全部能力，界面按这个顺序排列。
const std::vector<Capability>& all_capabilities();

/// 这台机器的事实。**全部由调用方查好填进来**，这个文件不碰磁盘。
struct NodeFacts {
    /// 二进制里编没编进 sd.cpp / llama.cpp。
    ///
    /// **这一条是真会不一样的**：`build-coord` 那份就是 `CHANGJI_SD=OFF`
    /// 编的，它起得来、界面能开，但一张图都出不了。
    bool built_with_sd = false;
    bool built_with_llama = false;

    /// ffmpeg 能不能跑起来（`FFmpeg::check()` 过了没有）。
    bool ffmpeg_ok = false;

    /// `[models]` 里每个键是不是**配了而且文件真在盘上**。
    /// 没出现在表里的键一律当成"没有"。
    std::map<std::string, bool> models;

    /// 大模型指到远端去了（`[llm].backend = "remote"` 且地址非空）。
    /// 那样的话本机有没有 llm 权重都不影响——它转手就发出去。
    bool llm_remote = false;
    /// 配音指到外部 HTTP 服务了（`[tts].backend = "http"` 且地址非空）。
    bool tts_remote = false;
};

/// 一个能力能不能干，干不了是因为什么。
struct CapabilityReport {
    Capability cap = Capability::Frame;
    bool able = false;
    /// 干不了的原因，**一句给人看的话**：它会显示在那张表的格子上
    /// （悬停时），所以要说到"该去做什么"那一步，不能只说"不支持"。
    std::string why;
};

/// 按上面那些事实，算出每个能力能不能干。顺序同 `all_capabilities()`。
std::vector<CapabilityReport> capabilities_of(const NodeFacts& f);

/// 某个能力缺什么。能干回空串。
///
/// 单独暴露是因为 `cannot_do`（接任务之前那道自检）问的是同一件事，
/// 只是问法是按任务而不是按能力。**规则只该有一处**——两处各写一份的话，
/// 迟早出现"表上说能干，派过去却被拒"，而那时候用户看到的是一镜失败。
std::string missing_for(Capability c, const NodeFacts& f);

}  // namespace changji::infer
