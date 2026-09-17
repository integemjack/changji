#pragma once

// 成片：GET /api/film 看切出来的几集，POST /api/film/cut 按每集时长切。
// 算法和落盘在 pipeline/series_cut。

#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"

namespace changji::http {

/// GET /api/film?path= —— output/final 里的几集，带上一次切的清单。
/// {per_episode_s, total_s, files: [{name, rel, size_mb, mtime, start_s, end_s, from_chapter}]}
ApiResult get_film(const std::string& path);

/// POST /api/film/cut —— body: {project, per_episode_s}。0 = 整部一集。
/// 跑在 JobKind::Run 那个槽上（出片那边忙着就 409），进度走 GET /api/run。
/// 缺哪一章的成片当场 400，不起活。
ApiResult post_film_cut(const nlohmann::json& body);

}  // namespace changji::http
