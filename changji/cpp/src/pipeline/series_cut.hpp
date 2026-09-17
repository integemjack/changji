#pragma once

// 成片：把所有章的片子按顺序接成一条，再按每集时长在镜头边界切成几集。
//
// 用户 2026-09-17：「剧本不应该有集的概念，集是最后用户选择多长时间为一集，
// 可以是 1 分钟、3 分钟、10 分钟、60 分钟，全部为一集那是用户的事情」。
// 所以"集"只在这儿出现一次：写作按章、出片按章，最后在这儿按时长切。
// 换个时长再切一次，前面什么都不用重跑。
//
// **只在镜头边界切**（media::split_into_episodes 的规矩）：切在一镜中间等于
// 把一个镜头劈成两半，画面和配音都对不上。所以每一集会比目标略长或略短。

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "config/settings.hpp"
#include "media/assemble.hpp"
#include "media/ffmpeg.hpp"
#include "models/project.hpp"
#include "pipeline/jobs.hpp"

namespace changji::pipeline {

/// 一集的切法：在整条时间线上从哪一秒到哪一秒，包含哪几镜。
struct CutPart {
    double start_s = 0.0;
    double end_s = 0.0;
    std::vector<std::string> shot_ids;
    /// 这一集从哪一章开始（chapter_id）。成片清单上给人看的。
    std::string first_chapter;
};

/// 纯算法，不碰文件。`chapters` 按章序，每一项是 (chapter_id, 这一章的时间线)。
/// per_episode_s <= 0 = 整部一集。
std::vector<CutPart> plan_series_cut(
    const std::vector<std::pair<std::string, media::Timeline>>& chapters,
    double per_episode_s);

/// 成片目录：output/final。每次切都清空重写。
std::filesystem::path final_dir(const models::ProjectPaths& paths);

/// 还不能切的话，为什么；空串 = 能。**有一章出了片就能切**（用户 2026-09-18：
/// 「这一章有片就可以成片了」），没片的章跳过、出了再切一次。
std::string series_cut_blocker(const models::ProjectStore& store);

struct SeriesCutReport {
    std::vector<std::filesystem::path> outputs;
    std::vector<CutPart> parts;
    double total_s = 0.0;
    /// 这次切进去的章、还没出片被跳过的章（chapter_id）。
    std::vector<std::string> chapters;
    std::vector<std::string> skipped;
};

/// 真干：把出了片的章（output/<ep>.mp4，切过的是 <ep>_NN.mp4）按章序拼成一条，
/// 按 plan 切，写进 output/final/，顺手写一份 cut.json（每一集从哪儿到哪儿、
/// 切了哪几章、跳过哪几章）。一章都没出片才抛。
SeriesCutReport cut_series(const models::ProjectStore& store,
                           const config::Settings& settings,
                           const media::FFmpeg& ff, double per_episode_s,
                           JobProgress& progress);

}  // namespace changji::pipeline
