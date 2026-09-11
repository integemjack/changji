#pragma once

// 生成参考图：/api/character/reference/generate、/api/location/reference/generate。
//
// 和 upload.hpp 那两个是**同一个槽位的两条来路**——一条是用户自己找图传上来，
// 一条是照着设定里那段外观描述现画一张。落点、旧图清理、以及"换了参考图就
// 全剧重跑"这条语义，两条路完全一样，所以共用 claim_ref_path 和
// reset_all_shots。
//
// 这是**「设定」那一页上唯一该用 AI 的地方**（用户 2026-09-11 的判断）：
// 内容都在故事里创作完，设定这一步不创作新的人和地方，只把故事里的名单
// 翻成可画的描述、再照着那份描述生成参考图。
//
// **同步的，一次一张。** 一张图几十秒（头一张还要先把出图模型读进显存，
// 十几秒到一分钟），没有走任务表：任务表一种只有一个槽，参考图要是占了
// Run 那个槽，用户就没法一边出参考图一边跑别的——而这两件事在调度器那层
// 本来就会排队，再加一层槽只会让"为什么点不动"更难说清。

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"  // ApiResult / ApiError / guard

namespace changji::http {

/// 这一张用哪个种子。
///
/// body 里给了 seed 就用它（折回 [0, 2^31) ），没给就按 stem 算一个**固定**
/// 值：同一个角色重出得到的还是那张图。随机的话，用户每点一次刷新就换一
/// 张脸——而他点刷新往往只是想确认刚才那张存下来了。真要换一张，界面自己
/// 送一个 seed 进来。
///
/// 导出只为了能测这条"固定"的规矩。
std::int64_t ref_seed(const nlohmann::json& body, const std::string& stem);

/// POST /api/character/reference/generate
///
/// body: {project, char_id, slot?（默认 front）, seed?}
///
/// seed 不给就按 char_id + slot 算一个固定值——同一个角色重出还是那张图。
/// 界面上的「换一张」要自己送一个随机 seed 进来，响应里回的就是这次用的。
ApiResult post_character_reference_generate(const nlohmann::json& body);

/// POST /api/location/reference/generate
///
/// body: {project, location_id, seed?}。场景只有一张空景图，没有 slot。
ApiResult post_location_reference_generate(const nlohmann::json& body);

}  // namespace changji::http
