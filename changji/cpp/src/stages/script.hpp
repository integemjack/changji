#pragma once

// 剧本、选题、预告片。
//
// 三件事共用一套 JSON schema 和一套解析——下游的分镜、配音、装配认的是
// 渲染出来的那段文本，不关心它是正片还是预告片。
//
// 和 bible/storyboard 一样，这里只有纯函数，不碰网络也不碰 llama.cpp。

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"

namespace changji::stages {

class ScriptError : public std::runtime_error {
public:
    explicit ScriptError(const std::string& what) : std::runtime_error(what) {}
};

// ---- 文本清洗 ----
//
// 这三个函数存在的原因是同一件事：**形式是我们定的，不是模型定的。**
// 模型爱给动作行加括号、给台词加引号、在开头挂个时间码，无论提示词里
// 怎么说。这些符号会一路流到分镜提示词和字幕里，所以在入口就削掉。

/// 削掉整段外面套的那一对括号或引号。
///
/// 只削套住整段的那一层，中间的括号是内容的一部分，不动。
std::string strip_wrapper(const std::string& text);

/// 削掉动作行开头的时间码标记，比如「[0-3秒] 画面特写：…」。
///
/// 提示词里没让它写，schema 里也没有这一项，模型自己加的。这段文字会原样
/// 进到分镜提示词里，让画面模型去理解一个时间码。
///
/// 只削开头那一对括号，而且要括号里确实像时间码才削：带数字，并且带
/// 秒、s 或者冒号。不做这个限制的话，「（他犹豫了）他开口」这种正常写法
/// 开头也会被削掉。
std::string strip_leading_timecode(const std::string& text);

/// 把「没人说话」的各种写法统一成空字符串。
///
/// schema 里写的是填空字符串，但模型经常填 none、null、旁白 这类词。
/// 原样当成名字的话，成片里会出现一个叫 none 的角色，
/// 字幕上写着「none：寂静」。
std::string normalize_speaker(const std::string& raw);

/// 这个时长大概能装多少字对白。
int budget_chars(double duration_s);

// ---- 数据 ----

/// 一个选题。
struct PremiseIdea {
    std::string title;
    std::string premise;
    std::string hook;
};

/// 一拍。动作或者一句台词。
struct Beat {
    std::string kind;     ///< action 或 dialogue
    std::string speaker;  ///< kind 是 action 时是空串
    std::string text;
};

/// 写出来的一集。
struct ScriptDraft {
    std::string title;
    std::string logline;
    std::vector<Beat> beats;

    /// 出场说话的角色，按首次出现排序，去重。
    std::vector<std::string> speakers() const;

    /// 对白总字数（**字符**不是字节）。用来判断超没超预算。
    std::size_t dialogue_chars() const;

    /// 渲染成后面几步认的写法。
    ///
    /// 对白一律「名字：台词」，动作单独成行。格式定死，不看模型心情，
    /// 否则下一步识别角色就开始出错。
    std::string render() const;
};

// ---- 提示词 ----
//
// 三个都**必须和 Python 的对应函数逐字节一致**。

std::string build_script_prompt(const std::string& premise, double duration_s,
                                models::StyleLine style_line,
                                const std::string& previous = "",
                                const std::vector<std::string>& characters = {});

std::string build_premise_prompt(const std::string& keywords,
                                 models::StyleLine style_line, int count = 3,
                                 const std::vector<std::string>& existing = {});

std::string build_trailer_prompt(const std::string& premise, double duration_s,
                                 models::StyleLine style_line,
                                 const std::string& episodes = "",
                                 const std::vector<std::string>& characters = {});

/// 剧本的 JSON Schema。正片和预告片共用。
const nlohmann::ordered_json& script_schema();
/// 选题的 JSON Schema。
const nlohmann::ordered_json& premise_schema();

// ---- 解析 ----

ScriptDraft parse_script(const std::string& raw);
std::vector<PremiseIdea> parse_premises(const std::string& raw);

}  // namespace changji::stages
