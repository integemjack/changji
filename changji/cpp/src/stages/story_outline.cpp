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
        // 紧跟着 want：**要什么和怕什么是一对**，挨着填模型才会让它们互相
        // 顶着（他想要的那样东西，正好要他做最怕的那件事）。
        character_props["fear"] = {
            {"type", "string"},
            {"description",
             "他怕什么：怕被谁看见什么、怕失去什么、怕自己其实是什么样的人。不是「怕黑」这种毛病，是会让他在关键时刻躲开、说谎、突然翻脸的那件事"},
            {"minLength", 8}};
        character_props["arc"] = {
            {"type", "string"}, {"description", "从什么变成什么"}};
        // **minLength 不能省。** 写进 required 只保证这个键在，不保证它有
        // 内容——2026-09-12 实跑，三个人物的 voice 全是空串，这一栏等于
        // 没加，而那一轮的「改进」实际上一个字都没生效。空字符串是合法的
        // JSON 字符串，语法采样照样让它过。
        character_props["voice"] = {
            {"type", "string"},
            {"description",
             "他说话什么样：长句还是短句、认不认错、生气时是提高声音还是干脆不说话、有没有嘴上的习惯。写得出来才分得清谁在说话。不要写长相"},
            {"minLength", 8}};

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
        // **排在 summary 前面。** 模型顺着往下生成：先定这一章抖出什么，
        // 后面那几句梗概才会围着它写。反过来的话它先把梗概写完，再回头
        // 凑一个「反转」，凑出来的多半是梗概里那件事换个说法。
        chapter_props["reveal"] = {
            {"type", "string"},
            {"description",
             "这一章抖出来的那件新事，要推翻前面的认知：谁其实是谁（身份）、两人其实是什么关系、当年那件事其实不是那样（事实）、他这么做其实为了什么（动机）。四选一。不是「又见了一面」「又谈了一次」"}};
        chapter_props["summary"] = {
            {"type", "string"}, {"description", "这一章发生什么，三五句"}};
        chapter_props["hook"] = {
            {"type", "string"},
            {"description", "这一章结束时悬着的那件事：悬念、反转，或明确的情绪落点"}};
        // ⚠️ **没写进 required 的字段，14B 一律不写。**
        //
        // 2026-09-11 实跑：出一份大纲 + 读一遍正文，两边都是
        // `locations: []`、每一章的 `characters` 和 `locations` 也都是空的
        // ——而人物那一项（在 required 里）给了三个、关系给了三条。不是模型
        // 读不懂，是语法采样里那些字段可以合法地不出现，它就不出现。
        //
        // 后果不是"少一点信息"：设定页的「场景」那一格永远是空的，空景图
        // 无从谈起；再往后排分镜时，不知道这一章在哪儿发生、谁在场——
        // 而那正是第二步全部的输入。
        //
        // 所以能定的都写进 required，再加 minItems。见
        // project-ai-chapter-quality 那条：这个体量的模型，只有 schema 管得住。
        // ⚠️ **这两项刻意不写进 required，也不设 minItems。**
        //
        // 试过。加上之后出一份大纲从三十几秒变成 **278 秒，而且最后截断在
        // 半截 JSON 上**（"大模型没有返回对象"）——语法一收紧，14B 就一路
        // 写到 token 上限也收不了口。大纲这一步的产出本来就长（四章，每章
        // 标题、梗概、钩子），再逼它给每章两份名单就过界了。
        //
        // 不加也不亏：每章谁在场、在哪儿，**「读故事」那一步是照着正文读
        // 出来的，比大纲阶段凭空想的准**，而那份 schema 里它们是 required
        // （见 story_analyze.cpp）。大纲这儿给了就收，没给也不拦。
        chapter_props["characters"] = {
            {"type", "array"},
            {"description", "这一章出场的人物名，照抄上面登记过的名字"},
            {"items", {{"type", "string"}}}};
        chapter_props["locations"] = {
            {"type", "array"},
            {"description", "这一章用到的地点名，照抄上面登记过的名字"},
            {"items", {{"type", "string"}}}};

        ordered props = ordered::object();
        // 用户没给梗概时，模型自己定的那个选题写在这里。给了梗概时这一项
        // 会被忽略——不让它改写用户写的那句话。
        props["premise"] = {
            {"type", "string"},
            {"description",
             "这部剧讲什么，一句话，具体到人物和处境。用户已经给了梗概时照抄"}};
        props["logline"] = {{"type", "string"}, {"description", "一句话说清这个故事"}};
        props["genre"] = {{"type", "string"}, {"description", "题材，如 都市情感"}};
        props["tone"] = {{"type", "string"}, {"description", "调子，如 克制、荒诞"}};
        props["characters"] = {
            {"type", "array"},
            {"description", "故事里的人。主要人物不超过三个"},
            {"items", {{"type", "object"},
                       {"properties", character_props},
                       {"required", {"name", "identity", "want", "fear", "voice"}},
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
            {"description", "故事里的地方。每个都要有人在那儿演戏，别列背景板"},
            {"minItems", 1},
            {"items", {{"type", "object"},
                       {"properties", location_props},
                       {"required", {"name", "what"}},
                       {"additionalProperties", false}}}};
        props["chapters"] = {
            {"type", "array"},
            {"description", "按顺序的章节。最后一章要把主线了结"},
            {"items", {{"type", "object"},
                       {"properties", chapter_props},
                       {"required", {"title", "reveal", "summary", "hook"}},
                       {"additionalProperties", false}}}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {"logline", "characters", "chapters", "locations",
                         "relations"};
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

    // 梗概可空：空着就让模型自己定选题。三个入口里只有「我自己有个想法」
    // 那条是从手写的一句话开始的，把它做成硬门槛等于又把人摁回空白框前面。
    const std::string p = text::strip_ws(premise);
    if (p.empty()) {
        out += prompt::kOutlineNoPremise;
    } else {
        out += prompt::kOutlineTailHead;
        out += text::truncate_utf8(p, prompt::kOutlinePremiseMaxChars);
    }
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
    // 用户给了就用用户的，一个字不动；没给才收模型自己定的那个选题。
    // 反过来（总是用模型回的）会让它悄悄改写用户写的那句话。
    std::string used_premise = text::strip_ws(premise);
    if (used_premise.empty()) {
        used_premise = text::strip_ws(get_str(data, "premise"));
    }
    story.premise = text::truncate_utf8(used_premise, 2000);
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
            sc.fear = text::clean_field(get_str(c, "fear"));
            sc.arc = text::clean_field(get_str(c, "arc"));
            sc.voice = text::clean_field(get_str(c, "voice"));
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
        ch.reveal = text::clean_field(get_str(c, "reveal"));
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
