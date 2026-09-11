#include "models/story.hpp"

#include <algorithm>
#include <set>

#include "util/text.hpp"

namespace changji::models {

namespace {

/// 章节 id 的形状：ch 开头，后面全是数字。
///
/// 比剧集 id 严一档（那边允许任意小写下划线串）。章节 id 是程序生成的，
/// 不经用户手，所以可以把形状钉死；钉死之后按 id 排序就等于按顺序排序。
bool is_chapter_id(const std::string& s) {
    if (s.size() < 3) return false;
    if (s.compare(0, 2, "ch") != 0) return false;
    return std::all_of(s.begin() + 2, s.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

const char* to_string(StoryScale v) {
    switch (v) {
        case StoryScale::SHORT: return "short";
        case StoryScale::MEDIUM: return "medium";
        case StoryScale::LONG: return "long";
    }
    return "medium";
}

int suggested_chapters(StoryScale scale) {
    // 只是给大纲提示词的一个量级，不是硬约束——模型按故事本身的需要多写少写
    // 一两章是对的。写死成「必须 N 章」会让它为了凑数注水或者硬收。
    switch (scale) {
        case StoryScale::SHORT: return 4;
        case StoryScale::MEDIUM: return 8;
        case StoryScale::LONG: return 16;
    }
    return 8;
}

const char* to_string(StorySource v) {
    switch (v) {
        case StorySource::AI: return "ai";
        case StorySource::PASTED: return "pasted";
        case StorySource::KEYWORDS: return "keywords";
    }
    return "ai";
}

int Chapter::text_len() const {
    return static_cast<int>(text::utf8_len(text));
}

bool Story::empty() const {
    return text::strip_ws(premise).empty() && chapters.empty();
}

const Chapter* Story::chapter_by_id(const std::string& chapter_id) const {
    for (const auto& c : chapters) {
        if (c.chapter_id == chapter_id) return &c;
    }
    return nullptr;
}

Chapter* Story::chapter_by_id(const std::string& chapter_id) {
    for (auto& c : chapters) {
        if (c.chapter_id == chapter_id) return &c;
    }
    return nullptr;
}

int Story::written_chapters() const {
    int n = 0;
    for (const auto& c : chapters) {
        if (!text::strip_ws(c.text).empty()) ++n;
    }
    return n;
}

std::vector<std::string> Story::validate() const {
    std::vector<std::string> errs;

    if (text::utf8_len(premise) > 2000) {
        errs.push_back("premise 最多 2000 字");
    }
    if (episode_duration_s <= 0.0) {
        errs.push_back("episode_duration_s 必须大于 0");
    }

    std::set<std::string> chapter_ids;
    for (const auto& c : chapters) {
        if (!is_chapter_id(c.chapter_id)) {
            errs.push_back("章节 id 必须是 ch 加数字，当前是 " + c.chapter_id);
            continue;
        }
        if (!chapter_ids.insert(c.chapter_id).second) {
            errs.push_back("章节 id 重复：" + c.chapter_id);
        }
        // 钩子越界是会真出事的一类错：切分算法拿它当切点，越界就会截出
        // 空的一集，或者把切线落到下一章去。
        const int len = c.text_len();
        for (const auto& h : c.hooks) {
            if (h.at_char < 0 || h.at_char > len) {
                errs.push_back(c.chapter_id + "：钩子位置 " +
                               std::to_string(h.at_char) + " 超出正文范围 0.." +
                               std::to_string(len));
            }
        }
    }

    std::set<std::string> names;
    for (const auto& ch : characters) {
        if (text::strip_ws(ch.name).empty()) {
            errs.push_back("人物没有名字");
        } else if (!names.insert(ch.name).second) {
            errs.push_back("人物重名：" + ch.name);
        }
    }
    // 关系的两端必须是登记过的人物。指向不存在的人时，界面上那条关系画不出来，
    // 而提示词里会凭空多出一个角色——这正是要防的漂移。
    for (const auto& r : relations) {
        if (!r.a.empty() && names.count(r.a) == 0) {
            errs.push_back("关系指向了不存在的人物：" + r.a);
        }
        if (!r.b.empty() && names.count(r.b) == 0) {
            errs.push_back("关系指向了不存在的人物：" + r.b);
        }
    }

    std::set<std::string> ep_ids;
    for (const auto& p : plan) {
        if (p.episode_id.empty()) {
            errs.push_back("分集表里有一条没有剧集 id");
        } else if (!ep_ids.insert(p.episode_id).second) {
            errs.push_back("分集表里剧集 id 重复：" + p.episode_id);
        }
        if (p.target_duration_s <= 0.0) {
            errs.push_back(p.episode_id + "：target_duration_s 必须大于 0");
        }
        if (!p.from_chapter.empty() && chapter_ids.count(p.from_chapter) == 0) {
            errs.push_back(p.episode_id + "：起始章 " + p.from_chapter + " 不存在");
        }
        if (!p.to_chapter.empty() && chapter_ids.count(p.to_chapter) == 0) {
            errs.push_back(p.episode_id + "：末章 " + p.to_chapter + " 不存在");
        }
    }

    return errs;
}

}  // namespace changji::models
