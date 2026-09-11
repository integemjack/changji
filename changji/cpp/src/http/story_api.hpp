#pragma once

// 故事层的接口。整条流水线的新源头。
//
//   GET  /api/story          读故事（含分集表）
//   POST /api/story          存可编辑的那几项：梗概、体量、每集时长
//   POST /api/story/outline  让大模型写大纲——**只回草稿，不落库**
//   POST /api/story/adopt    采用一份大纲，落库并算分集表
//   POST /api/story/plan     按每集时长重算分集表
//
// 写和采用分开，是照着剧本那边已经立住的规矩：源头没人审过就往下跑，
// 后面几十分钟的渲染全是白跑。所以 AI 写完先摆出来，点了采用才落库。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "llm/client.hpp"
#include "pipeline/jobs.hpp"

namespace changji::http {

/// GET /api/story?path=… —— 读故事。
///
/// 项目没有 story.json 时回一个空故事而**不是 404**：老项目本来就没有，
/// 而前端要靠「空不空」决定摆大纲页还是摆老的剧本页。404 会被当成出错。
ApiResult get_story(const std::string& path);

/// POST /api/story —— 存梗概、体量、每集时长。
///
/// 梗概会**同时写回 project.json**。老流程的写剧本提示词读的是
/// Project::premise，两边各存一份的话，在这一页改完梗概去写剧本
/// 用的还是旧的那句。
ApiResult post_story(const nlohmann::json& body);

/// POST /api/story/outline —— 大模型写大纲，只回草稿。
ApiResult post_story_outline(const nlohmann::json& body, llm::Client& client,
                             pipeline::CancelToken& tok);

/// POST /api/story/adopt —— 采用一份大纲。
///
/// 整份替换，不做逐章合并：章节 id 是按位置生成的（ch01、ch02…），
/// 新大纲的 ch01 和旧大纲的 ch01 根本不是同一章，按 id 合并只会把
/// 两个故事拌在一起。已经展开过正文时要显式 overwrite，否则 409。
ApiResult post_story_adopt(const nlohmann::json& body);

/// POST /api/story/plan —— 按每集时长重算分集表。
ApiResult post_story_plan(const nlohmann::json& body);

/// POST /api/story/episodes —— 把分集表落成真的剧集。
///
/// 分集表是计划，剧集是流水线真正在跑的东西。分成两步而不是采用大纲时
/// 顺手建出来，是因为重算分集（改每集时长）是个随手的动作，而建剧集会
/// 动到已经写好剧本、已经出过片的那些集。
///
/// 已经存在的同号剧集**只补元数据，绝不碰 script 和 shots**：改一次每集
/// 时长就把写好的剧本冲掉，那是没法接受的。分集表里没有的老剧集一律留着。
ApiResult post_story_episodes(const nlohmann::json& body);

}  // namespace changji::http
