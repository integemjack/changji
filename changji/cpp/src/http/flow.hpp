#pragma once

// 引导流程的进度判定：七步走到哪一步了。
//
// **这一层原来在 Node 那个 BFF 里**（`webapp/server/src/routes/flow.js`）。
// 引擎自己发前端之后，走那条路的人拿不到 `/bff/*`——表现是界面能打开、
// 项目列表也在，但**剧集下拉框是空的**，制作页点不动。
// 也就是说"打开端口就能看处理进度"这件事没真做完。所以把它搬过来。
//
// **判定规则只写在一处。** 写在前端的话，侧边栏、顶部进度条、下一步按钮
// 三个地方各判一遍，迟早对不上——这句是 flow.js 开头的原话，搬过来仍然成立。
//
// ⚠️ **有一处能力上的差**：`publish` 那一步在 Node 那边靠 BFF 自己存的投递
// 记录判定，引擎这边没有那份记录，所以恒为未完成。投递本身也只有 Node 那侧
// 有。要用投递就仍然起 Node 那一层。

#include <string>

#include <nlohmann/json.hpp>

namespace changji::http {

/// 七步的定义。前端的侧边导航按 `phase` 分段。
///
/// **2026-09-10 从八步变七步**：分镜和制作合成一步「镜头」，
/// 因为那两页也合成一页了（前端 `ShotsView`）。
///
/// 和 `flow.js` 的 `STEPS` 逐字对齐——前端按 `key` 认人，改一个字
/// 侧边栏就少一格。
nlohmann::json flow_steps();

/// 判定每一步做完了没有。**纯函数，好测。**
///
/// 参数就是三个接口的原样返回：`/api/project`、`/api/shots`、`/api/outputs`。
/// 单独抽出来是因为这里面全是"什么算做完"的判断，而那些判断错了不会报错，
/// 只会让侧边栏停在某一步上，用户不知道为什么走不下去。
///
/// `episode_id` 为空表示还没选集——这时按全剧那三步判定，
/// 按集的四步一律未完成。
nlohmann::json flow_assess(const nlohmann::json& project,
                           const nlohmann::json& shots,
                           const nlohmann::json& outputs,
                           const std::string& episode_id);

}  // namespace changji::http
