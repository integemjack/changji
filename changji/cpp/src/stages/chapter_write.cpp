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

/// 去掉标点和引号，只留下字。比对「这两句是不是在说同一件事」用。
std::string bare(const std::string& s) {
    static const char* kDrop[] = {"“", "”", "‘", "’", "，", "。", "！", "？",
                                  "…", "、", "：", "；", "「", "」", "—",
                                  " ", "\n", "\t", "\"", "'"};
    std::string out = s;
    for (const char* d : kDrop) {
        std::string::size_type i = 0;
        while ((i = out.find(d, i)) != std::string::npos) {
            out.erase(i, std::string(d).size());
        }
    }
    return out;
}

/// 这两句是不是在说同一件事。
///
/// **为什么要这个：分集的钩子就是场的 turn，两集的钩子撞车是致命的。**
/// 2026-09-12 实跑：一章里场 1 的结尾和场 3 的 turn 都是「我回来不是为了
/// 道歉」，切出来 ep02 和 ep05 的钩子几乎一样；另一处是第三章的第一场把
/// 第二章的末场原样重演，ep08 和 ep09 的钩子一字不差。观众看到的是剧情
/// 在原地打转。
///
/// 判据刻意做得宽松：短的那句被长的那句包住，或者两句有十个字以上连着
/// 一样。十个汉字连着撞车，正常写作里几乎只会是复制粘贴。
bool same_beat(const std::string& a, const std::string& b) {
    const std::vector<std::string> x = text::utf8_chars(bare(a));
    const std::vector<std::string> y = text::utf8_chars(bare(b));
    if (x.size() < 8 || y.size() < 8) return false;

    const auto join = [](const std::vector<std::string>& v, std::size_t from,
                         std::size_t to) {
        std::string s;
        for (std::size_t i = from; i < to; ++i) s += v[i];
        return s;
    };
    const std::string sx = join(x, 0, x.size());
    const std::string sy = join(y, 0, y.size());
    if (sx.find(sy) != std::string::npos || sy.find(sx) != std::string::npos) {
        return true;
    }
    // 最长公共子串。turn 都是几十个字，平方级别的比法足够快。
    constexpr std::size_t kRun = 10;
    if (x.size() < kRun || y.size() < kRun) return false;
    for (std::size_t i = 0; i + kRun <= x.size(); ++i) {
        if (sy.find(join(x, i, i + kRun)) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

bool scenes_repeat_beat(const std::string& a, const std::string& b) {
    return same_beat(a, b);
}

int chapter_target_chars(const Story& story) {
    const int cap = prose_budget_chars(story.episode_duration_s);
    return std::max(kChapterTargetChars, cap * kEpisodesPerChapter);
}

int chapter_target_scenes(const Story& story) {
    // **按这一章有多少字算，不按它会被切成几集。**
    //
    // 第一版是按集数算的（一场一集，切点最整齐）。2026-09-12 实跑当场露馅：
    // 每集 30 秒时一章要 4 场，而一章的梗概只撑得起一两件事——模型就从
    // 全局地点表里抓了下一章的地方来凑，四章都在同一个楼顶演同一件事。
    //
    // 场是**故事的单位**，一场要有一千字上下才铺得开（谁想干什么、谁拦着、
    // 局面变成什么）。一场装不下一集就让分集在场里面再切一刀——切点还是
    // 优先往场尾靠（kSceneSlack），只是不再强求一场一集。
    return std::clamp(chapter_target_chars(story) / kSceneTargetChars, 2, 4);
}

int chapter_scene_chars(const Story& story) {
    return std::max(200, chapter_target_chars(story) / chapter_target_scenes(story));
}

int chapter_scene_paras(const Story& story) {
    return std::max(6, chapter_scene_chars(story) / kCharsPerParagraph);
}

ordered chapter_schema(int target_scenes, int paras_per_scene) {
    // 场数只让它往上多一场，不让往下少一场。少一场的话让它少写一场的话，那一场会长到一千八百字，分集只能
    // 在场中间连切四刀，每一刀都落在说不出为什么的地方（2026-09-12 实跑）。
    const int min_scenes = std::max(2, target_scenes);
    const int max_scenes = std::max(min_scenes + 1, target_scenes + 1);

    // 段数的上下限：目标的三分之二到一倍半。下限让它没法一两段交差，上限让它
    // 没法写个没完。
    const int min_items = std::max(6, paras_per_scene * 2 / 3);
    const int max_items = std::max(min_items + 6, paras_per_scene * 3 / 2);

    const ordered schema = [&] {
        // **顺序就是生成顺序。** 模型顺着往下写，先把这一场的底子填掉，
        // 后面那几百字才有地方落；turn 也排在正文前面，它才写得到那儿去。
        ordered scene_props = ordered::object();
        scene_props["where"] = {
            {"type", "string"},
            {"description", "这一场在哪、什么时候、什么光"},
            {"minLength", 4}};
        scene_props["pov"] = {
            {"type", "string"},
            {"description", "这一场跟着谁走。只有他心里想什么可以写"},
            {"minLength", 1}};
        scene_props["goal"] = {
            {"type", "string"},
            {"description", "他在这一场里想做成什么"},
            {"minLength", 4}};
        scene_props["obstacle"] = {
            {"type", "string"},
            {"description", "谁、什么拦着他"},
            {"minLength", 4}};
        scene_props["turn"] = {
            {"type", "string"},
            {"description",
             "这一场结束时局面变成什么，而且要悬着。这一场就是一集，它就是那一集的钩子。把事情了结掉的不算"},
            {"minLength", 6}};
        scene_props[kChapterBodyField] = {
            {"type", "array"},
            {"description",
             "这一场的正文，一段一项。一段推进一两秒钟的事：一个动作、一句话、一次看见。小说体，不是梗概也不是分镜。长短要错落，别段段一样长。写到上面那个 turn 发生的那一刻就停，不要再加一段点题或者总结"},
            {"minItems", min_items},
            {"maxItems", max_items},
            // 每段至少 16 字：只卡段数时模型写一堆十几个字的短句凑数，
            // 一章才一千三。这是字数的杠杆——实测 12 出 1500~1900 字，
            // 20 出 2000~2700 字。但 20 时最后一段想 15 字收口，语法不让，
            // 它就拿「”'”””””」凑数；strip_quote_runs 兜底，源头也别逼太紧。
            // 每段最长 300 字：真实网文最长的段也就三百来字。实跑时模型把
            // 一千多字的自言自语塞进了一段（见 parse_chapter 里那道闸）。
            {"items", {{"type", "string"}, {"minLength", 16}, {"maxLength", 300}}}};

        ordered props = ordered::object();
        props[kChapterScenesField] = {
            {"type", "array"},
            {"description",
             "这一章的几场戏，按先后。一场 = 一个地方、一段连着的时间里，谁想做成一件事、谁拦着、最后局面变了"},
            {"minItems", min_scenes},
            {"maxItems", max_scenes},
            {"items", {{"type", "object"},
                       {"properties", scene_props},
                       {"required", {"where", "pov", "goal", "obstacle", "turn",
                                     kChapterBodyField}},
                       {"additionalProperties", false}}}};

        ordered s = ordered::object();
        s["type"] = "object";
        s["properties"] = props;
        s["required"] = {kChapterScenesField};
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
    out += std::to_string(chapter_target_scenes(story));
    out += prompt::kChapterSeg1b;
    out += std::to_string(chapter_scene_chars(story));
    out += prompt::kChapterSeg2;
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
        // **上一章停在哪，单说一句。** 2026-09-12 实跑：第二章把第一章的
        // 第一场原样重演了一遍，两章第一场的 turn 一字不差。原文的结尾
        // 贴在上面它也看得到，但那是一段叙述，读不出"故事已经走到这儿了"；
        // 把最后一场的 turn 单拎出来，起点才是明确的。
        if (!prev.scenes.empty() && !prev.scenes.back().turn.empty()) {
            out += "**上一章停在这件事上：" + prev.scenes.back().turn +
                   "。这一章从它之后接着往下走，不要把它再演一遍。**\n";
        }
    }

    if (idx == 0) out += prompt::kChapterFirstHead;

    // ---- 这一章要写的 ----
    out += "\n【这一章】" + me.title + "\n";
    if (!me.summary.empty()) out += me.summary + "\n";
    if (!me.hooks.empty() && !me.hooks.back().text.empty()) {
        // **这是最后一场的 turn 该是什么的说法，不是一句正文。** 上一版
        // 写「这一章要停在」，14B 把那行字原样抄成了章尾（见 2026-09-11
        // 的记录）。挂到 turn 那一栏上，它才知道这是要它转写的一件事。
        out += "\n【最后一场的 turn 要落到这件事上】" + me.hooks.back().text + "\n";
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

ChapterDraft parse_chapter(const std::string& raw, int min_chars, bool strict) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryError(e.what());
    }
    if (!data.is_object()) throw StoryError("大模型没有返回对象");

    ChapterDraft d;

    // 一段落进正文。**分隔符必须和 JsonFieldStreamer::kArraySeparator 一致**，
    // 否则编辑器里边写边看的那一版和最后落库的段距对不上。
    const auto push_para = [&d](const json& p) {
        if (!p.is_string()) return;
        const std::string one = text::strip_ws(p.get<std::string>());
        if (one.empty()) return;
        if (!d.text.empty()) d.text += "\n";
        d.text += one;
    };

    // 现在的形状：scenes[] 一场一项，每一场自己带 paragraphs。
    // 老形状两种都还认——顶层 paragraphs（改 schema 之前的草稿）、
    // 顶层 text 一个字符串（更早的那一版）。
    if (const auto ss = data.find(kChapterScenesField);
        ss != data.end() && ss->is_array()) {
        for (const auto& s : *ss) {
            if (!s.is_object()) continue;
            DraftScene sc;
            sc.where = text::clean_field(get_str(s, "where"));
            sc.pov = text::clean_field(get_str(s, "pov"));
            sc.goal = text::clean_field(get_str(s, "goal"));
            sc.obstacle = text::clean_field(get_str(s, "obstacle"));
            sc.turn = text::clean_field(get_str(s, "turn"));
            if (const auto ps = s.find(kChapterBodyField);
                ps != s.end() && ps->is_array()) {
                for (const auto& p : *ps) {
                    if (!p.is_string()) continue;
                    const std::string one = text::strip_ws(p.get<std::string>());
                    if (one.empty()) continue;
                    sc.paragraphs.push_back(one);
                    push_para(p);
                }
            }
            // 一段都没写出来的场不留：留着的话它在场次表里占一个位置，
            // 分集会照着它在正文里切一刀，而那一刀落在上一场的末尾。
            if (sc.paragraphs.empty()) continue;
            d.scenes.push_back(std::move(sc));
        }
    } else if (const auto ps = data.find(kChapterBodyField);
               ps != data.end() && ps->is_array()) {
        for (const auto& p : *ps) push_para(p);
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

    // **情绪标签不收。** 实跑那一章（chapter_check8 / ch02）1674 个字里，
    // 「神情复杂」「眼中满是惊讶与疑问」「心中涌起难以言喻的情绪」
    // 「内心充满期待」这类说法出现了十几次——它们把感受替读者做完了，
    // 是这份正文读起来像分镜表而不像小说的主要原因之一。提示词里已经列了
    // 这些词，但措辞 14B 不一定听（2026-09-11 的教训），所以这儿再拦一道。
    //
    // 阈值按每千字放宽：偶尔冒一个是行文，成串出现才是这个毛病。全禁的话
    // 14B 会反复撞墙，一章要重试好几轮。
    {
        static const char* kLabels[] = {
            "神情复杂", "五味杂陈", "百感交集", "难以言喻", "若有所思",
            "不知所措", "心中涌起", "内心充满", "眼中满是", "眼里满是",
            "脸上满是", "久久不能平静", "意味深长", "百般滋味",
        };
        int hits = 0;
        std::string worst;
        for (const char* w : kLabels) {
            std::string::size_type i = 0;
            while ((i = d.text.find(w, i)) != std::string::npos) {
                ++hits;
                if (worst.empty()) worst = w;
                i += std::string(w).size();
            }
        }
        const int chars = static_cast<int>(text::utf8_len(d.text));
        // 每千字允许三个，且整章至少要够四个才算数。
        const int budget = std::max(4, chars * 3 / 1000);
        if (strict && hits > budget) {
            throw StoryError("正文在贴情绪标签（「" + worst + "」这类出现了 " +
                             std::to_string(hits) +
                             " 次）：这些词替读者把感受做完了，要写成身体在干什么。重试一次");
        }
    }

    // **一句对白都没有的不收。** 2026-09-12 实跑四章里有一章通篇零对白
    // （65 段全是叙述），而下一步是把这段正文改成剧本——正文里没人说话，
    // 那一集出来就是默片。和剧本那边「整集一句台词都没有」是同一道闸。
    //
    // **按场查，不按章查。** 第一版只查整章有没有对白，于是出现了「三场里
    // 两场有对白、第三场是一个人在厂房里回忆五百字」——那一场切出来就是
    // 一集默片，而整章的账是平的，查不出来。
    //
    // **只查按场写回来的那一份。** 老形状（顶层 text / paragraphs）是改
    // schema 之前的草稿和粘贴导入那条路——那些正文不是照着现在这份提示词
    // 写的，拿现在的规矩去卡它们只会把打得开的故事变成打不开的。
    // **允许有一场是安静的，不许多数场都安静。** 第一版是「每一场都必须有
    // 对白」，2026-09-12 实跑当场打脸：四章里三章连着两次被打回，最后是空的
    // ——一章里夹一个内心场是 14B 的常态，为它把整章废掉，得到的是零分而
    // 不是高分。闸门要拦的是「整章没人说话」，不是「有一场没人说话」。
    if (!d.scenes.empty()) {
        int spoken = 0;
        std::string::size_type q = 0;
        while ((q = d.text.find("“", q)) != std::string::npos) {
            ++spoken;
            q += std::string("“").size();
        }
        if (strict && spoken < 2 && text::utf8_len(d.text) > 400) {
            throw StoryError("整章一句对白都没有：正文里没人说话，改成剧本就是默片。重试一次");
        }
    }

    // **两场不能停在同一件事上。** 场的 turn 就是那一集的钩子，两集钩子
    // 撞车观众看到的是剧情在原地打转。连正文也一起比：实跑那次是场 1 的
    // 结尾那句话，被场 3 拿去当了 turn。
    for (std::size_t k = 0; strict && k < d.scenes.size(); ++k) {
        const std::string& t = d.scenes[k].turn;
        if (t.empty()) continue;
        for (std::size_t j = 0; j < k; ++j) {
            if (same_beat(t, d.scenes[j].turn)) {
                throw StoryError("第 " + std::to_string(j + 1) + " 场和第 " +
                                 std::to_string(k + 1) +
                                 " 场停在同一件事上（「" + t +
                                 "」）：这两集的钩子会一模一样。重试一次");
            }
            for (const auto& p : d.scenes[j].paragraphs) {
                if (!same_beat(t, p)) continue;
                throw StoryError("第 " + std::to_string(k + 1) +
                                 " 场的 turn 是第 " + std::to_string(j + 1) +
                                 " 场里已经写过的那句话（「" + t +
                                 "」）：同一件事演了两遍。重试一次");
            }
        }
    }

    // **章尾点题不收（软闸）。** 每一场的最后一段应该就是那个 turn，
    // 写到它发生那一刻就停。实跑里模型有一半的章会在 turn 后面再加一段
    // 总结——「那一刻，林夏知道……」「窗外的雨还在下，却再也无法打湿
    // 她的心」——那种句子一出来，悬念当场被填平，而那一段正好是分集
    // 切线落下的地方。
    if (strict && !d.scenes.empty()) {
        static const char* kWrapUp[] = {"终于", "从此", "那一刻", "这一刻",
                                        "但至少", "再也", "明白了", "放下了",
                                        "释然"};
        for (const DraftScene& sc : d.scenes) {
            if (sc.paragraphs.empty()) continue;
            const std::string& last = sc.paragraphs.back();
            for (const char* w : kWrapUp) {
                if (last.find(w) == std::string::npos) continue;
                throw StoryError(std::string("有一场的最后一段在点题（「") + w +
                                 "」）：写到 turn 发生那一刻就该停，"
                                 "后面那一段会把悬念填平。重试一次");
            }
        }
    }

    // **占位符不收。** 2026-09-12 实跑：schema 里写了「最后一段就是上面那个
    // turn」，模型把它当成元指令，最后一段吐出来的是
    // `turn_sentence_from_above_repeated_but_in_correct_place`——一串下划线
    // 连着的英文，原样落进正文、落进分集、落进字幕。中文正文里不会出现
    // 这种东西，见着就打回。
    {
        int run = 0;
        for (const char c : d.text) {
            const bool wordish = (c >= 'a' && c <= 'z') ||
                                 (c >= 'A' && c <= 'Z') || c == '_';
            run = wordish ? run + 1 : 0;
            if (run >= 16) {
                throw StoryError("正文里混进了占位符（一长串英文下划线）。重试一次");
            }
        }
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

    // **这一章不能停在上一章已经停过的地方。** 2026-09-12 实跑：第三章的
    // 第一场把第二章的末场原样重演了一遍，两场的 turn 一字不差——切出来
    // ep08 和 ep09 的钩子完全一样。提示词里已经把上一章的 turn 单拎了
    // 一行出来说"别再演一遍"，14B 照样演，所以这儿再拦一道。
    //
    // 放在 apply 而不是 parse：只有这里才看得到上一章。
    {
        const int idx = index_of(out, chapter_id);
        if (idx > 0) {
            const Chapter& prev = out.chapters[static_cast<std::size_t>(idx - 1)];
            if (!prev.scenes.empty()) {
                const std::string& last = prev.scenes.back().turn;
                for (const auto& sc : draft.scenes) {
                    if (last.empty() || !scenes_repeat_beat(sc.turn, last)) continue;
                    throw StoryError("这一章又停在上一章停过的地方（「" + sc.turn +
                                     "」）：上一章就是这么结束的，这一章要从它之后往下走。重试一次");
                }
            }
        }
    }

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

    // **场的位置是数出来的，不是模型报的。** 正文就是各场的段落顺次拼起来
    // 的，所以第几段结束就是第几场结束——`paragraph_breaks` 给的是每个段落
    // 边界的字符位置，累加段数一查就得到。
    //
    // 上一版靠模型抄一句原文回来（DraftHook::after），程序再去正文里查，
    // 抄错一个字那一集就落不下去，只能收在一个说不出为什么的段落边界上。
    me->scenes.clear();
    if (!draft.scenes.empty()) {
        const std::vector<int> breaks = paragraph_breaks(text::strip_ws(me->text));
        int para = 0;
        int from = 0;
        for (std::size_t i = 0; i < draft.scenes.size(); ++i) {
            const DraftScene& sc = draft.scenes[i];
            para += static_cast<int>(sc.paragraphs.size());
            // 最后一场一律收在章尾：中间那些段落数对得上，末尾多一段少一段
            // （空段被丢掉、正文被截断）都不该让最后一场停在正文中间。
            int to = len;
            if (i + 1 < draft.scenes.size() && para >= 1 &&
                para - 1 < static_cast<int>(breaks.size())) {
                to = breaks[static_cast<std::size_t>(para - 1)];
            }
            to = std::clamp(to, from, len);
            if (to <= from) continue;  // 截断之后落在同一个点上的场不留
            Scene s;
            s.from_char = from;
            s.to_char = to;
            s.where = sc.where;
            s.pov = sc.pov;
            s.goal = sc.goal;
            s.obstacle = sc.obstacle;
            s.turn = sc.turn;
            me->scenes.push_back(std::move(s));
            from = to;
        }
    }

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

    // **每一场的末尾就是一个有说法的切点**，说法是那一场的 turn。
    // 这是现在钩子的主要来源：位置程序数得出来，说法模型本来就要填。
    for (const auto& s : me->scenes) put(s.to_char, s.turn);

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
