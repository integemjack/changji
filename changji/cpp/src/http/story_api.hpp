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

/// POST /api/story/revise —— 改原稿的某一段。**只回草稿，不落库。**
///
/// body: {project, chapter_id, from_char, to_char, instruction, history?}
///
/// 故事那一页的重点是创作，而创作不是"按一次按钮生成一整章"——是选中一段
/// 觉得不对的字，说一句"这儿太赶了，铺一下"，看它改完，再决定要不要。
///
/// `history` 是之前的来回（{role, text} 的数组），给对话那条路用。空的就是
/// "选中一段直接说一句"。**两条路共用同一个提示词和同一条写回路径**：两套
/// 机制都能改正文的话，迟早出现"对话改的和选中改的对同一段各有一份"。
///
/// 不落库比大纲那边更要紧：大纲落错了重写一份就是，改稿落错了盖掉的是
/// 作者自己写的字。
ApiResult post_story_revise(const nlohmann::json& body, llm::Client& client,
                            pipeline::CancelToken& tok);

/// POST /api/story/revise/apply —— 把改好的那一段写回去。
///
/// body: {project, chapter_id, from_char, to_char, text}
///
/// **不碰大模型**：这一步是机械的字符串替换 + 重算候选切点 + 重算分集表。
/// 分出来是因为它必须能单独测——写回去写错位置的话，盖掉的是作者的原文。
///
/// 有说法的钩子会跟着挪位置保住，落在被替换那一段里面的丢掉。
ApiResult post_story_revise_apply(const nlohmann::json& body);

/// POST /api/story/from_episodes —— 从已有剧集反推一份故事骨架。
///
/// 给**老项目**用：它们手里只有一集集写好的剧本，没有 story.json，于是
/// 人物关系、全局记忆这些新做的东西一样都用不上。
///
/// **不碰大模型**，也**不重新分集**：一集一章、整章，老项目那几集的边界
/// 一个字都不动——边界一挪，已经排好的分镜和出过的片就对不上它该在的那
/// 一段了。反推完顺手把每一集的 chapter_refs 接到对应的章上。
///
/// 回来的故事里人物、关系、地点都是空的，接着点「读现成正文提结构」
/// （/api/story/analyze）才有——那一步要模型读一遍内容。
ApiResult post_story_from_episodes(const nlohmann::json& body);

/// POST /api/story/import —— 把粘进来的一段文本切成章节。
///
/// 三个入口里的第二条：手里已经有小说/剧本，不必让模型再编一遍。
/// **不碰大模型**，切章节是机械活。和 /outline 一样**只回草稿不落库**。
///
/// 回来的故事只有章节和正文，人物、关系、地点都是空的——那些要读懂内容
/// 才提得出来，是下一步的事。
ApiResult post_story_import(const nlohmann::json& body);

/// POST /api/story/analyze —— 让大模型读一遍已经存下的正文，把结构提出来。
///
/// 粘贴导入只切章节，切完人物关系地点全是空的，走到「设定」那一步资产库
/// 还是空的。这一步补那个洞。
///
/// **正文一个字不动**，只补 summary、hook 和全剧的人物/关系/地点。
/// 和别的几个一样**只回草稿不落库**。
ApiResult post_story_analyze(const nlohmann::json& body, llm::Client& client,
                             pipeline::CancelToken& tok);

/// POST /api/story/chapter —— 展开一章的正文。
///
/// **这一个是直接落库的，不回草稿。** 别的几个都遵守「AI 写完先摆出来，
/// 点了采用才落库」，这里破例，理由是两条：一、它只往一个空字段里填东西，
/// 没有什么会被顶掉；二、一部十六章的故事要逐章展开，走草稿-采用就是
/// 三十二次点击。已经有正文的章要显式 overwrite，否则 409。
///
/// 正文落进去之后钩子要重建、分集表要重算——原来那些候选切点是对着空正文
/// 算出来的。
ApiResult post_story_chapter(const nlohmann::json& body, llm::Client& client,
                             pipeline::CancelToken& tok);

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
