#pragma once

// 一集的分镜怎么出：先切场，两场以上一场一次拆，一场就整集一次拆；
// 拆完补台词、查覆盖、推口型、重编号、拉回目标时长。
//
// 以前这一串在三个接口里各抄一份（POST /api/plan、单集页重出、批量补分镜）。
// 按场拆镜要改的正是这一串的中间那段——三处各改一遍迟早改漏一处，而漏掉
// 的那处表现是「这一集还是整集一次拆的」，不报错。所以收成一个。
//
// 它要一个 llm::Client，所以不放在 stages/（那儿只有纯函数）。

#include <functional>
#include <string>
#include <vector>

#include "llm/client.hpp"
#include "models/character.hpp"
#include "models/shot.hpp"
#include "pipeline/jobs.hpp"

namespace changji::pipeline {

struct StoryboardRunOptions {
    std::string script;
    models::AssetLibrary assets;
    std::string episode_id;
    double duration_s = 60.0;
    /// 模型的思考流，同 llm::Request::on_thinking。可空。
    std::function<void(const std::string&)> on_thinking;
    /// 「正在拆第 2/3 场：夜 · 内 · 天台」这类话往哪儿报。可空。
    std::function<void(const std::string&)> on_progress;
};

struct StoryboardRunResult {
    std::vector<models::Shot> shots;
    /// 引擎按剧本补进去的台词句数（见 stages::place_missing_dialogue）。
    int placed_lines = 0;
    /// 拆成了几场。1 = 整集一次拆（没有场次头）。
    int scenes = 1;
};

/// 出一集的分镜。分镜表废了抛 stages::StoryboardError，模型那边的错抛
/// llm::LlmError，和以前三处各自抛的一样。
StoryboardRunResult run_storyboard(const StoryboardRunOptions& opts,
                                   llm::Client& client, CancelToken& tok);

}  // namespace changji::pipeline
