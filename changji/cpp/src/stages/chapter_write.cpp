#include "stages/chapter_write.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "stages/chapter_write_prompt.inc.hpp"
#include "stages/json_extract.hpp"
#include "stages/story_import.hpp"
#include "stages/story_outline.hpp"
#include "stages/story_plan.hpp"
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

int index_of(const Story& story, const std::string& chapter_id) {
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        if (story.chapters[i].chapter_id == chapter_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// 在正文里找 needle，返回它结束之后那个**字符**位置；找不到返回 -1。
int find_after(const std::string& body, const std::string& needle) {
    const std::string n = text::strip_ws(needle);
    if (n.empty()) return -1;
    const std::size_t byte_pos = body.find(n);
    if (byte_pos == std::string::npos) return -1;
    return static_cast<int>(text::utf8_len(body.substr(0, byte_pos + n.size())));
}

}  // namespace

int chapter_target_chars(const Story& story) {
    const int cap = prose_budget_chars(story.episode_duration_s);
    return std::max(kChapterTargetChars, cap * kEpisodesPerChapter);
}

int chapter_hook_count(const Story& story) {
    const int cap = prose_budget_chars(story.episode_duration_s);
    if (cap <= 0) return kEpisodesPerChapter;
    const int n = chapter_target_chars(story) / cap;
    // 至少两个：只有一个的话就退化回「只有章尾」，那正是要修的毛病。
    return std::max(2, n);
}

const ordered& chapter_schema() {
    static const ordered schema = [] {
        ordered hook_props = ordered::object();
        hook_props["text"] = {
            {"type", "string"},
            {"description", "这里悬着的是什么：悬念、反转，或明确的情绪落点"}};
        hook_props["after"] = {
            {"type", "string"},
            {"description",
             "这个位置前面那句的原文，照抄十到二十个字。程序靠它定位切点"}};

        ordered props = ordered::object();
        props["text"] = {
            {"type", "string"},
            {"description",
             "这一章的**完整正文**，几千字连贯的叙述，小说体。不是标题、不是梗概、不是提纲，也不要剧本格式的标记"}};
        props["hooks"] = {
            {"type", "array"},
            {"description",
             "这一章里可以收一集的地方，按正文里的先后排。最后一个是章尾"},
            {"items", {{"type", "object"},
                       {"properties", hook_props},
                       {"required", {"text", "after"}},
                       {"additionalProperties", false}}}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {"text", "hooks"};
        s["additionalProperties"] = false;
        return s;
    }();
    return schema;
}

std::string build_chapter_prompt(const Story& story,
                                 const std::string& chapter_id,
                                 StyleLine style_line) {
    const int idx = index_of(story, chapter_id);
    if (idx < 0) throw StoryError("没有这一章：" + chapter_id);
    const Chapter& me = story.chapters[static_cast<std::size_t>(idx)];

    std::string out;
    out += prompt::kChapterSeg0;
    out += style_line == StyleLine::ANIME ? prompt::kChapterHintAnime
                                          : prompt::kChapterHintRealistic;
    out += prompt::kChapterSeg1;
    out += std::to_string(chapter_target_chars(story));
    out += prompt::kChapterSeg2;
    out += std::to_string(chapter_hook_count(story));
    out += prompt::kChapterSeg3;
    out += prompt::kChapterRules;
    out += prompt::kChapterContextHead;

    // ---- 压缩的全局记忆 ----
    if (!story.logline.empty()) out += "【这个故事】" + story.logline + "\n";
    if (!story.tone.empty()) out += "【调子】" + story.tone + "\n";
    out += "\n【人物】\n";
    for (const auto& c : story.characters) {
        out += c.name;
        if (!c.identity.empty()) out += "：" + c.identity;
        out += "。";
        if (!c.want.empty()) out += "他要的是：" + c.want + "。";
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
    if (!story.locations.empty()) {
        out += "\n【地方】\n";
        for (const auto& l : story.locations) {
            out += l.name;
            if (!l.what.empty()) out += "：" + l.what;
            out += "。";
            if (!l.when.empty()) out += l.when + "。";
            out += "\n";
        }
    }

    // ---- 前情：之前每章一句 ----
    //
    // 不是前面所有章的正文。那和逐集续写的失忆是同一个道理——二十章的正文
    // 谁也塞不下，截断之后早的那些照样丢。
    std::string recap;
    for (int i = 0; i < idx; ++i) {
        const Chapter& c = story.chapters[static_cast<std::size_t>(i)];
        recap += std::to_string(i + 1) + " " + c.title;
        if (!c.summary.empty()) recap += "：" + text::collapse_ws(c.summary);
        recap += "\n";
    }
    if (!recap.empty()) {
        out += "\n【前情提要】（已经发生过的，不要重写）\n";
        out += text::truncate_utf8(recap, prompt::kChapterRecapMaxChars);
    }

    // ---- 上一章的结尾，用来接语气 ----
    if (idx > 0) {
        const Chapter& prev = story.chapters[static_cast<std::size_t>(idx - 1)];
        const std::string tail = text::strip_ws(prev.text);
        if (!tail.empty()) {
            out += "\n【上一章是这么结束的】\n";
            const std::vector<std::string> chars = text::utf8_chars(tail);
            std::string piece;
            const std::size_t from =
                chars.size() > prompt::kChapterPrevTailMaxChars
                    ? chars.size() - prompt::kChapterPrevTailMaxChars
                    : 0;
            for (std::size_t i = from; i < chars.size(); ++i) piece += chars[i];
            out += text::strip_ws(piece);
            out += "\n";
        }
    }

    // ---- 这一章要写的 ----
    out += "\n【这一章】" + me.title + "\n";
    if (!me.summary.empty()) out += me.summary + "\n";
    if (!me.hooks.empty() && !me.hooks.back().text.empty()) {
        out += "\n【这一章要停在】" + me.hooks.back().text + "\n";
    }

    out += prompt::kChapterTail;
    return out;
}

ChapterDraft parse_chapter(const std::string& raw, int min_chars) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryError(e.what());
    }
    if (!data.is_object()) throw StoryError("大模型没有返回对象");

    ChapterDraft d;
    d.text = text::strip_ws(get_str(data, "text"));
    if (d.text.empty()) throw StoryError("大模型没写出正文");
    // 失控往下写个没完的时候截住。这段正文会整份存进 story.json，
    // 而且后面每一集的提示词都要读它。
    d.text = text::truncate_utf8(d.text, prompt::kChapterMaxChars);

    // **短得离谱的不收。** 见 kChapterMinRatio：模型会把章标题填进正文
    // 字段，一两个字也是合法 JSON，静默存下去的话故事看着有几章、
    // 实际全是空壳，到写剧本那一步才发现无米下锅。
    const int got = static_cast<int>(text::utf8_len(d.text));
    if (min_chars > 0 && got < min_chars) {
        throw StoryError("正文只写出 " + std::to_string(got) + " 个字，至少要 " +
                         std::to_string(min_chars) + " 个。八成是模型没听懂，重试一次");
    }

    const auto hooks = data.find("hooks");
    if (hooks != data.end() && hooks->is_array()) {
        for (const auto& h : *hooks) {
            DraftHook dh;
            dh.text = text::clean_field(get_str(h, "text"));
            dh.after = text::strip_ws(get_str(h, "after"));
            if (dh.text.empty()) continue;
            d.hooks.push_back(std::move(dh));
        }
    }
    // 老形状：只有一个 hook_after，说法在大纲那一章上。留着是因为改 schema
    // 之前存下来的草稿还可能走到这儿。
    const std::string legacy = text::strip_ws(get_str(data, "hook_after"));
    if (d.hooks.empty() && !legacy.empty()) {
        DraftHook dh;
        dh.after = legacy;
        d.hooks.push_back(std::move(dh));
    }
    return d;
}

Story apply_chapter(const Story& story, const std::string& chapter_id,
                    const ChapterDraft& draft) {
    Story out = story;
    Chapter* me = out.chapter_by_id(chapter_id);
    if (me == nullptr) throw StoryError("没有这一章：" + chapter_id);

    // 大纲那个钩子的说法要留着——它是**这一章整体**该停在哪，和中间几集
    // 收在哪不是一回事，所以它归章尾。
    std::string chapter_hook;
    for (const auto& h : me->hooks) {
        if (!h.text.empty()) chapter_hook = h.text;
    }

    me->text = draft.text;

    // **钩子全部重建。** 原来那些位置是对着空正文算出来的（大纲阶段一律
    // at_char = 0），正文落进去之后它们一个都不成立了，留着会让分集把刀
    // 切在章首。
    me->hooks = paragraph_hooks(me->text);
    const int len = me->text_len();

    // 在已有候选上补说法；那个位置还没有候选就新加一个。
    const auto put = [&](int at, const std::string& why) {
        if (why.empty()) return;
        if (at < 0 || at > len) at = len;
        for (auto& h : me->hooks) {
            if (h.at_char == at) {
                // 同一个位置已经有说法了就不覆盖：先到的是模型按先后给的，
                // 后到的多半是章尾那一个，盖掉等于把中间那集的钩子丢了。
                if (h.text.empty()) h.text = why;
                return;
            }
        }
        Hook h;
        h.at_char = at;
        h.text = why;
        me->hooks.push_back(std::move(h));
    };

    for (const auto& dh : draft.hooks) {
        // 查不到就不放：一章有好几个钩子，查不到的那个要是都堆到章尾，
        // 章尾会被一个中间情节的说法占掉。**只有章尾那一个值得兜底。**
        const int at = find_after(me->text, dh.after);
        if (at >= 0) put(at, dh.text);
    }
    // 章尾兜底：大纲给的那句挂上去，模型自己标了章尾就不动它。
    put(len, chapter_hook);

    return out;
}

}  // namespace changji::stages
