#include "stages/chapter_write.hpp"

#include "stages/repetition.hpp"

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

int chapter_target_paras(const Story& story) {
    return std::max(10, chapter_target_chars(story) / kCharsPerParagraph);
}

int chapter_hook_count(const Story& story) {
    const int cap = prose_budget_chars(story.episode_duration_s);
    if (cap <= 0) return kEpisodesPerChapter;
    const int n = chapter_target_chars(story) / cap;
    // 至少两个：只有一个的话就退化回「只有章尾」，那正是要修的毛病。
    return std::max(2, n);
}

ordered chapter_schema(int target_paras) {
    // 段数的上下限：目标的三分之二到一倍半。下限让它没法一两段交差，上限让它
    // 没法写个没完（85 段目标 → 56~127 段，撑死四五千 token，离 8192 远）。
    const int min_items = std::max(10, target_paras * 2 / 3);
    const int max_items = std::max(min_items + 10, target_paras * 3 / 2);
    const ordered schema = [&] {
        ordered hook_props = ordered::object();
        hook_props["text"] = {
            {"type", "string"},
            {"description", "这里悬着的是什么：悬念、反转，或明确的情绪落点"}};
        hook_props["after"] = {
            {"type", "string"},
            {"description",
             "这个位置前面那句的原文，照抄十到二十个字。程序靠它定位切点"}};

        ordered props = ordered::object();
        props[kChapterBodyField] = {
            {"type", "array"},
            {"description",
             "这一章的**完整正文**，一段一项：一段一两句话、三四十个字，动作一段、对白一段，整章几千字。小说体。不是标题、不是梗概、不是提纲，也不要剧本格式的标记"},
            {"minItems", min_items},
            {"maxItems", max_items},
            // 每段至少 16 字：只卡段数时模型写 80 段十几个字的短句凑数，
            // 一章才一千三。真实网文段长中位 33，这是字数的杠杆——实测
            // 12 出 1500~1900 字，20 出 2000~2700 字。但 20 时最后一段想
            // 15 字收口，语法不让，它就拿「”'”””””」凑数；strip_quote_runs
            // 兜底，源头也别逼太紧，取中间。
            // 每段最长 300 字：真实网文最长的段也就三百来字。实跑时模型把
            // 一千多字的自言自语塞进了一段（见 parse_chapter 里那道闸）。
            {"items", {{"type", "string"}, {"minLength", 16}, {"maxLength", 300}}}};
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
        s["required"] = {kChapterBodyField, "hooks"};
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
    out += prompt::kChapterSeg1b;
    out += std::to_string(chapter_target_paras(story));
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

    if (idx == 0) out += prompt::kChapterFirstHead;

    // ---- 这一章要写的 ----
    out += "\n【这一章】" + me.title + "\n";
    if (!me.summary.empty()) out += me.summary + "\n";
    if (!me.hooks.empty() && !me.hooks.back().text.empty()) {
        out += "\n【这一章要停在】" + me.hooks.back().text + "\n";
    }

    out += prompt::kChapterTail;
    return out;
}

/// 模型在 JSON 字符串里不敢写 “”（以为要转义），整章对白全用 ‘’ 顶替。
/// 中文小说的对白是 “”，‘’ 只在引号套引号时出现。整章一个 “” 都没有而
/// 出现了 ‘’，就是这种情况，换回来；有 “” 的说明它分得清，不动。
static std::string normalize_quotes(std::string s) {
    if (s.find("“") != std::string::npos || s.find("”") != std::string::npos) return s;
    if (s.find("‘") == std::string::npos) return s;
    const auto swap = [&s](const std::string& from, const std::string& to) {
        std::string::size_type i = 0;
        while ((i = s.find(from, i)) != std::string::npos) {
            s.replace(i, from.size(), to);
            i += to.size();
        }
    };
    swap("‘", "“");
    swap("’", "”");
    return s;
}

/// 语法卡了每段的最短长度之后，模型想在下限之前收口时会用一串引号凑数
/// （实跑：「……面对一切了。”'”””””」）。中文正文里不存在三个以上连着的
/// 引号，整串删掉，一个两个的照旧。
static std::string strip_quote_runs(const std::string& s) {
    const auto is_quote = [](const std::string& ch) {
        return ch == "“" || ch == "”" || ch == "‘" || ch == "’" || ch == "\"" ||
               ch == "'";
    };
    const std::vector<std::string> chars = text::utf8_chars(s);
    std::string out;
    std::size_t i = 0;
    while (i < chars.size()) {
        if (!is_quote(chars[i])) {
            out += chars[i++];
            continue;
        }
        std::size_t j = i;
        while (j < chars.size() && is_quote(chars[j])) ++j;
        if (j - i < 3) {
            for (std::size_t k = i; k < j; ++k) out += chars[k];
        }
        i = j;
    }
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
    // 新形状：paragraphs 一段一项，拼回一段一行的正文。老形状（text 一个
    // 字符串）照样认——粘贴导入和改 schema 之前存的草稿走这条。
    if (const auto ps = data.find(kChapterBodyField);
        ps != data.end() && ps->is_array()) {
        for (const auto& p : *ps) {
            if (!p.is_string()) continue;
            const std::string one = text::strip_ws(p.get<std::string>());
            if (one.empty()) continue;
            if (!d.text.empty()) d.text += "\n";
            d.text += one;
        }
    } else {
        d.text = text::strip_ws(get_str(data, "text"));
    }
    d.text = normalize_quotes(strip_quote_runs(d.text));
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

    // **模型的自言自语不收。** 语法把它关在 JSON 字符串里，它想解释、想
    // 纠正自己的时候，那些话就落进某一段正文——实跑原样：一段 1164 字的
    // 「不符合用户要求的“只输出 JSON”，请忽略此部分内容……」。字数守卫、
    // 复读守卫都抓不到它。小说正文里不会出现这些词。
    for (const char* bad : {"JSON", "json", "请忽略", "用户要求", "输出应"}) {
        if (d.text.find(bad) != std::string::npos) {
            throw StoryError(std::string("正文里混进了模型的解释（出现「") + bad +
                             "」）。重试一次");
        }
    }

    // **复读不收。** 字数守卫抓不住它：实跑那次写了 1124 字、稳稳过了 600
    // 的下限，而「你早就走了，我只是还在等。」一字不差出现了八次。
    // 只量长度不看内容的话，这段东西会一路存进 story.json，再被切成集、
    // 写成剧本、排成分镜、配成音、渲成片——一整条流水线为一段复读机跑了
    // 一个多小时。和 reject_silent_audio 是同一类事。
    if (const auto rep = check_repetition(d.text); !rep.ok) {
        throw StoryError("正文在复读：" + rep.detail + "。重试一次");
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
