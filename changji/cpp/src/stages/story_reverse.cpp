#include "stages/story_reverse.hpp"

#include <cstdio>

#include "stages/story_import.hpp"
#include "util/text.hpp"

namespace changji::stages {

using namespace changji::models;

namespace {

std::string chapter_id(std::size_t i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "ch%02zu", i + 1);
    return buf;
}

/// 几章算什么体量。
///
/// 只影响以后让模型续写时提示词里那个"大概写几章"的量级，所以按
/// suggested_chapters 那三个数（4 / 8 / 16）的中点分。反推出来的故事本身
/// 已经有多少章是多少章，这个字段不会去动它。
StoryScale scale_for(std::size_t chapters) {
    if (chapters <= 6) return StoryScale::SHORT;
    if (chapters <= 12) return StoryScale::MEDIUM;
    return StoryScale::LONG;
}

}  // namespace

Story story_from_episodes(const Project& project) {
    Story story;
    story.premise = project.premise;
    // **不是 AI 写的，所以不能标 AI。** 这个字段决定界面上那句"这个故事
    // 哪来的"，标错了用户会以为大纲是模型编的，而它其实是他自己那几集。
    story.source = StorySource::PASTED;

    double duration_sum = 0.0;
    int duration_count = 0;

    for (const Episode& ep : project.episodes) {
        const std::string script = text::strip_ws(ep.script);
        // 没剧本的集跳过：反推出来是一章空正文，既提不出结构也切不出分集，
        // 只会在章节列表里占一行说不清的东西。
        if (script.empty()) continue;

        Chapter ch;
        ch.chapter_id = chapter_id(story.chapters.size());
        ch.title = ep.title.empty() ? ep.episode_id : ep.title;
        ch.summary = ep.synopsis;
        ch.text = script;
        // 段落边界登记成候选切点。不登记的话这一章里只有章界一个候选，
        // 以后改每集时长重切时整章只能变成一集。
        ch.hooks = paragraph_hooks(ch.text);

        EpisodePlan p;
        // **指回真正的 episode_id**，不是章 id。老项目里集号可能不连续
        // （删过几集），而章 id 是重排过的；两边就靠这一个字段对上。
        p.episode_id = ep.episode_id;
        p.title = ch.title;
        // 钩子留空：切点是章界，而章界是这一集本来就有的边界，不是读懂
        // 剧情找出来的悬念。**宁可空着**——填一句假的，界面上会显示成
        // "这一集停在这儿"，而那句话没有任何依据。
        p.target_duration_s = ep.target_duration_s;
        p.from_chapter = ch.chapter_id;
        p.from_char = 0;
        p.to_chapter = ch.chapter_id;
        p.to_char = ch.text_len();

        if (ep.target_duration_s > 0.0) {
            duration_sum += ep.target_duration_s;
            ++duration_count;
        }

        story.chapters.push_back(std::move(ch));
        story.plan.push_back(std::move(p));
    }

    story.scale = scale_for(story.chapters.size());
    // 每集时长取现有各集的平均。**不用默认的 60 秒**：老项目那几集是按
    // 自己那个时长排的分镜，拿 60 秒去写进故事里，用户下次点"重算分集"
    // 会得到一份和现有剧集对不上的表，而他并没有改过时长。
    if (duration_count > 0) {
        story.episode_duration_s = duration_sum / duration_count;
    }
    return story;
}

}  // namespace changji::stages
