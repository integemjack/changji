#pragma once

// 角色圣经生成。
//
// 两阶段生成的第一阶段。先只产角色和场景，登记进资产库拿到 id，
// 第二阶段才产分镜，那时 schema 里已经没有外观字段可写了。
//
// 顺序不能反。先有分镜再补角色，模型已经在分镜里写过一遍外观，
// 之后再想统一就晚了。
//
// ---
//
// 这里刻意**不碰网络也不碰 llama.cpp**，只有两个纯函数：
// 拼提示词、解析返回。怎么把提示词送给模型是调用方的事。
//
// 这么切是因为这两件事的验证方式完全不同：提示词拼接要求和 Python
// 逐字节一致，可以用对拍测死；而"调模型"是有副作用、慢、且不确定的。
// 混在一起的话，前者就没法测了。

#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"

namespace changji::stages {

/// 大模型没按预期返回时抛这个。
class BibleError : public std::runtime_error {
public:
    explicit BibleError(const std::string& what) : std::runtime_error(what) {}
};

/// 请求里带的 JSON Schema，约束模型的输出结构。
///
/// 走远端 OpenAI 兼容接口时原样塞进 response_format；
/// 走进程内 llama.cpp 时要先转成 GBNF 语法。
const nlohmann::ordered_json& bible_schema();

/// 拼提示词。**输出必须和 Python 的 build_prompt 逐字节一致。**
std::string build_bible_prompt(const std::string& script,
                               models::StyleLine style_line);

/// 解析模型返回，产出资产库。
///
/// raw 是模型吐的原始文本，可能裹着 ```json 之类的外壳，
/// 解析前会先抽出 JSON 部分。
models::AssetLibrary parse_bible(const std::string& raw,
                                 models::StyleLine style_line,
                                 const std::string& aspect_ratio = "9:16");

/// 默认负向提示词。
///
/// 动漫线要额外压写实倾向，因为 Wan 有很强的写实偏置，
/// 不压的话动漫输入会被往真人方向拽。
std::string default_negative(models::StyleLine style_line);

}  // namespace changji::stages
