#include "stages/story_outline.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "stages/json_extract.hpp"
#include "stages/story_outline_prompt.inc.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

/// 从 JSON 对象里取字符串，缺了或不是字符串就返回空串。
///
/// 和 bible.cpp 的同名函数一个道理：模型漏字段是常态，
/// 缺一个 tone 不该让整份大纲作废。
std::string get_str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

/// 取字符串数组，非字符串项跳过。
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

std::string chapter_id(std::size_t i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "ch%02zu", i + 1);
    return buf;
}

}  // namespace

const ordered& outline_schema() {
    static const ordered schema = [] {
        ordered character_props = ordered::object();
        character_props["name"] = {
            {"type", "string"}, {"description", "剧本里的中文称呼，全剧一字不改"}};
        character_props["identity"] = {
            {"type", "string"}, {"description", "一句话身份。不要写长相、发型、服装"}};
        character_props["want"] = {
            {"type", "string"},
            {"description", "这个人物主动要的东西。写「希望生活变好」等于没写"}};
        character_props["arc"] = {
            {"type", "string"}, {"description", "从什么变成什么"}};

        ordered relation_props = ordered::object();
        relation_props["a"] = {{"type", "string"}, {"description", "人物名，必须在 characters 里"}};
        relation_props["b"] = {{"type", "string"}, {"description", "人物名，必须在 characters 里"}};
        relation_props["kind"] = {{"type", "string"}, {"description", "前任、母女、上下级"}};
        relation_props["tension"] = {
            {"type", "string"},
            {"description", "这段关系里绷着的是什么。只写关系名不够"}};

        ordered location_props = ordered::object();
        location_props["name"] = {{"type", "string"}, {"description", "中文地点名"}};
        location_props["what"] = {
            {"type", "string"}, {"description", "什么地方，具体到看得见"}};
        location_props["when"] = {
            {"type", "string"}, {"description", "什么时间、什么光"}};

        ordered chapter_props = ordered::object();
        chapter_props["title"] = {{"type", "string"}, {"description", "章标题，十个字以内"}};
        chapter_props["summary"] = {
            {"type", "string"}, {"description", "这一章发生什么，三五句"}};
        chapter_props["hook"] = {
            {"type", "string"},
            {"description", "这一章结束时悬着的那件事：悬念、反转，或明确的情绪落点"}};
        chapter_props["characters"] = {
            {"type", "array"},
            {"description", "这一章出场的人物名"},
            {"items", {{"type", "string"}}}};
        chapter_props["locations"] = {
            {"type", "array"},
            {"description", "这一章用到的地点名"},
            {"items", {{"type", "string"}}}};

        ordered props = ordered::object();
        props["logline"] = {{"type", "string"}, {"description", "一句话说清这个故事"}};
        props["genre"] = {{"type", "string"}, {"description", "题材，如 都市情感"}};
        props["tone"] = {{"type", "string"}, {"description", "调子，如 克制、荒诞"}};
        props["characters"] = {
            {"type", "array"},
            {"description", "故事里的人。主要人物不超过三个"},
            {"items", {{"type", "object"},
                       {"properties", character_props},
                       {"required", {"name", "identity", "want"}},
                       {"additionalProperties", false}}}};
        props["relations"] = {
            {"type", "array"},
            {"description", "人物之间有戏的关系，两端都要是上面登记过的人"},
            {"items", {{"type", "object"},
                       {"properties", relation_props},
                       {"required", {"a", "b", "kind", "tension"}},
                       {"additionalProperties", false}}}};
        props["locations"] = {
            {"type", "array"},
            {"description", "故事里的地方"},
            {"items", {{"type", "object"},
                       {"properties", location_props},
                       {"required", {"name", "what"}},
                       {"additionalProperties", false}}}};
        props["chapters"] = {
            {"type", "array"},
            {"description", "按顺序的章节。最后一章要把主线了结"},
            {"items", {{"type", "object"},
                       {"properties", chapter_props},
                       {"required", {"title", "summary", "hook"}},
                       {"additionalProperties", false}}}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {"logline", "characters", "chapters"};
        s["additionalProperties"] = false;
        return s;
    }();
    return schema;
}

std::string build_outline_prompt(const std::string& premise, StoryScale scale,
                                 StyleLine style_line,
                                 const std::string& keywords) {
    const char* hint = style_line == StyleLine::ANIME
                           ? prompt::kOutlineHintAnime
                           : prompt::kOutlineHintRealistic;
    std::string out;
    out += prompt::kOutlineSeg0;
    out += hint;
    out += prompt::kOutlineSeg1;
    out += std::to_string(suggested_chapters(scale));
    out += prompt::kOutlineSeg2;
    out += prompt::kOutlineRules;

    const std::string kw = text::strip_ws(keywords);
    if (!kw.empty()) {
        out += prompt::kOutlineKeywordsPre;
        out += text::truncate_utf8(kw, prompt::kOutlineKeywordsMaxChars);
        out += prompt::kOutlineKeywordsPost;
    }

    out += prompt::kOutlineTailHead;
    out += text::truncate_utf8(text::strip_ws(premise),
                               prompt::kOutlinePremiseMaxChars);
    out += prompt::kOutlineTailEnd;
    return out;
}

Story parse_outline(const std::string& raw, const std::string& premise,
                    StoryScale scale) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryError(e.what());
    }
    if (!data.is_object()) throw StoryError("大模型没有返回对象");

    Story story;
    story.premise = text::truncate_utf8(text::strip_ws(premise), 2000);
    story.scale = scale;
    story.source = StorySource::AI;
    story.logline = text::clean_field(get_str(data, "logline"));
    story.genre = text::clean_field(get_str(data, "genre"));
    story.tone = text::clean_field(get_str(data, "tone"));

    // 人物。名字是后面一切的键：分镜提示词按名字找角色，关系按名字连边。
    // 所以重名的只留第一个，没名字的直接丢——留下来会变成一个叫空串的角色。
    std::set<std::string> names;
    const auto chars = data.find("characters");
    if (chars != data.end() && chars->is_array()) {
        for (const auto& c : *chars) {
            if (story.characters.size() >= prompt::kMaxCharacters) break;
            StoryCharacter sc;
            sc.name = text::clean_field(get_str(c, "name"));
            if (sc.name.empty() || !names.insert(sc.name).second) continue;
            sc.identity = text::clean_field(get_str(c, "identity"));
            sc.want = text::clean_field(get_str(c, "want"));
            sc.arc = text::clean_field(get_str(c, "arc"));
            story.characters.push_back(std::move(sc));
        }
    }

    // 关系。两端都得是登记过的人——指向不存在的人时，界面上那条边画不出来，
    // 而提示词里会凭空多出一个角色，那正是要防的漂移。丢掉比留着强。
    const auto rels = data.find("relations");
    if (rels != data.end() && rels->is_array()) {
        for (const auto& r : *rels) {
            Relation rel;
            rel.a = text::clean_field(get_str(r, "a"));
            rel.b = text::clean_field(get_str(r, "b"));
            if (names.count(rel.a) == 0 || names.count(rel.b) == 0) continue;
            rel.kind = text::clean_field(get_str(r, "kind"));
            rel.tension = text::clean_field(get_str(r, "tension"));
            story.relations.push_back(std::move(rel));
        }
    }

    std::set<std::string> loc_names;
    const auto locs = data.find("locations");
    if (locs != data.end() && locs->is_array()) {
        for (const auto& l : *locs) {
            StoryLocation sl;
            sl.name = text::clean_field(get_str(l, "name"));
            if (sl.name.empty() || !loc_names.insert(sl.name).second) continue;
            sl.what = text::clean_field(get_str(l, "what"));
            sl.when = text::clean_field(get_str(l, "when"));
            story.locations.push_back(std::move(sl));
        }
    }

    const auto chaps = data.find("chapters");
    if (chaps == data.end() || !chaps->is_array() || chaps->empty()) {
        throw StoryError("大纲里一章都没有");
    }
    for (const auto& c : *chaps) {
        if (story.chapters.size() >= prompt::kMaxChapters) break;
        Chapter ch;
        ch.chapter_id = chapter_id(story.chapters.size());
        ch.title = text::clean_field(get_str(c, "title"));
        ch.summary = text::strip_ws(get_str(c, "summary"));
        if (ch.title.empty() && ch.summary.empty()) continue;

        // 大纲阶段没有正文，所以钩子的位置只能是 0——而正文为空时
        // 0 既是章首也是章尾，语义上就是「这一章结束时悬着的那件事」。
        // 校验那边要求 at_char ∈ [0, 正文长度]，这里正好落在边界上。
        const std::string hook = text::clean_field(get_str(c, "hook"));
        if (!hook.empty()) {
            Hook h;
            h.at_char = 0;
            h.text = hook;
            ch.hooks.push_back(std::move(h));
        }

        // 出场人物只留登记过的。模型经常在章节里蹦出一个没在 characters
        // 里登记的名字，留着的话后面按名字找角色会找不到。
        for (const auto& n : get_str_array(c, "characters")) {
            if (names.count(n)) ch.characters.push_back(n);
        }
        for (const auto& n : get_str_array(c, "locations")) {
            if (loc_names.count(n)) ch.locations.push_back(n);
        }
        story.chapters.push_back(std::move(ch));
    }
    if (story.chapters.empty()) throw StoryError("大纲里一章都没有");

    return story;
}

}  // namespace changji::stages
