#pragma once

// 故事大纲生成。故事分三层里的中间那层。
//
//     梗概（一两句）
//       ↓  这里
//     故事大纲（分章 + 人物关系 + 场景表）
//       ↓
//     章节正文（逐章展开）
//
// **中间这层是压缩的全局记忆。** 原来逐集续写的失忆不是因为「逐集」，
// 是因为没有一份压缩的全局状态可以每次都带上——只能带前三集原文，带不下
// 就截断。有了大纲，写第二十章时第二章埋的伏笔照样在上下文里。
//
// 和 bible/storyboard 一样，这里**不碰网络也不碰 llama.cpp**，只有拼提示词、
// 给 schema、解析返回三个纯函数。怎么把提示词送给模型是调用方的事。

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 大模型没按预期返回时抛这个。
class StoryError : public std::runtime_error {
public:
    explicit StoryError(const std::string& what) : std::runtime_error(what) {}
};

/// 请求里带的 JSON Schema，约束模型的输出结构。
///
/// **schema 里没有任何外观字段**，和分镜表是同一个手法：给了模型它就会忍不住
/// 写一遍长相，而那份长相和后面美术那一步出的必然有偏差。外观只存在于资产库。
const nlohmann::ordered_json& outline_schema();

/// 拼大纲提示词。
///
/// 章数由体量推（models::suggested_chapters），**不由用户填集数**——
/// 集数是后面按每集时长算出来的。
std::string build_outline_prompt(const std::string& premise,
                                 models::StoryScale scale,
                                 models::StyleLine style_line,
                                 const std::string& keywords = "");

/// 解析模型返回，产出 Story。
///
/// premise 和 scale 是调用方给的，模型不回这两项——它们是**输入**，
/// 让模型复述一遍只会让它改写用户写的那句话。
///
/// 章节 id 由这里生成（ch01、ch02…），不让模型取：模型给的 id 会重、会跳号、
/// 会带中文，而分集表是按 id 指回章节的。
models::Story parse_outline(const std::string& raw, const std::string& premise,
                            models::StoryScale scale);

}  // namespace changji::stages
