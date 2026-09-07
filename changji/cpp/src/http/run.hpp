#pragma once

// POST /api/run —— 开跑。
//
// 走 job 表：立刻返回 {"started": true, "queue": [...]}，进度靠
// GET /api/run 轮询或者 WebSocket 推。一集跑几十分钟，同步返回没有意义。
//
// 队列是为量产准备的：all_episodes 一次把整个项目有分镜的集都排上。
// **一集出错不拖垮后面几集**——跑一晚上，早上发现第二集挂了导致后面十集
// 都没动，那这一晚上就白熬了。

#include <functional>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"
#include "models/hardware.hpp"
#include "models/project.hpp"
#include "pipeline/episode.hpp"

namespace changji::http {

/// 开跑要用的外部东西。
///
/// 全都是回调而不是值，两个原因：一是**每次开跑现取配置**，用户改完
/// 模型文件不用重启（这条是有代价学来的，见 sd_image.hpp 里那段注释）；
/// 二是测试能塞假后端进来——真跑一集要几十分钟，而这一层要测的是
/// 队列、错误汇总和状态码，那些几毫秒就能测完。
struct RunDeps {
    std::function<config::Settings()> settings;
    std::function<models::HardwareProfile()> profile;
    /// 按当前配置造出图和出片的后端。
    ///
    /// 要 store 是因为 ComfyUI 那条路的工作流在**项目里**
    /// （workflows/video.json 覆盖内置的那份）。只给 settings 的话，
    /// "不同的剧用不同的模型"就没了。
    std::function<pipeline::Backends(const config::Settings&,
                                     const models::ProjectStore&)> backends;
};

/// 默认的那套：配置从 runtime 取，后端是 sd.cpp。
RunDeps default_run_deps();

ApiResult post_run(const nlohmann::json& body, const RunDeps& deps);

/// GET /api/run/preview —— 开跑之前先说清楚这一次会做什么、大概多久。
///
/// 以前只能按下开始再看，一按就是几十分钟。哪些镜头会重做、总共要等多久，
/// 这两件事应该在按下去之前就知道。
///
/// **一个镜头从它现在的状态开始，会一路走完后面所有阶段。** 只按当前状态
/// 归到一个阶段的话，会告诉人"配音 2 镜，粗估 16 秒"，而实际上那两镜还要
/// 出首帧、跑草稿档、跑成片档，得等十几分钟。报小了的预演比没有预演更糟。
ApiResult get_run_preview(const std::string& path,
                          const std::string& episode_id, bool all_episodes,
                          bool skip_final, bool force,
                          const models::HardwareProfile& profile);

/// GET /api/outputs —— 列出已经出好的成片。审片时直接在界面里播。
ApiResult get_outputs(const std::string& path);

/// 把阶段名列表翻成枚举。认不出的抛 ApiError。
///
/// 单独暴露是因为**它抛的错不该变成 400**：Python 那边这个校验在
/// run_stages 里，而那是在任务线程上跑的，错误落进任务状态的 error 字段。
/// 变成 400 的话前端的表现完全不同——一个是弹错误框，一个是任务列表里
/// 显示一条失败记录。
std::vector<pipeline::Stage> parse_stages(
    const std::vector<std::string>& names);

}  // namespace changji::http
