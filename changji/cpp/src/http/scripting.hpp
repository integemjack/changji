#pragma once

// 剧本相关的三个接口：想选题、写一集、剪预告。
//
// 三个都是**同步**的：调一次大模型，几十秒内返回，不进 job 表。
// 写整季（/api/script/series）不一样，那个要跑几分钟，走 job 表和
// WebSocket 广播。
//
// 大模型客户端是传进来的，不是这里造的。测试要用 ReplayClient——
// 真调模型的话每次输出都不一样，接口的形状就没法对拍了。

#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"
#include "llm/client.hpp"
#include "pipeline/jobs.hpp"

namespace changji::http {

/// 预告片挂的剧集 id。它在好几处要被特殊对待，所以是个常量。
inline constexpr const char* kTrailerEpisodeId = "trailer";

/// POST /api/script/premise —— 想几个选题给人挑。不落库。
ApiResult post_script_premise(const nlohmann::json& body, llm::Client& client,
                              pipeline::CancelToken& tok);

/// POST /api/script/write —— 写一集，回给界面让人过目。
///
/// 只有梗概会落库（下次写新一集时回填，不用凭记忆重打），剧本本身不落。
/// 剧本是整条流水线的源头，源头没审过就往下跑，后面几十分钟的渲染全是白跑。
ApiResult post_script_write(const nlohmann::json& body, llm::Client& client,
                            pipeline::CancelToken& tok);

/// POST /api/script/trailer —— 剪一条预告片。不落库。
ApiResult post_script_trailer(const nlohmann::json& body, llm::Client& client,
                              pipeline::CancelToken& tok);

}  // namespace changji::http
