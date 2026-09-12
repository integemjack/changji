#pragma once

// 剧本、选题、预告片。
//
// 三件事共用一套解析——下游的分镜、配音、装配认的是渲染出来的那段文本，
// 不关心它是正片还是预告片。schema 有两份：正片是**四段的**（开场钩子、
// 冲突推进、情绪回报、集尾留扣，按秒排，每段自己的拍数地板），预告片是
// 平的一份。
//
// **四段是 2026-09-12 加的，因为 60 秒的集写出了 13 秒的剧本。** 行业里
// 一集的时长是剧本自己长出来的：四拍各占几秒、台词两三百字、分镜照着拍子
// 来。我们的剧本原来是一个扁平的 beats 数组，地板写死 minItems 4，60 秒
// 只是一句话交给后面的分镜模型——所以 60 → 13 → 6 每一步都合法地缩水。
// 时长的形状得先在剧本这一层立住。
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

/// 削掉动作行开头的机位标签，比如「镜头特写：病历单上的日期…」。
///
/// **2026-09-12 实跑撞上的。** 分镜是下一步的事（行业里那叫导演脚本），
/// 剧本里不该有景别和运镜；但模型会写。而这句话的祸不是多余——**渲染出来
/// 「镜头特写：…」和一句台词长得一模一样**，下游全靠冒号认说话人：
/// ScriptReader 会把它画成一个叫「镜头特写」的角色在说话，分镜模型和
/// 配音也照着当台词处理。
///
/// 只削确实像机位标签的那一层：冒号前八个字以内，而且带景别词。
/// 「牌子上写着：营业中」这种不动——它前面那截不是机位词。
std::string strip_camera_prefix(const std::string& text);

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

/// 一集四段里的一段，按秒排。
///
/// 开场钩子 / 冲突推进 / 情绪回报 / 集尾留扣——短剧行业的单集节奏公式
/// （0–5 秒抛冲突、5–40 推进、40–75 回报、最后 5–10 秒卡死钩子），
/// 按这一集的时长等比缩放，开场不超过 8 秒、留扣不超过 10 秒。
struct ActSpec {
    std::string key;    ///< opening / escalation / payoff / cliff，JSON 里的键
    std::string label;  ///< 开场钩子 / 冲突推进 / 情绪回报 / 集尾留扣
    int from_s = 0;
    int to_s = 0;
    /// 这一段至少几拍、至多几拍。**地板是 schema 里唯一管用的东西**——
    /// 提示词里"要凑够"模型不听，minItems 写几它就写几。每 4 秒一拍起，
    /// 顶到每 1.5 秒一拍。
    int min_beats = 0;
    int max_beats = 0;
};

/// 把一集按秒切成四段。时长小到切不开也不会崩（最少按 4 秒排）。
std::vector<ActSpec> act_plan(double duration_s);

/// 给提示词看的那一段：四段各占几秒、各干什么、至少几拍。
///
/// 一定要写进提示词，不能只放在 schema 的 description 里——**GBNF 里只有
/// 结构，描述模型从来没看见过**（见 chapter_write.cpp 那段注释）。
std::string render_act_brief(const std::vector<ActSpec>& specs);

/// 渲染出来的剧本里，段与段之间那一行：「【开场钩子 0–5 秒】」。
///
/// 段头留在正文里而不是另存一份结构，是因为下游全认这段文本：分镜模型
/// 看得到这一段占几秒，页面按它分块显示，人在文本框里改也改得动。
std::string act_header(const std::string& label, int from_s, int to_s);

/// 这一行是不是段头。只认那四个名字，「【字幕】三年后」「【倒计时 10 秒】」
/// 都不是。认出来的话把名字和秒数填进去（秒数没写就是 0）。
bool parse_act_header(const std::string& line, std::string* label,
                      int* from_s, int* to_s);
bool is_act_header(const std::string& line);

/// 去掉段头，只留拍子。老项目反推故事时用：段头进了"小说正文"是噪音。
std::string strip_act_headers(const std::string& script);

/// 写出来的一段。
struct Act {
    std::string key;
    std::string label;
    int from_s = 0;
    int to_s = 0;
    std::vector<Beat> beats;
};

/// 写出来的一集。
struct ScriptDraft {
    std::string title;
    std::string logline;
    /// 所有拍子，平的一份。acts 非空时它就是四段顺次拼起来的同一批拍子，
    /// speakers() / dialogue_chars() 都看它。
    std::vector<Beat> beats;
    /// 四段。平的回包（预告片、旧模型）解析出来这里是空的，渲染时就没段头。
    std::vector<Act> acts;

    /// 出场说话的角色，按首次出现排序，去重。
    std::vector<std::string> speakers() const;

    /// 对白总字数（**字符**不是字节）。用来判断超没超预算。
    std::size_t dialogue_chars() const;

    /// 渲染成后面几步认的写法。
    ///
    /// 对白一律「名字：台词」，动作单独成行，有段的话每段前面一行段头。
    /// 格式定死，不看模型心情，否则下一步识别角色就开始出错。
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

/// 剧本的 JSON Schema，**平的一份**：title、logline、beats。预告片用它——
/// 预告片是蒙太奇，不分四段。
const nlohmann::ordered_json& script_schema();

/// 正片的 JSON Schema，**四段的**：title、logline，然后 opening / escalation /
/// payoff / cliff 四个对象按顺序各带一个 beats 数组，每段的 minItems /
/// maxItems 按这一集的时长算（见 act_plan）。
///
/// 用四个命名字段而不是一个数组：GBNF 按 properties 的顺序生成，四段就一定
/// 按顺序、一段不少地出来；数组的话每一项的地板没法各不相同。
nlohmann::ordered_json script_schema(double duration_s);
/// 选题的 JSON Schema。
const nlohmann::ordered_json& premise_schema();

// ---- 解析 ----

/// 解析模型回包。四段的和平的都认。
///
/// duration_s 给了才知道每段占几秒（段头上那个数）；不给的话段还在，
/// 只是段头没有秒数。
ScriptDraft parse_script(const std::string& raw, double duration_s = 0.0);
std::vector<PremiseIdea> parse_premises(const std::string& raw);

}  // namespace changji::stages
