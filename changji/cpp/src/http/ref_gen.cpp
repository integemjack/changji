#include "http/ref_gen.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

#include "config/runtime.hpp"
#include "http/reset.hpp"
#include "http/upload.hpp"
#include "infer/scheduler.hpp"
#include "infer/sd_image.hpp"
#include "models/project.hpp"
#include "pipeline/episode.hpp"  // frame_spec
#include "pipeline/jobs.hpp"
#include "stages/ref_images.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::models;

std::string need_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        throw ApiError(400, std::string("请求里缺少字符串字段 ") + key);
    }
    return body.at(key).get<std::string>();
}

ProjectStore open_project(const std::string& path) {
    if (path.empty()) throw ApiError(400, "没有指定项目目录");
    return ProjectStore(paths::from_utf8(path));
}


struct Rendered {
    std::string rel;
    std::int64_t seed = 0;
    int width = 0;
    int height = 0;
    int steps = 0;
    double seconds = 0.0;
};

/// 真出一张图，写进 refs/。
///
/// 画幅和步数**跟这部剧的首帧完全一样**（项目的 `[video]` 盖在档位表上，
/// 见 pipeline::apply_project_spec）。刻意不另设一档：参考图是拿去喂首帧
/// 的，比首帧小等于先把细节丢掉再让模型照着画，比首帧大只是白花时间——
/// 实测 5090 上多花一倍（544×928 约一分钟，1088×1920 要两分钟）。
Rendered render_ref(const ProjectStore& store, const std::string& stem,
                    const std::string& positive, const std::string& negative,
                    std::int64_t seed, const std::string& aspect_ratio) {
    // **项目自己的 changji.toml 盖在全局上**，和跑流水线走同一条路。
    config::Settings settings = config::load_settings(store.root());
    HardwareProfile profile = config::runtime().profile();
    pipeline::apply_project_spec(settings, profile);
    const TierSpec spec =
        pipeline::frame_spec(profile, settings).scaled_to(aspect_ratio);
    if (spec.width <= 0 || spec.height <= 0 || spec.steps <= 0) {
        throw ApiError(500, "档位表里没有首帧那一档，出不了图。先去设置页体检一下");
    }

    const fs::path dest = claim_ref_path(store, stem, ".png");

    // 借图像槽。**借之前不报任何进度**——这个接口是同步的，界面上就是一个
    // 转圈。头一张要先把出图模型读进显存（十几秒到一分钟），这段时间
    // sd.cpp 的回调一次都不会触发，看着像卡住了，实际是在读权重。
    auto lease = infer::scheduler().acquire(
        infer::Slot::Image,
        static_cast<std::size_t>(spec.width) * spec.height);
    auto ctx = infer::current_image_context();
    if (!ctx) throw ApiError(503, "出图后端没准备好，这个版本大概没链 sd.cpp");

    infer::ImageRequest req;
    req.positive = positive;
    req.negative = negative;
    req.width = spec.width;
    req.height = spec.height;
    req.steps = spec.steps;
    req.seed = seed;
    // req.tag 留空：预览是往镜头墙上那一格推的，参考图没有格子。

    const auto t0 = std::chrono::steady_clock::now();
    pipeline::CancelToken tok;
    try {
        ctx->generate(req, dest, tok, [](int, int, double, bool) {});
    } catch (const infer::SdError& e) {
        throw ApiError(500, std::string("出图失败：") + e.what());
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();

    std::error_code ec;
    if (!fs::is_regular_file(dest, ec)) {
        throw ApiError(500, "出图那一步没报错，但文件没落地：" +
                                paths::to_utf8(dest));
    }

    return {store.paths().rel(dest), seed, spec.width, spec.height, spec.steps,
            seconds};
}

}  // namespace

std::int64_t ref_seed(const json& body, const std::string& stem) {
    if (body.is_object() && body.contains("seed") &&
        body.at("seed").is_number_integer()) {
        const std::int64_t given = body.at("seed").get<std::int64_t>();
        // 负数和超范围的都折回正数区间，和 frame_seed 一个规矩
        const std::int64_t wrapped = given % 2147483648LL;
        return wrapped < 0 ? wrapped + 2147483648LL : wrapped;
    }
    const std::string hex = text::sha1_hex("ref:" + stem);
    const auto base =
        static_cast<std::uint32_t>(std::stoul(hex.substr(0, 8), nullptr, 16));
    return static_cast<std::int64_t>(base % 2147483648u);
}

ApiResult post_character_reference_generate(const json& body) {
    const std::string slot =
        body.is_object() && body.contains("slot") && body.at("slot").is_string()
            ? body.at("slot").get<std::string>()
            : std::string("front");
    static const std::set<std::string> kSlots = {"front", "three_quarter",
                                                 "back"};
    if (kSlots.count(slot) == 0) {
        throw ApiError(400, "只有正面、四分之三侧面、背面三个位置");
    }

    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string char_id = need_str(body, "char_id");
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, "没有角色 " + char_id);

    Character& c = it->second;
    const std::string stem = char_id + "_" + slot;
    const Rendered out = render_ref(
        store, stem, stages::build_character_ref_prompt(c, assets.style, slot),
        stages::ref_negative(assets.style), ref_seed(body, stem),
        assets.style.aspect_ratio);

    if (slot == "front")              c.ref_front = out.rel;
    else if (slot == "three_quarter") c.ref_three_quarter = out.rel;
    else                              c.ref_back = out.rel;
    // **先出图再存盘。** 反过来的话出图失败会留下一条指向不存在的文件的
    // 路径，而界面上那个位置会显示成"已有参考图"——比没有更糟。
    store.save_assets(assets);

    // 和上传那条一样**无条件重跑**：参考图直接决定画面长什么样。
    return {200, {
        {"saved", out.rel},
        {"slot", slot},
        {"seed", out.seed},
        {"width", out.width},
        {"height", out.height},
        {"steps", out.steps},
        {"seconds", out.seconds},
        {"reset_shots", reset_all_shots(store)},
    }};
}

ApiResult post_location_reference_generate(const json& body) {
    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string location_id = need_str(body, "location_id");
    const auto it = assets.locations.find(location_id);
    if (it == assets.locations.end()) {
        throw ApiError(404, "没有场景 " + location_id);
    }

    Location& l = it->second;
    const std::string stem = location_id + "_empty";
    const Rendered out = render_ref(
        store, stem, stages::build_location_ref_prompt(l, assets.style),
        stages::ref_negative(assets.style), ref_seed(body, stem),
        assets.style.aspect_ratio);

    l.ref_empty = out.rel;
    store.save_assets(assets);

    return {200, {
        {"saved", out.rel},
        {"seed", out.seed},
        {"width", out.width},
        {"height", out.height},
        {"steps", out.steps},
        {"seconds", out.seconds},
        {"reset_shots", reset_all_shots(store)},
    }};
}

}  // namespace changji::http
