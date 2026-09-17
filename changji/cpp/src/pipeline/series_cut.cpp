#include "pipeline/series_cut.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "models/story.hpp"
#include "util/paths.hpp"

namespace changji::pipeline {

namespace fs = std::filesystem;
using json = nlohmann::json;
using models::Episode;
using models::Project;
using models::ProjectStore;

std::vector<CutPart> plan_series_cut(
    const std::vector<std::pair<std::string, media::Timeline>>& chapters,
    double per_episode_s) {
    // 接成一条：start_s 顺着往后排；记住每一镜出自哪一章
    media::Timeline all;
    std::map<std::string, std::string> chapter_of;
    double cursor = 0.0;
    for (const auto& [chapter_id, tl] : chapters) {
        for (media::TimelineEntry e : tl.entries) {
            e.start_s = cursor;
            cursor += e.duration_s;
            chapter_of[e.shot_id] = chapter_id;
            all.entries.push_back(std::move(e));
        }
    }
    std::vector<CutPart> out;
    double at = 0.0;
    for (const media::Timeline& part : media::split_into_episodes(all, per_episode_s)) {
        CutPart p;
        p.start_s = at;
        for (const auto& e : part.entries) p.shot_ids.push_back(e.shot_id);
        at += part.total_duration_s();
        p.end_s = at;
        if (!part.entries.empty()) p.first_chapter = chapter_of[part.entries.front().shot_id];
        out.push_back(std::move(p));
    }
    return out;
}

fs::path final_dir(const models::ProjectPaths& paths) { return paths.output() / "final"; }

namespace {

/// 这一章的成片文件，按名字排（切过的章是 ep01_01、ep01_02）。
std::vector<fs::path> films_of(const fs::path& dir, const std::string& episode_id) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        if (!f.is_regular_file(ec)) continue;
        if (paths::to_utf8(f.path().extension()) != ".mp4") continue;
        const auto owner = media::episode_of_output(paths::to_utf8(f.path().stem()));
        if (owner && *owner == episode_id) out.push_back(f.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// 挂着章的集，按故事里章的先后排。
std::vector<const Episode*> chapter_episodes(const Project& project,
                                             const models::Story& story) {
    std::map<std::string, int> order;
    for (std::size_t i = 0; i < story.chapters.size(); ++i) {
        order[story.chapters[i].chapter_id] = static_cast<int>(i);
    }
    std::vector<const Episode*> eps;
    for (const auto& e : project.episodes) {
        if (!e.chapter_refs.empty()) eps.push_back(&e);
    }
    const auto rank = [&](const Episode* e) {
        const auto it = order.find(e->chapter_refs.front());
        return it == order.end() ? (1 << 20) : it->second;
    };
    std::stable_sort(eps.begin(), eps.end(),
                     [&](const Episode* a, const Episode* b) { return rank(a) < rank(b); });
    return eps;
}

models::Story story_or_empty(const ProjectStore& store) {
    try {
        return store.load_story();
    } catch (const std::exception&) {
        return models::Story{};
    }
}

std::string secs(double s) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", s);
    return b;
}

}  // namespace

std::string series_cut_blocker(const ProjectStore& store) {
    const Project project = store.load_project();
    const models::Story story = story_or_empty(store);
    const auto eps = chapter_episodes(project, story);
    if (eps.empty()) return "一章都没有，没什么可切";
    for (const Episode* e : eps) {
        if (films_of(store.paths().output(), e->episode_id).empty()) {
            const models::Chapter* c = story.chapter_by_id(e->chapter_refs.front());
            const std::string label = c != nullptr && !c->title.empty()
                                          ? c->title
                                          : e->chapter_refs.front();
            return "「" + label + "」还没出片。每一章都出了片才能切集";
        }
    }
    return "";
}

SeriesCutReport cut_series(const ProjectStore& store, const config::Settings& settings,
                           const media::FFmpeg& ff, double per_episode_s,
                           JobProgress& progress) {
    const std::string blocker = series_cut_blocker(store);
    if (!blocker.empty()) throw std::runtime_error(blocker);

    const Project project = store.load_project();
    const models::Story story = story_or_empty(store);
    const fs::path out_dir = store.paths().output();

    // 每一章的成片和时间线。时间线只用来定切点（在镜头边界上）。
    std::vector<fs::path> films;
    std::vector<std::pair<std::string, media::Timeline>> chapters;
    for (const Episode* e : chapter_episodes(project, story)) {
        for (auto& f : films_of(out_dir, e->episode_id)) films.push_back(f);
        chapters.emplace_back(
            e->chapter_refs.front(),
            media::build_timeline(e->shots, store.paths(), settings.assembly,
                                  [&ff](const fs::path& v) {
                                      return ff.probe(v).duration_s;
                                  }));
    }
    const std::vector<CutPart> parts = plan_series_cut(chapters, per_episode_s);
    if (parts.empty()) throw std::runtime_error("时间线是空的，没什么可切");
    progress.set_total(static_cast<int>(parts.size()) + 1);
    progress.set_message("把 " + std::to_string(films.size()) + " 段片子接成一条");

    const fs::path dir = final_dir(store.paths());
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path work = dir / ".work";
    fs::create_directories(work, ec);
    struct Cleanup {
        const fs::path& d;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(d, e);
        }
    } cleanup{work};

    // 各章的成片是同一个装配器出的，参数一样，拼接直接 copy。
    const fs::path listing = work / "concat.txt";
    {
        std::ofstream f(listing, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("写不了拼接清单：" + paths::to_utf8(listing));
        f << media::concat_listing(films);
    }
    const fs::path joined = work / "joined.mp4";
    ff.run(media::concat_args(listing, joined));
    progress.set_done(1);

    // 一集一集切出来。**重编码**，不 -c copy：copy 只能落在关键帧上，
    // 切点会往前漂到上一个关键帧，也就是切进上一镜里。
    SeriesCutReport report;
    report.parts = parts;
    const auto& a = settings.assembly;
    json manifest{{"per_episode_s", per_episode_s}, {"parts", json::array()}};
    for (std::size_t k = 0; k < parts.size(); ++k) {
        if (progress.cancelled()) throw std::runtime_error("已手动停止。已经切出来的留着");
        char name[32];
        std::snprintf(name, sizeof name, "第%02d集.mp4", static_cast<int>(k + 1));
        const fs::path out = dir / paths::from_utf8(name);
        progress.set_message("切第 " + std::to_string(k + 1) + " 集 · " +
                             std::to_string(static_cast<int>(parts[k].end_s - parts[k].start_s)) +
                             " 秒");
        ff.run({"-y", "-ss", secs(parts[k].start_s), "-i", paths::to_utf8(joined),
                "-t", secs(parts[k].end_s - parts[k].start_s),
                "-c:v", a.video_codec, "-crf", std::to_string(a.crf),
                "-pix_fmt", a.pix_fmt, "-c:a", a.audio_codec, "-b:a", a.audio_bitrate,
                "-movflags", "+faststart", paths::to_utf8(out)});
        report.outputs.push_back(out);
        progress.add_output(paths::to_utf8(out));
        manifest["parts"].push_back({{"name", name},
                                     {"start_s", parts[k].start_s},
                                     {"end_s", parts[k].end_s},
                                     {"duration_s", parts[k].end_s - parts[k].start_s},
                                     {"from_chapter", parts[k].first_chapter},
                                     {"shots", parts[k].shot_ids.size()}});
        progress.set_done(static_cast<int>(k) + 2);
    }
    report.total_s = parts.back().end_s;
    manifest["total_s"] = report.total_s;
    {
        std::ofstream f(dir / "cut.json", std::ios::binary | std::ios::trunc);
        f << manifest.dump(2);
    }
    progress.set_message("切好了 " + std::to_string(parts.size()) + " 集");
    return report;
}

}  // namespace changji::pipeline
