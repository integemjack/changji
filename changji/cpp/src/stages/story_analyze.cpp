#include "stages/story_analyze.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "stages/json_extract.hpp"
#include "stages/story_analyze_prompt.inc.hpp"
#include "stages/story_outline.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

std::string get_str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

std::vector<std::string> get_str_array(const json& obj, const char* key) {
    std::vector<std::string> out;
    if (!obj.is_object()) return out;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (!v.is_string()) continue;
        const std::string s = text::clean_field(v.get<std::string>());
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

std::string head_chars(const std::vector<std::string>& chars, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n && i < chars.size(); ++i) out += chars[i];
    return out;
}

std::string tail_chars(const std::vector<std::string>& chars, std::size_t n) {
    std::string out;
    const std::size_t from = chars.size() > n ? chars.size() - n : 0;
    for (std::size_t i = from; i < chars.size(); ++i) out += chars[i];
    return out;
}

/// 在正文里找 needle，返回它**结束之后**那个字符位置；找不到返回 -1。
///
/// 按字符比，不按字节：返回值要当 Hook::at_char 用，而那个位置是按字符算的。
int find_after(const std::string& body, const std::string& needle) {
    const std::string n = text::strip_ws(needle);
    if (n.empty()) return -1;
    const std::size_t byte_pos = body.find(n);
    if (byte_pos == std::string::npos) return -1;
    // 字节位置换成字符位置：数前面有多少个 UTF-8 首字节。
    const std::string prefix = body.substr(0, byte_pos + n.size());
    return static_cast<int>(text::utf8_len(prefix));
}

}  // namespace

std::string render_chapters_for_analysis(const Story& story) {
    const std::size_t n = story.chapters.size();
    if (n == 0) return {};

    std::size_t per = prompt::kAnalyzeBudgetChars / n;
    if (per < prompt::kAnalyzeMinPerChapter) per = prompt::kAnalyzeMinPerChapter;
    // 开头给得比结尾多：结尾只要够看出钩子落在哪，开头要交代清楚人和处境。
    const std::size_t head_n = per * 2 / 3;
    const std::size_t tail_n = per - head_n;

    std::string out;
    for (const auto& c : story.chapters) {
        out += "【" + c.chapter_id + "】" + c.title + "\n";
        const std::vector<std::string> chars = text::utf8_chars(c.text);
        if (chars.size() <= per) {
            out += text::strip_ws(c.text);
        } else {
            out += text::strip_ws(head_chars(chars, head_n));
            // 省略号要标出来。不标的话模型会把断开的两段当成连着的一句，
            // 归纳出一个正文里根本没有的情节。
            out += "\n……（中间略）……\n";
            out += text::strip_ws(tail_chars(chars, tail_n));
        }
        out += "\n\n";
    }
    return out;
}

const ordered& analyze_schema() {
    static const ordered schema = [] {
        // 人物、关系、地点三块和大纲那份**故意长一样**：下游（bible、
        // 剧本提示词）认的是同一个形状，两边不一致的话每个消费者都要分支。
        const ordered& outline = outline_schema();
        ordered props = ordered::object();
        props["logline"] = outline.at("properties").at("logline");
        props["genre"] = outline.at("properties").at("genre");
        props["tone"] = outline.at("properties").at("tone");
        props["characters"] = outline.at("properties").at("characters");
        props["relations"] = outline.at("properties").at("relations");
        props["locations"] = outline.at("properties").at("locations");

        ordered chapter_props = ordered::object();
        chapter_props["chapter_id"] = {
            {"type", "string"}, {"description", "照抄给你的那个，不要改"}};
        chapter_props["summary"] = {
            {"type", "string"}, {"description", "这一章发生了什么，三五句，归纳不是摘抄"}};
        chapter_props["hook"] = {
            {"type", "string"},
            {"description", "这一章结束时悬着的那件事：悬念、反转，或明确的情绪落点"}};
        chapter_props["hook_after"] = {
            {"type", "string"},
            {"description",
             "钩子前面最后一句的原文，照抄十到二十个字。程序靠它定位切点"}};
        chapter_props["characters"] = {
            {"type", "array"},
            {"description", "这一章出场的人物名"},
            {"items", {{"type", "string"}}}};
        chapter_props["locations"] = {
            {"type", "array"},
            {"description", "这一章用到的地点名"},
            {"items", {{"type", "string"}}}};

        props["chapters"] = {
            {"type", "array"},
            {"description", "每一章一条，chapter_id 照抄，不要漏章"},
            {"items", {{"type", "object"},
                       {"properties", chapter_props},
                       {"required", {"chapter_id", "summary", "hook"}},
                       {"additionalProperties", false}}}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {"characters", "chapters"};
        s["additionalProperties"] = false;
        return s;
    }();
    return schema;
}

std::string build_analyze_prompt(const Story& story, StyleLine style_line) {
    std::string out;
    out += prompt::kAnalyzeSeg0;
    out += style_line == StyleLine::ANIME ? prompt::kAnalyzeHintAnime
                                          : prompt::kAnalyzeHintRealistic;
    out += prompt::kAnalyzeSeg1;
    out += prompt::kAnalyzeRules;
    out += prompt::kAnalyzeChaptersHead;
    out += render_chapters_for_analysis(story);
    out += prompt::kAnalyzeTail;
    return out;
}

Story apply_analysis(const Story& story, const std::string& raw) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryError(e.what());
    }
    if (!data.is_object()) throw StoryError("大模型没有返回对象");

    // 从原来那份出发：正文、章名、章节 id、分集表全部照旧。
    Story out = story;
    const std::string logline = text::clean_field(get_str(data, "logline"));
    if (!logline.empty()) out.logline = logline;
    const std::string genre = text::clean_field(get_str(data, "genre"));
    if (!genre.empty()) out.genre = genre;
    const std::string tone = text::clean_field(get_str(data, "tone"));
    if (!tone.empty()) out.tone = tone;

    std::set<std::string> names;
    out.characters.clear();
    const auto chars = data.find("characters");
    if (chars != data.end() && chars->is_array()) {
        for (const auto& c : *chars) {
            StoryCharacter sc;
            sc.name = text::clean_field(get_str(c, "name"));
            if (sc.name.empty() || !names.insert(sc.name).second) continue;
            sc.identity = text::clean_field(get_str(c, "identity"));
            sc.want = text::clean_field(get_str(c, "want"));
            sc.arc = text::clean_field(get_str(c, "arc"));
            out.characters.push_back(std::move(sc));
        }
    }
    if (out.characters.empty()) {
        throw StoryError("大模型没从正文里读出任何人物");
    }

    out.relations.clear();
    const auto rels = data.find("relations");
    if (rels != data.end() && rels->is_array()) {
        for (const auto& r : *rels) {
            Relation rel;
            rel.a = text::clean_field(get_str(r, "a"));
            rel.b = text::clean_field(get_str(r, "b"));
            // 两端都得是登记过的人，和大纲那条一样：指向不存在的人时，
            // 提示词里会凭空多出一个角色。
            if (names.count(rel.a) == 0 || names.count(rel.b) == 0) continue;
            rel.kind = text::clean_field(get_str(r, "kind"));
            rel.tension = text::clean_field(get_str(r, "tension"));
            out.relations.push_back(std::move(rel));
        }
    }

    std::set<std::string> loc_names;
    out.locations.clear();
    const auto locs = data.find("locations");
    if (locs != data.end() && locs->is_array()) {
        for (const auto& l : *locs) {
            StoryLocation sl;
            sl.name = text::clean_field(get_str(l, "name"));
            if (sl.name.empty() || !loc_names.insert(sl.name).second) continue;
            sl.what = text::clean_field(get_str(l, "what"));
            sl.when = text::clean_field(get_str(l, "when"));
            out.locations.push_back(std::move(sl));
        }
    }

    const auto chaps = data.find("chapters");
    if (chaps == data.end() || !chaps->is_array()) return out;
    for (const auto& c : *chaps) {
        const std::string id = text::strip_ws(get_str(c, "chapter_id"));
        Chapter* target = out.chapter_by_id(id);
        if (target == nullptr) continue;  // 模型编了个不存在的章号

        const std::string summary = text::strip_ws(get_str(c, "summary"));
        if (!summary.empty()) target->summary = summary;

        for (const auto& n : get_str_array(c, "characters")) {
            if (names.count(n)) target->characters.push_back(n);
        }
        for (const auto& n : get_str_array(c, "locations")) {
            if (loc_names.count(n)) target->locations.push_back(n);
        }

        const std::string hook = text::clean_field(get_str(c, "hook"));
        if (hook.empty()) continue;

        // 钩子落在哪：拿模型抄的那句原文去正文里查。查不到就挂章尾——
        // 章尾本来就是合法切点，比把钩子丢掉强。
        const int len = target->text_len();
        int at = find_after(target->text, get_str(c, "hook_after"));
        if (at < 0 || at > len) at = len;

        // 同一个位置已经有机械切点了就把说法补上去，别多挂一个。
        bool merged = false;
        for (auto& h : target->hooks) {
            if (h.at_char == at) {
                h.text = hook;
                merged = true;
                break;
            }
        }
        if (!merged) {
            Hook h;
            h.at_char = at;
            h.text = hook;
            target->hooks.push_back(std::move(h));
        }
    }

    return out;
}

}  // namespace changji::stages
