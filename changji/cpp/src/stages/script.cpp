#include "stages/script.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "stages/json_extract.hpp"
#include "stages/script_prompt.inc.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

/// 对应 Python 的 f"{x:.0f}"。
///
/// 注意这是**银行家舍入**：f"{0.5:.0f}" 是 "0"，f"{1.5:.0f}" 是 "2"。
/// C 的 %.0f 在默认舍入模式下同样如此，所以直接用。
std::string format_f0(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return std::string(buf);
}

std::string strip_ascii(const std::string& s) { return text::strip_ws(s); }

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

/// 首尾这一对是不是套住整段的那一对。
///
/// 光数左右个数不够。「（甲说）乙答（丙笑）」左右各两个，数目相等，
/// 但首尾那两个并不是一对，削掉就把中间的括号弄错位了。
/// 要从头扫一遍看深度什么时候回到零。
///
/// 按 UTF-8 字符扫而不是按字节：中文括号一个三字节，按字节扫会把
/// 一个汉字的中间字节误当成括号。这里靠"整段前缀比较"来避免——
/// UTF-8 是自同步的，一个完整字符的字节序列不会出现在别的字符中间。
bool wraps_whole(const std::string& s, const std::string& left,
                 const std::string& right) {
    if (left == right) {
        // 引号这类左右一样的，中间不能再出现同一个符号
        const std::string inner =
            s.substr(left.size(), s.size() - left.size() - right.size());
        return inner.find(right) == std::string::npos;
    }
    int depth = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, left.size(), left) == 0) {
            ++depth;
            i += left.size();
        } else if (s.compare(i, right.size(), right) == 0) {
            --depth;
            if (depth == 0) return i + right.size() == s.size();
            if (depth < 0) return false;
            i += right.size();
        } else {
            i += text::utf8_char_len(static_cast<unsigned char>(s[i]));
        }
    }
    return false;
}

const std::set<std::string>& no_speaker_words() {
    static const std::set<std::string> kWords = [] {
        std::set<std::string> s;
        for (const char* w : prompt::kNoSpeaker) s.insert(w);
        return s;
    }();
    return kWords;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
    }
    return s;
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string get_str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    // 对应 Python 的 str(x)：模型偶尔会填数字
    return it->dump();
}

}  // namespace

std::string strip_wrapper(const std::string& text_in) {
    std::string out = strip_ascii(text_in);
    bool changed = true;
    while (changed && text::utf8_len(out) >= 2) {
        changed = false;
        for (const auto& pair : prompt::kWrappers) {
            const std::string left = pair[0], right = pair[1];
            if (!starts_with(out, left) || !ends_with(out, right)) continue;
            if (!wraps_whole(out, left, right)) continue;
            const std::string inner = strip_ascii(
                out.substr(left.size(), out.size() - left.size() - right.size()));
            if (inner.empty()) continue;
            out = inner;
            changed = true;
            break;
        }
    }
    return out;
}

std::string strip_leading_timecode(const std::string& text_in) {
    const std::string out = strip_ascii(text_in);
    for (const auto& pair : prompt::kLeadBrackets) {
        const std::string left = pair[0], right = pair[1];
        if (!starts_with(out, left)) continue;
        const std::size_t end = out.find(right);
        if (end == std::string::npos || end == 0) continue;
        const std::string inside = out.substr(left.size(), end - left.size());

        // 要有数字
        const bool has_digit = std::any_of(
            inside.begin(), inside.end(),
            [](char c) { return c >= '0' && c <= '9'; });
        if (!has_digit) continue;

        // 还要有时间单位。光有数字不够——
        // 「（他犹豫了3秒）」和「（第3次）」得区分开。
        bool has_unit = false;
        for (const char* u : prompt::kTimeUnits) {
            if (inside.find(u) != std::string::npos) {
                has_unit = true;
                break;
            }
        }
        if (!has_unit) continue;

        return strip_ascii(out.substr(end + right.size()));
    }
    return out;
}

std::string normalize_speaker(const std::string& raw) {
    const std::string name = strip_wrapper(raw);
    const std::string key = lower_ascii(strip_ascii(name));
    return no_speaker_words().count(key) ? std::string() : name;
}

int budget_chars(double duration_s) {
    // Python 的 int() 是**朝零截断**，不是四舍五入
    const double v = duration_s * prompt::kCharsPerSecond * prompt::kDialogueShare;
    return std::max(20, static_cast<int>(v));
}

// ---- ScriptDraft ----

std::vector<std::string> ScriptDraft::speakers() const {
    std::vector<std::string> seen;
    for (const Beat& b : beats) {
        const std::string name = strip_ascii(b.speaker);
        if (b.kind != "dialogue" || name.empty()) continue;
        if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
            seen.push_back(name);
        }
    }
    return seen;
}

std::size_t ScriptDraft::dialogue_chars() const {
    std::size_t n = 0;
    for (const Beat& b : beats) {
        if (b.kind == "dialogue") n += text::utf8_len(b.text);
    }
    return n;
}

std::string ScriptDraft::render() const {
    std::vector<std::string> lines;
    for (const Beat& b : beats) {
        const std::string t = strip_ascii(b.text);
        if (t.empty()) continue;
        if (b.kind == "dialogue") {
            const std::string name = strip_ascii(b.speaker);
            lines.push_back(name.empty() ? t : name + "：" + t);
        } else {
            lines.push_back(t);
        }
    }
    return join(lines, "\n");
}

// ---- 提示词 ----

std::string build_script_prompt(const std::string& premise, double duration_s,
                                StyleLine style_line,
                                const std::string& previous,
                                const std::vector<std::string>& characters) {
    std::string out;
    out += prompt::kScriptSeg0;
    out += format_f0(duration_s);
    out += prompt::kScriptSeg1;
    out += style_line == StyleLine::ANIME ? prompt::kScriptHintAnime
                                          : prompt::kScriptHintRealistic;
    out += prompt::kScriptSeg2;
    out += std::to_string(budget_chars(duration_s));
    out += prompt::kScriptRules;

    if (!characters.empty()) {
        out += prompt::kScriptCharsPre;
        out += join(characters, "、");
        out += prompt::kScriptCharsPost;
    }
    const std::string prev = strip_ascii(previous);
    if (!prev.empty()) {
        out += prompt::kScriptPrevPre;
        out += text::truncate_utf8(prev, prompt::kPrevMaxChars);
        out += prompt::kScriptPrevPost;
    }
    out += prompt::kScriptTailHead;
    out += strip_ascii(premise);
    out += prompt::kScriptTailEnd;
    return out;
}

std::string build_premise_prompt(const std::string& keywords,
                                 StyleLine style_line, int count,
                                 const std::vector<std::string>& existing) {
    std::string out;
    out += prompt::kPremiseSeg0;
    out += style_line == StyleLine::ANIME ? prompt::kPremiseHintAnime
                                          : prompt::kPremiseHintRealistic;
    out += prompt::kPremiseSeg1;
    out += std::to_string(count);
    out += prompt::kPremiseRules;

    const std::string kw = strip_ascii(keywords);
    if (!kw.empty()) {
        out += prompt::kPremiseKeywordsPre;
        out += kw;
        out += prompt::kPremiseKeywordsPost;
    }
    if (!existing.empty()) {
        // 已经有的方向要避开，否则连点两次「再想几个」会拿到同一批
        std::vector<std::string> trimmed;
        const std::size_t n =
            std::min<std::size_t>(existing.size(), prompt::kExistingMaxItems);
        for (std::size_t i = 0; i < n; ++i) {
            trimmed.push_back(text::truncate_utf8(strip_ascii(existing[i]),
                                                  prompt::kExistingMaxChars));
        }
        out += prompt::kPremiseExistingPre;
        out += join(trimmed, "、");
        out += prompt::kPremiseExistingPost;
    }
    out += prompt::kPremiseTailEnd;
    return out;
}

std::string build_trailer_prompt(const std::string& premise, double duration_s,
                                 StyleLine style_line,
                                 const std::string& episodes,
                                 const std::vector<std::string>& characters) {
    std::string out;
    out += prompt::kTrailerSeg0;
    out += style_line == StyleLine::ANIME ? prompt::kTrailerHintAnime
                                          : prompt::kTrailerHintRealistic;
    out += prompt::kTrailerSeg1;
    out += format_f0(duration_s);
    out += prompt::kTrailerSeg2;
    out += std::to_string(budget_chars(duration_s));
    out += prompt::kTrailerRules;

    if (!characters.empty()) {
        out += prompt::kTrailerCharsPre;
        out += join(characters, "、");
        out += prompt::kTrailerCharsPost;
    }
    const std::string eps = strip_ascii(episodes);
    if (!eps.empty()) {
        out += prompt::kTrailerEpisodesPre;
        out += text::truncate_utf8(eps, prompt::kEpisodesMaxChars);
        out += prompt::kTrailerEpisodesPost;
    }
    out += prompt::kTrailerTailHead;
    out += strip_ascii(premise);
    out += prompt::kTrailerTailEnd;
    return out;
}

// ---- Schema ----

const ordered& script_schema() {
    static const ordered s = [] {
        ordered beat_props = ordered::object();
        beat_props["kind"] = {
            {"type", "string"},
            {"enum", ordered::array({"action", "dialogue"})},
            {"description", "action 是动作或环境描写，dialogue 是有人说话"}};
        beat_props["speaker"] = {
            {"type", "string"},
            {"description", "说话的人。kind 是 action 时填空字符串"}};
        beat_props["text"] = {
            {"type", "string"},
            {"description",
             "这一拍的内容。\n"
             "kind=dialogue：只写说出口的话，不带引号，不重复人名。\n"
             "kind=action：写**画面上看得见的东西**——谁在哪、身体在做"
             "什么、碰到什么物件。换了地方或时间就把光线一并交代。\n"
             "不要写心里怎么想（「她很生气」画不出来），也不要一拍塞"
             "三个动作（分镜只能挑一个画，剩下的就丢了）。"}};

        ordered items = ordered::object();
        items["type"] = "object";
        items["additionalProperties"] = false;
        items["required"] = {"kind", "speaker", "text"};
        items["properties"] = beat_props;

        ordered props = ordered::object();
        props["title"] = {{"type", "string"},
                          {"description", "这一集的标题，六个字以内"}};
        props["logline"] = {
            {"type", "string"},
            {"description", "一句话说清这一集发生了什么，给人看的，不进成片"}};
        props["beats"] = {
            {"type", "array"},
            {"minItems", 4},
            {"description", "按时间顺序排的场次。动作和对白交替，不要连着五句对白"},
            {"items", items}};

        ordered out = ordered::object();
        out["type"] = "object";
        out["additionalProperties"] = false;
        out["required"] = {"title", "logline", "beats"};
        out["properties"] = props;
        return out;
    }();
    return s;
}

const ordered& premise_schema() {
    static const ordered s = [] {
        ordered idea_props = ordered::object();
        idea_props["title"] = {{"type", "string"},
                               {"description", "剧名，八个字以内"}};
        idea_props["premise"] = {
            {"type", "string"},
            {"description",
             "一两句话说清这部剧讲什么。要具体到人物和处境，不要写题材标签"}};
        idea_props["hook"] = {{"type", "string"},
                              {"description", "一句话说清观众为什么会看下去"}};

        ordered items = ordered::object();
        items["type"] = "object";
        items["additionalProperties"] = false;
        items["required"] = {"title", "premise", "hook"};
        items["properties"] = idea_props;

        ordered ideas = ordered::object();
        ideas["type"] = "array";
        ideas["minItems"] = 3;
        ideas["maxItems"] = 5;
        ideas["items"] = items;

        ordered props = ordered::object();
        props["ideas"] = ideas;

        ordered out = ordered::object();
        out["type"] = "object";
        out["additionalProperties"] = false;
        out["required"] = {"ideas"};
        out["properties"] = props;
        return out;
    }();
    return s;
}

// ---- 解析 ----

ScriptDraft parse_script(const std::string& raw) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw ScriptError(e.what());
    }
    if (!data.is_object()) throw ScriptError("大模型没有返回对象");

    json beats_raw = data.contains("beats") ? data["beats"] : json();
    if (!beats_raw.is_array() || beats_raw.empty()) {
        throw ScriptError("大模型没写出任何内容");
    }

    std::vector<Beat> beats;
    for (const auto& item : beats_raw) {
        if (!item.is_object()) continue;
        const std::string t =
            strip_wrapper(strip_leading_timecode(get_str(item, "text")));
        if (t.empty()) continue;

        std::string kind = get_str(item, "kind") == "dialogue" ? "dialogue"
                                                               : "action";
        const std::string speaker = normalize_speaker(get_str(item, "speaker"));
        if (kind == "dialogue" && speaker.empty()) {
            // 说了话却没说是谁说的，当旁白处理。丢掉的话这句台词
            // 就从成片里消失了，那比配错声音还糟。
            kind = "action";
        }
        beats.push_back(Beat{kind, speaker, t});
    }

    if (beats.empty()) throw ScriptError("大模型写的内容全是空的");
    const bool any_dialogue = std::any_of(
        beats.begin(), beats.end(),
        [](const Beat& b) { return b.kind == "dialogue"; });
    if (!any_dialogue) {
        throw ScriptError("整集一句台词都没有，这样出来的是默片");
    }

    ScriptDraft draft;
    draft.title = strip_ascii(get_str(data, "title"));
    draft.logline = strip_ascii(get_str(data, "logline"));
    draft.beats = std::move(beats);
    return draft;
}

std::vector<PremiseIdea> parse_premises(const std::string& raw) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw ScriptError(e.what());
    }

    json items;
    if (data.is_object()) {
        items = data.contains("ideas") ? data["ideas"] : json();
    } else {
        items = data;
    }
    if (!items.is_array() || items.empty()) {
        throw ScriptError("大模型没给出任何选题");
    }

    std::vector<PremiseIdea> ideas;
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        const std::string premise = strip_wrapper(get_str(item, "premise"));
        if (premise.empty()) continue;
        ideas.push_back(PremiseIdea{strip_wrapper(get_str(item, "title")),
                                    premise,
                                    strip_wrapper(get_str(item, "hook"))});
    }
    if (ideas.empty()) throw ScriptError("大模型给的选题全是空的");
    return ideas;
}

}  // namespace changji::stages
