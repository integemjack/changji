#pragma once

#include <string_view>

namespace changji::http {

/// 这个二进制自己答的那几条 `/bff/*`。
///
/// **为什么要有这份清单。** webapp 原来跑在 Node 那层后面，`/bff/*` 是那层
/// 的接口；把 webapp 嵌进二进制之后，这几条要由 C++ 自己答。漏一条的后果
/// 不是"少个功能"——2026-09-10 漏了 `/bff/settings/overview`，表现是
/// **同一个页面上两个相反的结论**：顶栏读 `/bff/settings/status`（实现了）
/// 显示"引擎已连接"，设置页读 overview（404）当引擎离线处理，
/// 地址空、配置文件空、右上角写"连不上"。
///
/// 单元测试看不见这种错：两边各自都是对的，错的是中间少了一条。
/// 所以 `test_webapp.cpp` 拿**打包进来的前端代码**里出现的 `/bff/` 路径
/// 来查这份清单。
///
/// **在 server.cpp 里加一条 `/bff` 路由时，这里也要加。** 不加的话那条
/// 路由照样能用，只是这份清单不再是全集——而清单的意义就是"前端要的
/// 都在这儿"，少了它就只是个摆设。
inline constexpr std::string_view kBffRoutes[] = {
    "/bff/health",
    "/bff/settings/status",
    "/bff/settings/config",
    "/bff/settings/overview",
    "/bff/flow",
    "/bff/project/video",
    "/bff/settings/llm",
    // 投递那一套**没搬进来**，但这几条仍然要答——回空形状加一句说明，
    // 而不是 404。前端 Promise.all 一挂整页就是个红框，
    // 用户分不清是"没做"还是"坏了"。
    "/bff/publish/platforms",
    "/bff/publish/targets",
    "/bff/publish/records",
    // 前端拼的是 /bff/publish/targets/<id>，扫出来就是带尾斜杠这一条。
    "/bff/publish/targets/",
    "/bff/publish/deliver",
    "/bff/publish/batch",
};

}  // namespace changji::http
