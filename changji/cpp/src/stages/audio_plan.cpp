#include "stages/audio_plan.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

#include "stages/render.hpp"
#include "util/text.hpp"

namespace changji::stages {

namespace {

/// 标点不发音，但产生停顿。逗号短一点，句号问号感叹号长一点。
///
/// 把标点当成字算的话，一句"什么？！"会被估成四个字的长度，
/// 而它实际上要停顿快一秒。
const std::map<std::string, double>& pauses() {
    static const std::map<std::string, double> m = {
        {"，", 0.18}, {"、", 0.12}, {"；", 0.22}, {"：", 0.18},
        {"。", 0.32}, {"？", 0.35}, {"！", 0.35}, {"…", 0.40}, {"—", 0.25},
    };
    return m;
}

const std::vector<std::string>& sentence_ends() {
    static const std::vector<std::string> v = {"。", "！", "？", "…"};
    return v;
}

const std::vector<std::string>& clause_ends() {
    static const std::vector<std::string> v = {"；", "，", "、"};
    return v;
}

bool in(const std::vector<std::string>& set, const std::string& ch) {
    return std::find(set.begin(), set.end(), ch) != set.end();
}

/// 去掉全部空白（不只是首尾）。对齐 Python 的 re.sub(r"[\s]", "", text)。
std::string strip_all_space(const std::string& s) {
    std::string out;
    for (const auto& ch : text::utf8_chars(s)) {
        if (ch.size() == 1) {
            const unsigned char c = static_cast<unsigned char>(ch[0]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
                c == '\f') {
                continue;
            }
        }
        out += ch;
    }
    return out;
}

std::string join_chars(const std::vector<std::string>& v, std::size_t from,
                       std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to && i < v.size(); ++i) out += v[i];
    return out;
}

std::string fmt(const char* spec, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

}  // namespace

double estimate_speech_duration(const std::string& text_in) {
    const std::string stripped = strip_all_space(text_in);
    if (stripped.empty()) return 0.0;

    double pause_total = 0.0;
    int spoken = 0;
    for (const auto& ch : text::utf8_chars(stripped)) {
        const auto it = pauses().find(ch);
        if (it != pauses().end()) {
            pause_total += it->second;
        } else {
            ++spoken;
        }
    }
    return spoken / kCharsPerSecond + pause_total + kLeadInS + kTailS;
}

double max_line_seconds(int fps) { return max_shot_duration_s(fps) - kTailS; }

std::vector<std::string> split_long_text(const std::string& text_in,
                                         double max_seconds) {
    if (estimate_speech_duration(text_in) <= max_seconds) return {text_in};

    // 在给定的一组标点处切。**只有攒够 60% 的预算才切**——
    // 见一个标点就切的话，"你好，我来了。"会被切成三段，
    // 每段一秒不到，配出来是三段各自带头尾留白的碎音频。
    const auto chunks_by = [max_seconds](const std::vector<std::string>& marks,
                                         const std::vector<std::string>& src) {
        std::vector<std::string> out;
        for (const auto& part : src) {
            if (estimate_speech_duration(part) <= max_seconds) {
                out.push_back(part);
                continue;
            }
            std::string buf;
            for (const auto& ch : text::utf8_chars(part)) {
                buf += ch;
                if (in(marks, ch) &&
                    estimate_speech_duration(buf) >= max_seconds * 0.6) {
                    out.push_back(buf);
                    buf.clear();
                }
            }
            if (!buf.empty()) out.push_back(buf);
        }
        return out;
    };

    std::vector<std::string> pieces = chunks_by(sentence_ends(), {text_in});
    pieces = chunks_by(clause_ends(), pieces);

    // 还有过长的就只能按字数硬切。**每一段都要自带头尾的呼吸留白**，
    // 算字数时先把这部分扣掉，否则切出来的每一段都刚好超一点。
    const double speakable = std::max(0.5, max_seconds - kLeadInS - kTailS);
    const std::size_t take =
        std::max<std::size_t>(1, static_cast<std::size_t>(speakable * kCharsPerSecond));

    std::vector<std::string> final_pieces;
    for (const auto& piece : pieces) {
        std::vector<std::string> chars = text::utf8_chars(piece);
        while (estimate_speech_duration(join_chars(chars, 0, chars.size())) >
               max_seconds) {
            final_pieces.push_back(join_chars(chars, 0, take));
            if (chars.size() <= take) {
                chars.clear();
                break;
            }
            chars.erase(chars.begin(),
                        chars.begin() + static_cast<std::ptrdiff_t>(take));
        }
        if (!chars.empty()) {
            final_pieces.push_back(join_chars(chars, 0, chars.size()));
        }
    }

    std::vector<std::string> out;
    for (const auto& p : final_pieces) {
        const std::string t = text::strip_ws(p);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

std::string free_shot_id(const std::string& base,
                         const std::set<std::string>& used) {
    for (const char suffix : std::string("bcdefghijklmnopqrstuvwxyz")) {
        const std::string candidate = base + "_" + suffix;
        if (used.count(candidate) == 0) return candidate;
    }
    // 二十五个后缀都用光了（一个镜头拆出二十六段），退回数字。
    int n = 2;
    while (used.count(base + "_" + std::to_string(n)) != 0) ++n;
    return base + "_" + std::to_string(n);
}

std::vector<std::vector<models::DialogueLine>> group_lines(
    const models::Shot& shot, double max_seconds) {
    if (shot.dialogue.size() <= 1) return {shot.dialogue};

    const auto dur = [](const models::DialogueLine& line) {
        // 已经配过音的用真实时长，没配过的用估算。混着用是对的：
        // 拆分发生在配音之后，但重跑时可能有几句还没配。
        if (line.actual_duration_s.has_value()) return *line.actual_duration_s;
        return estimate_speech_duration(line.text);
    };

    const double budget = std::max(0.5, max_seconds - kTailS);
    std::vector<std::vector<models::DialogueLine>> groups;
    std::vector<models::DialogueLine> current;
    double total = 0.0;

    for (const auto& line : shot.dialogue) {
        const double d = dur(line);
        // **一句话就超预算时也要单独成组**，不能因为装不下就丢掉。
        // 所以先看 current 非空——空的时候无条件收下。
        if (!current.empty() && total + d > budget) {
            groups.push_back(current);
            current.clear();
            total = 0.0;
        }
        current.push_back(line);
        total += d;
    }
    if (!current.empty()) groups.push_back(current);
    return groups;
}

std::vector<models::Shot> split_overlong_shots(std::vector<models::Shot> shots,
                                               double max_seconds) {
    // 编号要跟**全集**比对着发。同一集重跑一次配音会再拆一次，
    // 只按本次的序号取名的话第二次又会取出一个 sh001_b，
    // 于是一集里出现两个同名镜头：按 id 找镜头只能找到头一个，
    // 音频和首帧的文件名也会互相覆盖。
    std::set<std::string> used;
    for (const auto& s : shots) used.insert(s.shot_id);

    std::stable_sort(shots.begin(), shots.end(),
                     [](const models::Shot& a, const models::Shot& b) {
                         return a.order < b.order;
                     });

    std::vector<models::Shot> out;
    for (auto& shot : shots) {
        auto groups = group_lines(shot, max_seconds);
        if (groups.size() <= 1) {
            out.push_back(shot);
            continue;
        }
        shot.dialogue = groups[0];
        out.push_back(shot);

        for (std::size_t g = 1; g < groups.size(); ++g) {
            models::Shot extra = shot;   // 深拷贝
            extra.shot_id = free_shot_id(shot.shot_id, used);
            used.insert(extra.shot_id);
            extra.dialogue = groups[g];
            // **新镜是全新的画面，之前那一镜的产物一概不能继承。**
            // 继承的话，新镜会带着原镜的 frame_path 和 video_path，
            // 流水线看到"已经有产物"就跳过它，成片里那一段是重复的画面。
            extra.frame_path.reset();
            extra.video_path.reset();
            extra.attempts = 0;
            extra.gate_notes.clear();
            extra.status = models::ShotStatus::PLANNED;
            extra.duration_locked = false;
            out.push_back(extra);
        }
    }

    // order 是整数，拆完统一重排一遍。不重排的话新镜和原镜同号，
    // 排序不稳定时成片里两镜的先后是随机的。
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i].order = static_cast<int>(i);
    }
    return out;
}

std::string summarize(const std::vector<ShotAudioPlan>& plans) {
    if (plans.empty()) return "没有需要配音的镜头";

    double total_speech = 0.0;
    double total_locked = 0.0;
    int total_lines = 0;
    std::vector<std::string> tight;
    for (const auto& p : plans) {
        total_speech += p.speech_duration_s;
        total_locked += p.locked_duration_s;
        total_lines += p.lines;
        // 没有台词的镜头留白当然充足，不该出现在"留白不足"的名单里。
        if (p.is_tight() && p.lines) tight.push_back(p.shot_id);
    }

    std::string out =
        "配音完成 " + std::to_string(total_lines) + " 句，覆盖 " +
        std::to_string(plans.size()) + " 个镜头\n语音总长 " +
        fmt("%.1f", total_speech) + " 秒，锁定后镜头总长 " +
        fmt("%.1f", total_locked) + " 秒";

    if (!tight.empty()) {
        out += "\n其中 " + std::to_string(tight.size()) +
               " 个镜头留白不足半秒，装配时不能再压缩：";
        // 只列前五个。四十镜里有二十个紧的时候，列全了没人会读。
        for (std::size_t i = 0; i < tight.size() && i < 5; ++i) {
            if (i) out += "、";
            out += tight[i];
        }
    }
    return out;
}

}  // namespace changji::stages
