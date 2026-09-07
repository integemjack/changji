#pragma once

// 两个长任务：连着写好几集、给还没分镜的剧集批量出分镜。
//
// 和前面五个接口的根本区别：**它们跑几分钟，不是几十秒**。
// 所以走 job 表——立刻返回 {"started": true, ...}，进度靠
// GET /api/script/series 轮询或者 WebSocket 推。
//
// 两个共用同一个任务槽（JobKind::Write）。Python 那边也是同一个
// WriteState，所以"写整季的时候不能同时批量出分镜"这条限制是照抄的，
// 不是我加的。

#include <memory>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"
#include "llm/client.hpp"

namespace changji::http {

/// POST /api/script/series —— 连着写好几集，边写边存。
///
/// 跟单集不同，这个**直接落库并建出剧集**。一集一集手点新建再手点写，
/// 写到第五集人就放弃了，那量产就无从谈起。写完可以逐集再改。
///
/// 客户端用 shared_ptr 是因为任务在工作线程上跑，可能比这次请求活得久。
/// 传引用的话，调用方一不小心让它先析构，表现是工作线程访问已释放对象——
/// 而那时候栈上已经没有任何线索指向这里了。
ApiResult post_script_series(const nlohmann::json& body,
                             std::shared_ptr<llm::Client> client);

/// POST /api/plan/all —— 把还没分镜的剧集一次补齐。
///
/// 连着写了五集之后，每一集都还得单独点一次「重出分镜」。
/// 五次里漏掉一次，跑整个项目时那一集就被跳过去了。
ApiResult post_plan_all(const nlohmann::json& body,
                        std::shared_ptr<llm::Client> client);

}  // namespace changji::http
