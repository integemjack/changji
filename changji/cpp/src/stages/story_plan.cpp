#include "stages/story_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "util/text.hpp"

namespace changji::stages {

using models::Chapter;
using models::EpisodePlan;
using models::Story;

namespace {

/// 一个 run 里的一章。run 是「连续的、都有正文的」那几章。
struct Piece {
    const Chapter* ch = nullptr;
    int start = 0; ///< 在这个 run 里的全局起始字符
    int len = 0;
};

/// 剧集 id。和 http/episodes.cpp 的 ep_fmt 同形，两边生成的 id 要能对上。
std::string ep_id(int n) {
    if (n < 100) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "ep%02d", n);
        return buf;
    }
    return "ep" + std::to_string(n);
}

/// 一章拆成好几集时，标题后面挂的那个后缀。
///
/// 两集用上/下、三集用上/中/下，是中文里的习惯写法；再多就只能编号了。
/// 不做这一层的话，同一章切出来的几集标题一模一样，集号条上根本分不清。
std::string part_suffix(int i, int k) {
    if (k <= 1) return "";
    if (k == 2) return i == 0 ? "（上）" : "（下）";
    if (k == 3) {
        static const char* kThree[] = {"（上）", "（中）", "（下）"};
        return kThree[i];
    }
    static const char* kCn[] = {"一", "二", "三", "四", "五",
                                "六", "七", "八", "九", "十"};
    const int n = i + 1;
    return n <= 10 ? std::string("（") + kCn[n - 1] + "）"
                   : "（" + std::to_string(n) + "）";
}

}  // namespace

int prose_budget_chars(double duration_s) {
    if (duration_s <= 0.0) return 0;
    const long v = std::lround(duration_s * kProseCharsPerSecond);
    return v < 1 ? 1 : static_cast<int>(v);
}

std::vector<EpisodePlan> plan_episodes(const Story& story, double per_episode_s) {
    double per = per_episode_s;
    if (per <= 0.0) per = story.episode_duration_s;
    if (per <= 0.0) per = 60.0;

    const int cap = prose_budget_chars(per);

    std::vector<EpisodePlan> out;
    std::vector<Piece> run;
    int n = 1;

    // 把攒着的这一段连续章节切成集。
    auto flush = [&]() {
        if (run.empty()) return;

        const int total = run.back().start + run.back().len;

        // 候选切点，以及落在那个位置的钩子说明。
        // 章界永远是候选：它天然是一个情节单元的结束。
        std::set<int> cuts;
        // 有说法的那些单独再记一份。它们是读懂剧情之后标出来的真钩子，
        // 挑切点时优先用——而 cuts 里大多数是机械登记的段落边界。
        std::set<int> named;
        std::map<int, std::string> hook_at;
        for (const auto& p : run) {
            for (const auto& h : p.ch->hooks) {
                const int at = std::clamp(h.at_char, 0, p.len);
                const int g = p.start + at;
                cuts.insert(g);
                if (!h.text.empty()) named.insert(g);
                // 同一个位置有多个钩子时留第一个，不覆盖——覆盖的话结果
                // 取决于 hooks 数组的顺序，而那是模型给的，不稳定。
                hook_at.emplace(g, h.text);
            }
            cuts.insert(p.start + p.len);
        }

        auto chapter_at = [&](int g) -> const Piece& {
            for (const auto& p : run) {
                if (g >= p.start && g < p.start + p.len) return p;
            }
            return run.back();
        };

        int pos = 0;
        while (pos < total) {
            int next = total;
            if (total - pos > static_cast<double>(cap) * kTailMergeRatio) {
                const int ideal = pos + cap;
                // 有序集合里找离 ideal 最近的那个。距离先减后增，
                // 开始变大就不会再变小了，所以看到回升就停。
                const auto nearest = [&](const std::set<int>& from) {
                    int best = -1;
                    long best_d = 0;
                    for (auto it = from.upper_bound(pos); it != from.end(); ++it) {
                        const long d = std::labs(static_cast<long>(*it) - ideal);
                        if (best < 0 || d < best_d) {
                            best = *it;
                            best_d = d;
                        } else {
                            break;
                        }
                    }
                    return std::pair<int, long>{best, best_d};
                };

                const auto any = nearest(cuts);
                const auto tagged = nearest(named);
                int best = any.first;
                // **有说法的钩子让一让也值得。** 每一集的结尾是完播率的
                // 命门，切在一个读懂剧情标出来的悬念上，比切在一个说不出
                // 为什么的段落边界上强——只要别偏得太离谱。
                if (tagged.first > pos &&
                    static_cast<double>(tagged.second) <=
                        static_cast<double>(cap) * kNamedHookSlack) {
                    best = tagged.first;
                }
                if (best > pos) next = best;
            }

            const Piece& from = chapter_at(pos);
            const Piece& to = chapter_at(next - 1);

            EpisodePlan p;
            p.episode_id = ep_id(n++);
            p.title = from.ch->title;
            p.target_duration_s = per;
            p.from_chapter = from.ch->chapter_id;
            p.from_char = pos - from.start;
            p.to_chapter = to.ch->chapter_id;
            p.to_char = next - to.start;
            const auto it = hook_at.find(next);
            if (it != hook_at.end()) p.hook = it->second;
            out.push_back(std::move(p));

            pos = next;
        }
        run.clear();
    };

    for (const auto& ch : story.chapters) {
        if (text::strip_ws(ch.text).empty()) {
            // 没展开正文的章节：一章一集。先把前面攒着的切掉，保证顺序。
            flush();
            EpisodePlan p;
            p.episode_id = ep_id(n++);
            p.title = ch.title;
            p.target_duration_s = per;
            p.from_chapter = ch.chapter_id;
            p.from_char = 0;
            p.to_chapter = ch.chapter_id;
            p.to_char = ch.text_len();
            if (!ch.hooks.empty()) p.hook = ch.hooks.back().text;
            out.push_back(std::move(p));
            continue;
        }
        Piece piece;
        piece.ch = &ch;
        piece.start = run.empty() ? 0 : run.back().start + run.back().len;
        piece.len = ch.text_len();
        run.push_back(piece);
    }
    flush();

    // 同一章切出好几集时给标题加上/下。要等全部切完才知道一章切了几集。
    std::map<std::string, int> parts;
    for (const auto& p : out) ++parts[p.from_chapter];
    std::map<std::string, int> seen;
    for (auto& p : out) {
        const int k = parts[p.from_chapter];
        if (k <= 1) continue;
        p.title += part_suffix(seen[p.from_chapter]++, k);
    }

    return out;
}

}  // namespace changji::stages
