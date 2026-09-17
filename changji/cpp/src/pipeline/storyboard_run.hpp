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
    /// **只在剧本估不出秒数时兜底。** 镜头数由剧本的内容定，拆完也不压回
    /// 这个数——一章多长由它自己的内容定，装配时再按每集时长切。
    ///
    /// 2026-09-16 之前这儿有个 `content_driven` 开关，关着的时候按一集
    /// 60 秒的配额拆、拆完再压回去，等于把「不够凑、超了压」从剧本挪到了
    /// 分镜。那条路（集模式）当天整个删了，开关跟着没了。
    double duration_s = 60.0;
    /// 模型的思考流，同 llm::Request::on_thinking。可空。
    std::function<void(const std::string&)> on_thinking;
    /// 「正在拆第 2/3 场：夜 · 内 · 天台」这类话往哪儿报。可空。
    std::function<void(const std::string&)> on_progress;
    /// **只看不发**：把每一场真正要发的那段字拼进 `peeked`，一个模型都不调。
    ///
    /// 用户 2026-09-17 要"复制提示词拿到别处去跑"。这一步是按场跑的，
    /// 一集三场就是三份不同的提示词——所以给的是三份，各带一个场次头，
    /// 不是只给第一份。
    bool peek = false;
};

struct StoryboardRunResult {
    std::vector<models::Shot> shots;
    /// 引擎按剧本补进去的台词句数（见 stages::place_missing_dialogue）。
    int placed_lines = 0;
    /// 拆成了几场。1 = 整集一次拆（没有场次头）。
    int scenes = 1;
    /// `opts.peek` 为真时，每一场那段提示词拼起来的全文。别的时候是空串。
    std::string peeked;
};

/// 出一集的分镜。分镜表废了抛 stages::StoryboardError，模型那边的错抛
/// llm::LlmError，和以前三处各自抛的一样。
StoryboardRunResult run_storyboard(const StoryboardRunOptions& opts,
                                   llm::Client& client, CancelToken& tok);

}  // namespace changji::pipeline
