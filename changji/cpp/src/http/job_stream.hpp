#pragma once

// 一件长活干完了，怎么把结果送回界面。
//
// **接口当场回一句"开始了"，结果走这条 WebSocket。** 为什么非这么办：
// Crow 一条 I/O 线程管着一批连接，请求在它上面占多久，落在同一条线程上的
// 连接就干等多久——写一章一两分钟，出一张参考图几十秒。实测那期间别的
// 请求会卡满二十多秒，而顶栏那块表更惨：它是长连接，认准了一条线程，
// 一旦落在被占住那条上，会冻到这件活干完。
//
// ⚠️ 试过另一条路——活挪到后台线程、干完在那条线程上 `res.end()`
// （Crow README 里的异步写法）——**不行，别再试**。详见 server.hpp 里
// concurrency 那段。响应必须由 Crow 自己那条线程发出，所以它只能当场
// 回"开始了"。
//
// 三种消息，够了：
//
//     job_progress   干到哪儿了（有进度才发，比如出图的采样步数）
//     job_done       干完了，带整份结果——界面拿它换掉手上那份
//     job_error      砸了，带一句人话
//
// **done 里带整份结果，而不是让界面自己再拉一次。** 多一个来回本身不算
// 什么，但那一步正好是用户等得最久的一步，"写完了"和"看到"之间再插一段
// 空白很难受。

#include <string>

#include <nlohmann/json.hpp>

namespace changji::http {

/// 干完了。`result` 就是这个接口同步跑时会回的那份 body。
void job_done(const std::string& stream_id, nlohmann::json result);

/// 砸了。`message` 直接显示给用户看。
void job_error(const std::string& stream_id, const std::string& message);

/// 干到哪儿了。`total` 为 0 表示"不知道一共几步"，界面就不画进度条。
void job_progress(const std::string& stream_id, int current, int total,
                  const std::string& message = "");

/// 采样到一半的那张小图（`data:image/png;base64,…`）。
///
/// **只广播，不留底。** 一张几十 KB，而这条通道两秒还要推一次系统表；
/// 存起来或者补发都会把它变成主要流量。错过就错过——下一步马上又有一张。
void job_preview(const std::string& stream_id, int step, std::string data_url);

}  // namespace changji::http
