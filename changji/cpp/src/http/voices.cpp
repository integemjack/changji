#include "http/voices.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/project.hpp"
#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::http {

using json = nlohmann::ordered_json;

namespace {

/// 项目 voices/ 里已经有的那几段参考音色。
///
/// **这就是进程内配音的"音色清单"。** 它没有服务端的那种列表——音色来自
/// 参考音频，所以"有哪些音色"等于"这个项目里存了哪几段人声"。以前这个
/// 接口只回一句说明，界面上那一栏是个空文本框，用户得手打路径，而路径
/// 打错的表现只是配音失败。
std::vector<std::string> clips_in(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const std::string ext = paths::to_utf8(e.path().extension());
        if (ext != ".wav" && ext != ".mp3" && ext != ".m4a" && ext != ".flac") {
            continue;
        }
        out.push_back("voices/" + paths::to_utf8(e.path().filename()));
    }
    // 目录遍历的顺序是文件系统给的，排一下——不排的话同一个项目在不同
    // 机器上这一栏的顺序不一样，看着像数据变了。
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

ApiResult get_voices(const std::string& path, const std::string& backend) {
    // 项目路径照旧要：前端一直是带着它来的，突然不校验的话
    // 拼错路径的请求会静默成功，而错误在别的接口上才暴露出来。
    if (path.empty()) throw ApiError(400, "没有指定项目目录");

    // **两条后端都没有服务端清单，但原因不一样，说法也不该一样。**
    if (backend == "http") {
        return {200,
                {{"voices", json::array()},
                 {"kind", "name"},
                 {"error", "外部配音服务（[tts].backend = http）的音色由那个"
                           "服务自己管，这里问不到。把角色的 voice_id 填成"
                           "那个服务认的音色名就行。"}}};
    }

    const models::ProjectPaths paths(paths::from_utf8(path));
    const auto clips = clips_in(paths.voices());
    json arr = json::array();
    for (const auto& c : clips) arr.push_back(c);

    // `kind` 告诉界面这一栏该画成什么：clip 是"从这几段里挑，也可以传新的"，
    // name 是"手填一个名字"。前端不该靠 backend 猜。
    json out = {{"voices", arr}, {"kind", "clip"}};
    if (clips.empty()) {
        out["error"] =
            "这个项目还没有参考音色。进程内配音（[tts].backend = local）"
            "的音色来自参考音频——在角色那一栏传一段几秒的干净人声，"
            "模型照着它的音色念。";
    }
    return {200, std::move(out)};
}

}  // namespace changji::http
