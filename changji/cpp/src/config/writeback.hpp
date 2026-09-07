#pragma once

// 把改动写回用户全局配置。
//
// **保留文件里已有的注释和其它项。** 这一条是硬要求，不是锦上添花：
// 内置的配置模板整个是注释写的，每一节都在解释这一项是干什么的、
// 改了会怎样。重新序列化一遍整个 Settings 会把它们全抹掉，
// 用户下次打开配置文件看到的是一堆没有说明的键值对。
//
// Python 那边用 tomlkit，它天生保留格式。toml++ **不保留注释**——
// 解析成 table 再 << 出去，注释全没了。所以这里不走"解析再序列化"，
// 走**逐行编辑**：找到那一行改掉，别的一个字节都不动。
//
// 代价是要自己处理 TOML 的一小部分语法（节头、键值行、注释行）。
// 这个代价是值得的：写回配置是低频操作，而注释是用户理解配置的唯一途径。

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace changji::config {

/// 写回配置。patch 的形状是 {节名: {键: 值}}，顶层标量也收
/// （比如 vram_gb_override 就不在任何节里）。
///
/// 返回写到了哪个文件。写不进去抛 std::runtime_error。
///
/// 三条行为：
/// - 键已经在那一节里 —— 就地改那一行，前后的注释都不动
/// - 键不在但节在     —— 插在那一节的**末尾**，不是开头（开头会插到
///                       节的说明注释和第一个键之间，读起来断裂）
/// - 节不存在         —— 追加到文件末尾
std::filesystem::path save_user_config(
    const nlohmann::json& patch,
    const std::optional<std::filesystem::path>& path = std::nullopt);

/// 把一个 JSON 值转成 TOML 的字面量写法。
///
/// 单独暴露是为了能测：字符串要加引号并转义，浮点要保证带小数点
/// （写成 `1` 的话下次读回来是整数，而字段类型是 float，toml++ 会拒绝）。
std::string to_toml_literal(const nlohmann::json& v);

}  // namespace changji::config
