#include "http/film.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "config/settings.hpp"
#include "http/job_stream.hpp"
#include "media/ffmpeg.hpp"
#include "pipeline/jobs.hpp"
#include "pipeline/series_cut.hpp"
#include "util/fs_time.hpp"
#include "util/paths.hpp"

namespace changji::http {

namespace fs = std::filesystem;
using json = nlohmann::json;
using models::ProjectStore;

namespace {

// 这几个小工具每个接口文件各抄一份，是这一层的老规矩（planning.cpp、
// story_api.cpp 里都有一份）。

void forbid_extra(const json& body, const std::set<std::string>& allowed) {
    if (!body.is_object()) throw ApiError(422, "请求体要是个对象");
    for (const auto& kv : body.items()) {
        if (!allowed.count(kv.key())) throw ApiError(422, "多了一个字段：" + kv.key());
    }
}

ProjectStore open_project(const std::string& path) {
    const fs::path root = paths::from_utf8(path);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) throw ApiError(404, "项目目录不存在：" + path);
    return ProjectStore(root);
}

ProjectStore open_project(const json& body) {
    const auto it = body.is_object() ? body.find("project") : body.end();
    if (it == body.end() || !it->is_string()) throw ApiError(422, "缺 project");
    return open_project(it->get<std::string>());
}

void load_or_400(const ProjectStore& store) {
    try {
        (void)store.load_project();
    } catch (const std::exception& e) {
        throw ApiError(400, std::string("项目读不了：") + e.what());
    }
}

double round1(double v) { return std::round(v * 10.0) / 10.0; }

json read_manifest(const fs::path& dir) {
    std::ifstream in(dir / "cut.json", std::ios::binary);
    if (!in.good()) return json::object();
    try {
        json m;
        in >> m;
        return m.is_object() ? m : json::object();
    } catch (const std::exception&) {
        return json::object();
    }
}

}  // namespace

ApiResult get_film(const std::string& path) {
    const ProjectStore store = open_project(path);
    const fs::path dir = pipeline::final_dir(store.paths());
    json out{{"per_episode_s", 0.0}, {"total_s", 0.0}, {"files", json::array()}};

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {200, out};
    const json manifest = read_manifest(dir);
    if (manifest.contains("per_episode_s")) out["per_episode_s"] = manifest["per_episode_s"];
    if (manifest.contains("total_s")) out["total_s"] = manifest["total_s"];
    std::map<std::string, json> parts;
    for (const auto& p : manifest.value("parts", json::array())) {
        if (p.is_object()) parts[p.value("name", std::string())] = p;
    }

    std::vector<json> files;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        if (!f.is_regular_file(ec)) continue;
        if (paths::to_utf8(f.path().extension()) != ".mp4") continue;
        const std::string name = paths::to_utf8(f.path().filename());
        json item{{"name", name},
                  {"rel", store.paths().rel(f.path())},
                  {"size_mb", round1(static_cast<double>(fs::file_size(f.path(), ec)) /
                                     (1024.0 * 1024.0))},
                  {"mtime", static_cast<long long>(util::file_mtime_unix(f.path()))}};
        if (const auto it = parts.find(name); it != parts.end()) {
            for (const char* k : {"start_s", "end_s", "duration_s", "from_chapter", "shots"}) {
                if (it->second.contains(k)) item[k] = it->second[k];
            }
        }
        files.push_back(std::move(item));
    }
    // 按名字排：第01集、第02集……成片的顺序就是剧的顺序，不按改动时间。
    std::sort(files.begin(), files.end(), [](const json& a, const json& b) {
        return a.at("name").get<std::string>() < b.at("name").get<std::string>();
    });
    for (auto& f : files) out["files"].push_back(std::move(f));
    return {200, out};
}

ApiResult post_film_cut(const json& body) {
    forbid_extra(body, {"project", "per_episode_s"});
    double per = 0.0;
    if (const auto it = body.find("per_episode_s"); it != body.end()) {
        if (!it->is_number()) throw ApiError(422, "per_episode_s 要是个数（秒）");
        per = it->get<double>();
        if (per < 0.0 || per > 86400.0) throw ApiError(422, "per_episode_s 要在 0 到 86400 之间");
    }
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, "出片那边还在忙，等它完了再切");
    }
    ProjectStore store = open_project(body);
    load_or_400(store);
    const std::string blocker = pipeline::series_cut_blocker(store);
    if (!blocker.empty()) throw ApiError(400, blocker);

    const std::string root = paths::to_utf8(store.root());
    const std::string title =
        per > 0.0 ? "成片 · 每集 " + std::to_string(static_cast<int>(std::round(per / 60.0))) + " 分钟"
                  : "成片 · 整部一集";
    const bool started = pipeline::jobs().start(
        pipeline::JobKind::Run, "",
        [root, per](pipeline::JobProgress& p) {
            const JobScope scope{pipeline::jobs().job_id(pipeline::JobKind::Run), p.token()};
            const ProjectStore st(paths::from_utf8(root));
            const config::Settings s = config::load_settings(st.root());
            const media::FFmpeg ff(s.assembly.ffmpeg_path, s.assembly.ffprobe_path,
                                   media::default_runner());
            pipeline::cut_series(st, s, ff, per, p);
        },
        "已手动停止。已经切出来的留着。", root, title);
    if (!started) throw ApiError(409, "出片那边还在忙，等它完了再切");
    return {202, {{"started", true}}};
}

}  // namespace changji::http
