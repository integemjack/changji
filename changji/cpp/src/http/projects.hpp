#pragma once

// 项目的新建、删除、改梗概。
//
// 这三个是阶段 3 漏掉的，路由表核对时找出来的。

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"

namespace changji::http {

/// POST /api/new —— 新建项目。界面上不用先跑命令行。
///
/// 只填名字就落在项目库根目录下。让用户去猜容器里的绝对路径是没道理的，
/// 他也不知道项目库挂在哪。
ApiResult post_new_project(const nlohmann::json& body,
                           const config::Settings& settings);

/// POST /api/project/delete —— 删掉一个项目，连同它的素材和成片。
///
/// **这一步不可逆，所以设了三道闸**：
///
///   一，目录必须在项目库**里面**（严格在里面，等于项目库本身也不行）
///   二，必须确实是一个项目（有 project.json）
///   三，名字必须一字不差地再打一遍
///
/// 少任何一道，一次误点就能把跑了一夜的成片全删了。
/// 三道闸都是照抄 Python 的，一道都没放宽。
ApiResult post_delete_project(const nlohmann::json& body,
                              const config::Settings& settings);

/// POST /api/project/premise —— 存下这部剧讲什么。写下一集时当提示词用。
ApiResult post_project_premise(const nlohmann::json& body);

}  // namespace changji::http
