#include "http/voices.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/runtime.hpp"
#include "media/ffmpeg.hpp"
#include "models/project.hpp"
#include "pipeline/activity.hpp"
#include "stages/audio.hpp"
#include "stages/tts_backends.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::http {

// **回包用 ordered_json（键序稳定，便于对拍），入参用 nlohmann::json
// （头文件里声明的就是它）。** 两个是不同类型：一个底下是 ordered_map，
// 一个是 std::map。混用的话声明和定义对不上，编译过、链接才炸——
// 2026-09-13 就是这么栽的一次（undefined reference to post_voice_take）。
using json = nlohmann::ordered_json;
using in_json = nlohmann::json;
using namespace changji::models;

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

namespace {

/// 名字变成文件名。
///
/// **保留中文。** 项目目录本来就可能叫「雨夜天台」，这套代码到处用
/// paths::from_utf8 处理，中文文件名不是问题。真正要挡的是路径分隔符和
/// 几个在 Windows 上非法的字符——留着它们的话，用户起个名叫「男/低沉」
/// 就写到别的目录去了。
std::string slugify(const std::string& name, unsigned int seed) {
    static const std::string kBad = "/\\:*?\"<>|";
    std::string out;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) continue;          // 控制字符
        if (kBad.find(c) != std::string::npos) continue;
        out.push_back(c);
    }
    // 首尾空白削掉：文件名带空格在命令行上很难用。
    out = text::strip_ws(out);
    // 太长的名字在有些文件系统上会被截断，截断之后可能撞名。
    out = text::truncate_utf8(out, 40);
    if (out.empty()) out = "voice_" + std::to_string(seed);
    return out;
}

/// 试听用的那句话。短，摇得快。
constexpr const char* kTakeText = "你好，这是我说话的样子。";

/// 存下来时念的那一段。**比试听长**，理由见 post_voice_save 的注释。
constexpr const char* kSaveText =
    "你好，这是我说话的样子。今天天气不错，风从窗口吹进来，"
    "把桌上的纸吹得哗哗响。我慢慢把它们压好，然后坐下来，等着你开口。";

unsigned int seed_of(const in_json& body) {
    if (body.is_object() && body.contains("seed") &&
        body.at("seed").is_number()) {
        const double v = body.at("seed").get<double>();
        if (v >= 0 && v <= 4294967295.0) return static_cast<unsigned int>(v);
    }
    // 没给就随机摇一个。**回包里要带上**，否则用户听到一个喜欢的却存不下来。
    std::random_device rd;
    return rd() % 1000000u;
}

ProjectStore open_or_400(const in_json& body) {
    if (!body.is_object() || !body.contains("project") ||
        !body.at("project").is_string()) {
        throw ApiError(400, "没有指定项目目录");
    }
    const std::string p = body.at("project").get<std::string>();
    if (p.empty()) throw ApiError(400, "没有指定项目目录");
    return ProjectStore(paths::from_utf8(p));
}

/// 出一段，顺带把基频量出来。
json render_into(const ProjectStore& store, const fs::path& dest,
                 const std::string& text, unsigned int seed) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);

    // **先把后端搭起来。** 注册调度器里那个配音槽是 local_tts_backend
    // 干的事，而下面 render_voice_take 是直接去借槽的——引擎刚重启、
    // 还没跑过任何一集时，借到的是「槽 配音 还没注册」。
    // 2026-09-13 实测撞到：八个预置种子全部摇不出来，就是这一条。
    const config::Settings cfg = config::runtime().snapshot();
    const auto ff = media::FFmpeg(cfg.assembly.ffmpeg_path,
                                  cfg.assembly.ffprobe_path,
                                  media::default_runner());
    const stages::TTSBackend backend = stages::pick_tts_backend(cfg, ff);
    // **摇音色只在进程内那条路上有意义。** estimate 出来的是等长静音，
    // 摇一百次都是同一段无声；外部服务的音色是它自己管的名字，不是我们
    // 生成的片段。这两种情况下明说，别让人对着静音以为音箱坏了。
    if (backend.name != "local") {
        throw ApiError(
            503,
            std::string("现在的配音后端是 ") + backend.name +
                "，摇不了音色。制作音色要的是进程内配音（[tts].backend = "
                "local）——它不给参考音频时会随机摇一个说话人，而那正是"
                "「制作」的全部内容。");
    }

    // 顶栏那本账要看得见：摇一段要借配音槽，而那一槽和大模型抢同一张卡。
    pipeline::Activity act{"say", paths::to_utf8(store.root()), "",
                           "正在摇音色"};

    double seconds = 0;
    try {
        seconds = stages::render_voice_take(text, dest, seed);
    } catch (const std::exception& e) {
        throw ApiError(502, std::string("摇不出来：") + e.what());
    }

    json out = {{"rel", store.paths().rel(dest)},
                {"seed", seed},
                {"seconds", seconds}};
    // **基频是量出来的，不是我编的。** 男声大致 85~180 Hz、女声
    // 165~255 Hz；标出来比给一个"沉稳中年男"诚实，而且和角色资产里的
    // voice_gender 对得上。量不出来就不给这一项，不要瞎猜一个。
    const auto f0 = stages::estimate_wav_f0(dest);
    if (f0.has_value()) out["hz"] = std::lround(*f0);
    return out;
}

}  // namespace

const std::vector<unsigned int>& preset_voice_seeds() {
    // 八个。多了用户听不过来（一段几秒，八段就是一分钟）。
    static const std::vector<unsigned int> kSeeds = {
        1101, 2027, 3313, 4409, 5521, 6637, 7743, 8859,
    };
    return kSeeds;
}

ApiResult post_voice_take(const in_json& body) {
    ProjectStore store = open_or_400(body);
    const unsigned int seed = seed_of(body);
    const std::string text =
        body.is_object() && body.contains("text") && body.at("text").is_string()
            ? body.at("text").get<std::string>()
            : std::string(kTakeText);

    // **落点是固定的一个临时文件。** 摇是随手点的，一次点几十下；
    // 按种子起名的话 voices/ 里会堆满再也用不上的 wav，而它们和真正
    // 存下来的音色混在同一个目录里，看着像出了一堆废文件。
    const fs::path dest = store.paths().voices() / ".take.wav";
    return {200, render_into(store, dest, text, seed)};
}

ApiResult post_voice_save(const in_json& body) {
    ProjectStore store = open_or_400(body);
    if (!body.is_object() || !body.contains("seed") ||
        !body.at("seed").is_number()) {
        throw ApiError(400, "没给 seed。存的是「这一摇」，而这一摇由种子定");
    }
    const unsigned int seed = static_cast<unsigned int>(
        body.at("seed").get<double>());
    const std::string name =
        body.contains("name") && body.at("name").is_string()
            ? body.at("name").get<std::string>()
            : std::string();
    const std::string stem = slugify(name, seed);

    const fs::path dest =
        store.paths().voices() / paths::from_utf8(stem + ".wav");
    json out = render_into(store, dest, kSaveText, seed);
    out["saved"] = store.paths().rel(dest);
    out["name"] = stem;

    // 给了角色就顺手挂上去。**不挂的话用户存完还得再去那一栏点一下**，
    // 而他刚刚就是在那一栏里摇的。
    if (body.contains("char_id") && body.at("char_id").is_string()) {
        const std::string char_id = body.at("char_id").get<std::string>();
        if (!char_id.empty()) {
            AssetLibrary assets = store.load_assets();
            const auto it = assets.characters.find(char_id);
            if (it == assets.characters.end()) {
                throw ApiError(404, "没有角色 " + char_id);
            }
            it->second.voice_id = out["saved"].get<std::string>();
            store.save_assets(assets);
            out["char_id"] = char_id;
        }
    }
    return {200, std::move(out)};
}

}  // namespace changji::http
