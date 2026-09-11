#include "stages/bible.hpp"

#include <string>
#include <vector>

#include "stages/bible_prompt.inc.hpp"
#include "stages/bible_story_prompt.inc.hpp"
#include "stages/json_extract.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

/// 从 JSON 对象里取字符串，缺了或不是字符串就返回空串。
///
/// 对应 Python 的 item.get("x", "")。不抛异常是刻意的：
/// 模型漏字段是常态，缺一个 body 不该让整份圣经作废。
std::string get_str(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

}  // namespace

const ordered& bible_schema() {
    // 手写而不是从 Python 导出，因为它要和 Python 的字典**结构**一致，
    // 不要求序列化字节一致——远端服务收到的是解析后的对象。
    // 用 ordered_json 是为了字段顺序稳定，方便出问题时和 Python 的
    // 请求体逐行 diff。
    static const ordered schema = [] {
        ordered character_props = ordered::object();
        character_props["key"] = {{"type", "string"},
                                  {"description", "英文小写下划线短标识，如 lin_wan"}};
        character_props["name"] = {{"type", "string"},
                                   {"description", "剧本里的中文称呼"}};
        character_props["identity"] = {{"type", "string"},
                                       {"description", "身份：性别、年龄段、气质。一句话"}};
        character_props["body"] = {{"type", "string"}, {"description", "体型和身高感"}};
        character_props["face"] = {
            {"type", "string"},
            {"description", "五官、发型、发色、瞳色。这段会在几十个镜头里逐字复用，写得具体且不要含糊"}};
        character_props["attire"] = {{"type", "string"}, {"description", "默认服装"}};

        ordered location_props = ordered::object();
        location_props["key"] = {{"type", "string"}, {"description", "英文小写下划线短标识"}};
        location_props["name"] = {{"type", "string"}, {"description", "中文场景名"}};
        location_props["space"] = {{"type", "string"}, {"description", "空间结构和布景"}};
        location_props["lighting"] = {{"type", "string"}, {"description", "光线基调"}};
        location_props["palette"] = {{"type", "string"}, {"description", "色彩方案"}};

        ordered props = ordered::object();
        props["characters"] = {
            {"type", "array"},
            {"description", "剧本里所有有名有姓或有台词的角色"},
            {"items", {{"type", "object"},
                       {"properties", character_props},
                       {"required", {"key", "name", "identity", "face", "attire"}},
                       {"additionalProperties", false}}}};
        props["locations"] = {
            {"type", "array"},
            {"description", "剧本里出现的场景"},
            {"items", {{"type", "object"},
                       {"properties", location_props},
                       {"required", {"key", "name", "space", "lighting"}},
                       {"additionalProperties", false}}}};
        props["global_style"] = {{"type", "string"},
                                 {"description", "全剧统一的画风、色温、质感。一句话"}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {"characters", "locations", "global_style"};
        s["additionalProperties"] = false;
        return s;
    }();
    return schema;
}

std::string build_bible_prompt(const std::string& script, StyleLine style_line) {
    // 各段来自 bible_prompt.inc.hpp，那个文件是从 Python 生成的。
    const char* hint = style_line == StyleLine::ANIME ? prompt::kBibleHintAnime
                                                      : prompt::kBibleHintRealistic;
    std::string out;
    out += prompt::kBiblePrefix;
    out += hint;
    out += prompt::kBibleMiddle;
    out += script;
    out += prompt::kBibleTail;
    return out;
}

std::string render_story_for_bible(const Story& story) {
    std::string out;

    // 调子先写。美术看这几行定色温和质感，后面每个人每个地方都在这个
    // 调子下面。放在名单后面的话，模型已经把人写完了才读到"荒诞"。
    if (!story.logline.empty()) out += "【这个故事】" + story.logline + "\n";
    if (!story.genre.empty()) out += "【题材】" + story.genre + "\n";
    if (!story.tone.empty()) out += "【调子】" + story.tone + "\n";
    if (!out.empty()) out += "\n";

    out += "【人物】\n";
    for (const auto& c : story.characters) {
        out += c.name;
        if (!c.identity.empty()) out += "：" + c.identity;
        out += "。";
        // 欲望和弧线给进去是为了让美术判断气质（想复仇的人和想赎罪的人
        // 眼神不一样）。提示词里另外写死了"不要把它们写进外观"。
        if (!c.want.empty()) out += "他要的是：" + c.want + "。";
        if (!c.arc.empty()) out += "他会从" + c.arc + "。";
        out += "\n";
    }

    if (!story.relations.empty()) {
        out += "\n【关系】\n";
        for (const auto& r : story.relations) {
            out += r.a + " — " + r.b;
            if (!r.kind.empty()) out += "：" + r.kind;
            out += "。";
            if (!r.tension.empty()) out += r.tension + "。";
            out += "\n";
        }
    }

    out += "\n【地点】\n";
    for (const auto& l : story.locations) {
        out += l.name;
        if (!l.what.empty()) out += "：" + l.what;
        out += "。";
        if (!l.when.empty()) out += l.when + "。";
        out += "\n";
    }

    // 分章只给调子用，所以可以截。人物表和地点表不截——它们是名单，
    // 截掉一半等于漏掉几个人，而漏掉的那个后面分镜里指不到。
    std::string chapters;
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        const auto& c = story.chapters[i];
        chapters += std::to_string(i + 1) + " " + c.title;
        if (!c.summary.empty()) {
            chapters += "：" + text::collapse_ws(c.summary);
        }
        chapters += "\n";
    }
    if (!chapters.empty()) {
        out += "\n【分章】\n";
        out += text::truncate_utf8(chapters, prompt::kBibleChaptersMaxChars);
    }

    return out;
}

std::string build_bible_prompt_from_story(const Story& story,
                                          StyleLine style_line) {
    const char* hint = style_line == StyleLine::ANIME ? prompt::kBibleHintAnime
                                                      : prompt::kBibleHintRealistic;
    std::string out;
    out += prompt::kBibleStoryPrefix;
    out += hint;
    out += prompt::kBibleStoryMiddle;
    out += render_story_for_bible(story);
    out += prompt::kBibleStoryTail;
    return out;
}

std::string default_negative(StyleLine style_line) {
    static const std::string base =
        "低质量，模糊，过曝，畸形，多余的手指，画得不好的手部，"
        "画得不好的脸部，静止不动的画面，字幕，水印";
    if (style_line == StyleLine::ANIME) return base + "，写实，照片质感，真人";
    // ⚠️ **写实线这儿不加"压卡通"。** 试过加一句
    // 「3D 渲染，卡通，动画，插画，CG 质感」——治的是同一个病（没有画风词
    // 时模型爱往 3D 卡通跑），但这串字是和 Python 逐字节对拍的接口输出，
    // 一改三条对拍用例当场红。
    //
    // 正向那边已经有底子了（StyleProfile 的 default_style，读资产库时补进
    // global_style），够用；真要再压一道，项目页那个「负向」框就是干这个
    // 的，而且写在那儿用户看得见。
    return base;
}

AssetLibrary parse_bible(const std::string& raw, StyleLine style_line,
                         const std::string& aspect_ratio) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw BibleError(e.what());
    }
    if (!data.is_object()) throw BibleError("大模型没有返回对象");

    AssetLibrary lib;

    // 角色。voice_order 用的是**遍历下标**而不是插入序号——
    // key 为空的会被 continue 跳过，但下标照样加。这是 Python 的行为
    // （enumerate 在 continue 之前就取好了 index），照抄。
    int index = 0;
    if (data.contains("characters") && data["characters"].is_array()) {
        for (const auto& item : data["characters"]) {
            const int this_index = index++;
            const std::string raw_key = get_str(item, "key").empty()
                                            ? get_str(item, "name")
                                            : get_str(item, "key");
            const std::string key = text::slug(raw_key);
            if (key.empty()) continue;

            const std::string char_id = "c_" + key;
            Character c;
            c.char_id = char_id;
            c.name = get_str(item, "name").empty() ? key : get_str(item, "name");
            // 音色留空，配音时按服务端实际有哪些参考音频再定。
            // 这里写死路径的话，换一台推理服务就可能对不上，
            // 节点校验不过整条流水线直接断在配音这一步。
            c.voice_id = std::nullopt;
            c.voice_gender = guess_gender(get_str(item, "identity"));
            c.voice_order = this_index;
            c.appearance.identity = text::clean_field(get_str(item, "identity"));
            c.appearance.body = text::clean_field(get_str(item, "body"));
            c.appearance.face = text::clean_field(get_str(item, "face"));
            c.appearance.attire = text::clean_field(get_str(item, "attire"));

            std::vector<std::string> errs;
            c.validate(errs);
            if (!errs.empty()) {
                throw BibleError("角色 " + key + " 的设定不合法：" + errs.front());
            }
            lib.characters[char_id] = std::move(c);
        }
    }

    if (data.contains("locations") && data["locations"].is_array()) {
        for (const auto& item : data["locations"]) {
            const std::string raw_key = get_str(item, "key").empty()
                                            ? get_str(item, "name")
                                            : get_str(item, "key");
            const std::string key = text::slug(raw_key);
            if (key.empty()) continue;

            const std::string loc_id = "loc_" + key;
            Location l;
            l.location_id = loc_id;
            l.name = get_str(item, "name").empty() ? key : get_str(item, "name");
            l.space = text::clean_field(get_str(item, "space"));
            l.lighting = text::clean_field(get_str(item, "lighting"));
            l.palette = text::clean_field(get_str(item, "palette"));

            std::vector<std::string> errs;
            l.validate(errs);
            if (!errs.empty()) {
                throw BibleError("场景 " + key + " 的设定不合法：" + errs.front());
            }
            lib.locations[loc_id] = std::move(l);
        }
    }

    if (lib.characters.empty()) {
        throw BibleError(
            "大模型没有产出任何角色。检查剧本里是否真的有人物，"
            "或者换一个更强的模型");
    }

    lib.style.style_line = style_line;
    lib.style.global_style = text::clean_field(get_str(data, "global_style"));
    lib.style.negative_prompt = default_negative(style_line);
    lib.style.aspect_ratio = aspect_ratio;
    return lib;
}

}  // namespace changji::stages
