#pragma once

// 打包进来的前端，由引擎自己发。
//
// **为什么引擎要发前端。** 原来 `GET /` 回一句 text/plain 指路
// （"界面在 Node 那一层，默认 5174"），那是迁移期的临时状态：前端由一个
// Node 的 BFF 发，engine 只管接口。可它带来两件事——用户得起两个进程，
// 而且**打开引擎的端口看不到任何东西**，只有一句让他去别处的话。
//
// 现在把打包好的 dist 嵌进二进制（`bundled_webapp.inc.hpp`，28 个文件
// 329 KB），访问引擎的端口就能直接打开界面。
//
// ⚠️ **这一层只发静态文件，不替代 Node 那个 BFF。** BFF 除了转发 `/api`
// 还有自己的 `/bff/*`（引导流程判定、连接设置、投递平台）。走这条路的时候
// 那几个前缀是 404 的：制作页（看跑批进度）只用 `/api/*`，所以能用；
// 设置页和上传页要 `/bff/*`，用不了。
// 谁需要完整功能就仍然起 Node 那一层。

#include <string>
#include <string_view>

namespace changji::http {

/// 按扩展名给 Content-Type。
///
/// **必须带 charset=utf-8**：界面里全是中文，少了它浏览器按本地代码页猜，
/// 简体 Windows 上就是一片乱码。
std::string webapp_content_type(std::string_view path);

/// 找一个打包进来的文件。找不到回 nullptr。
///
/// `path` 是相对 dist 的，不带前导斜杠。
const std::string* find_webapp_file(std::string_view path);

/// 把请求路径规整成 dist 里的相对路径。
///
/// 干三件事：去掉前导 `/`、空路径当 `index.html`、**挡住 `..`**。
/// 最后一件是重点：这些文件虽然在内存里、翻不出去，但规整逻辑以后要是
/// 改成读磁盘，`..` 就是目录穿越。现在就挡住，省得那天忘了。
/// 挡下来的返回空串。
std::string normalize_webapp_path(std::string_view request_path);

/// 这个路径该不该由前端接管。
///
/// `/api` 开头的一律不接管——那是接口。其余的交给前端，
/// 找不到对应文件就回 `index.html`（单页应用的深链接要靠它，
/// 直接打开 /production 这种地址才不会 404）。
bool webapp_owns(std::string_view request_path);

}  // namespace changji::http
