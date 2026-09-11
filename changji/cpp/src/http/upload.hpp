#pragma once

// 参考图上传：/api/character/reference /api/location/reference
// 以及对应的两个 /clear。
//
// 参考图是一致性最硬的手段：文字描述再细，模型每次也会重新想象一遍
// 那张脸；给一张图，它就照着画。所以传了图必须无条件重跑全剧——
// 跟改外观是一回事。
//
// multipart 的解析留在路由层（那是 crow 的事），这里只收已经拆好的
// 字段和二进制数据，这样能不起服务就把校验和落盘逻辑测一遍。

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "http/readonly.hpp"  // ApiResult / ApiError / guard
#include "models/project.hpp"

namespace changji::http {

/// 参考图能传哪些格式。
///
/// 不是随便什么文件都往项目目录里塞，而且 ComfyUI 那边的 LoadImage
/// 也只认这几种。返回扩展名，认不出来返回空串。
std::string ref_suffix_for(const std::string& content_type);

/// 单张上限 20MB。
///
/// 参考图是给模型看的，几千像素足够；传一张两百兆的原片进来
/// 只会把项目目录撑爆。
constexpr std::size_t kRefMaxBytes = 20u * 1024u * 1024u;

/// 在 refs/ 里给这个名字定一个落点，顺手清掉**同名不同扩展名**的旧图。
///
/// 上传和生成都从这里过，因为那条清理规则两边都要守：同一个槽位先传过
/// 一张 jpg、再生成一张 png 的话，不清的话 refs 里会留一张永远用不上的
/// ——而且用户看不到，只有翻目录才发现。
std::filesystem::path claim_ref_path(const models::ProjectStore& store,
                                     const std::string& stem,
                                     const std::string& suffix);

/// POST /api/character/reference
///
/// slot 只能是 front / three_quarter / back。
ApiResult post_character_reference(const std::string& project_path,
                                   const std::string& char_id,
                                   const std::string& slot,
                                   const std::string& content_type,
                                   const std::string& data);

/// POST /api/location/reference —— 空景图，场景只有一张，没有 slot。
ApiResult post_location_reference(const std::string& project_path,
                                  const std::string& location_id,
                                  const std::string& content_type,
                                  const std::string& data);

/// POST /api/character/reference/clear
ApiResult post_character_reference_clear(const nlohmann::json& body);

/// POST /api/location/reference/clear
ApiResult post_location_reference_clear(const nlohmann::json& body);

}  // namespace changji::http
