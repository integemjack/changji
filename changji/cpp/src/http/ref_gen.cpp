#include "http/ref_gen.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

#include "config/runtime.hpp"
#include "http/job_stream.hpp"
#include "http/offload.hpp"
#include "http/reset.hpp"
#include "http/upload.hpp"
#include "infer/scheduler.hpp"
#include "infer/sd_image.hpp"
#include "models/project.hpp"
#include "pipeline/episode.hpp"  // frame_spec
#include "pipeline/activity.hpp"
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

std::string opt_str(const json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body.at(key).is_string()) {
        return {};
    }
    return text::strip_ws(body.at(key).get<std::string>());
}

bool opt_bool(const json& body, const char* key) {
    return body.is_object() && body.contains(key) && body.at(key).is_boolean() &&
           body.at(key).get<bool>();
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
                    std::int64_t seed, const std::string& aspect_ratio,
                    const std::string& stream_id) {
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

    // **登记到"在干的活"里去，登记在借槽之前。** 这个接口是同步的，
    // 没有任务表那一套，所以它以前在界面上整个不可见——2026-09-11 撞上过：
    // 用户这边正出着参考图（占着图像槽），另一头的批量写作四章全挂在
    // 「显存不够加载 LLM：「图像」正用着」，而顶栏一片安静、GPU 占用 0%，
    // 挡路的那件事只能登服务器翻日志才查得到。
    //
    // 登记在 acquire 之前，是因为**等显存也是在忙**：头一张要先把出图模型
    // 读进显存（十几秒到一分钟），这段时间 sd.cpp 的回调一次都不触发，
    // 界面上就是一个不动的转圈——那正是最需要顶栏说句话的时候。
    pipeline::Activity act{"image", paths::to_utf8(store.root()), "",
                           "正在画参考图"};

    // **借不到就排队等**，不当场抛。撞车的常态是"另一边正在写一章"
    // （一两分钟），当场抛的话用户得到一个 500，而他唯一能做的就是过会儿
    // 再点一次——那正是机器该替他做的事。排队的时候顶栏那句话会变成
    // 「排队中，等「LLM」用完」。
    infer::Scheduler::AcquireOptions opt;
    opt.work = static_cast<std::size_t>(spec.width) * spec.height;
    opt.wait = infer::kAcquireWait;
    opt.on_queued = pipeline::note_queued;
    auto lease = infer::scheduler().acquire(infer::Slot::Image, opt);
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
        ctx->generate(req, dest, tok,
                      [&act, &stream_id](int step, int steps, double,
                                         bool loading) {
                          // 读权重和采样不是一个量级（1927 个张量 vs 8 步），
                          // 画在同一条进度条上会像"跑到头又倒回去了"。
                          // 顶栏只有一行，就只画采样那一段。
                          if (loading) return;
                          act.set_progress(step, steps);
                          // 异步那条路上，点了按钮的人也在等这个数。
                          job_progress(stream_id, step, steps);
                      });
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

namespace {

/// 真画一张角色参考图并存盘。回的就是这个接口同步跑时那份 body。
///
/// **从接口里抽出来，因为它有两条调用路**（同步那条、后台那条），而两条
/// 必须跑同一段代码——抄一份的话两边迟早只改一边。
///
/// 自己重开一遍 store：后台那条跑起来时，接口那一帧的栈早没了，而重开
/// 就是读两个 json 文件，比起几十秒的出图不值一提。
json character_ref_job(const std::string& project_path,
                       const std::string& char_id, const std::string& slot,
                       std::int64_t seed, const std::string& stream_id) {
    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.characters.find(char_id);
    if (it == assets.characters.end()) throw ApiError(404, "没有角色 " + char_id);

    Character& c = it->second;
    const Rendered out = render_ref(
        store, char_id + "_" + slot,
        stages::build_character_ref_prompt(c, assets.style, slot),
        stages::ref_negative(assets.style), seed, assets.style.aspect_ratio,
        stream_id);

    if (slot == "front")              c.ref_front = out.rel;
    else if (slot == "three_quarter") c.ref_three_quarter = out.rel;
    else                              c.ref_back = out.rel;
    // **先出图再存盘。** 反过来的话出图失败会留下一条指向不存在的文件的
    // 路径，而界面上那个位置会显示成"已有参考图"——比没有更糟。
    store.save_assets(assets);

    // 和上传那条一样**无条件重跑**：参考图直接决定画面长什么样。
    return {
        {"saved", out.rel},
        {"slot", slot},
        {"seed", out.seed},
        {"width", out.width},
        {"height", out.height},
        {"steps", out.steps},
        {"seconds", out.seconds},
        {"reset_shots", reset_all_shots(store)},
    };
}

}  // namespace

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

    // **能当场判的先当场判。** 项目打不开、没这个角色，这两条要立刻回
    // 400/404——扔到后台去的话，用户点完看到的是"开始了"，几秒后才从
    // 另一条路上飘回来一句报错。
    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string char_id = need_str(body, "char_id");
    if (assets.characters.count(char_id) == 0) {
        throw ApiError(404, "没有角色 " + char_id);
    }

    const std::string project_path = paths::to_utf8(store.root());
    const std::int64_t seed = ref_seed(body, char_id + "_" + slot);
    const std::string stream_id = opt_str(body, "stream");

    // **异步那条：当场回一句"开始了"，活在后台线程上干。** 理由见
    // http/job_stream.hpp 开头那段——一张几十秒，而这几十秒里落在同一条
    // I/O 线程上的连接全都干等，顶栏那块表更是会直接冻住。
    //
    // 没有 stream 就照旧同步跑到底：结果没地方送回去。老客户端、curl、
    // 对拍脚本走的都是那条，一个字没变。
    if (opt_bool(body, "async") && !stream_id.empty()) {
        Offload::instance().post([project_path, char_id, slot, seed, stream_id] {
            try {
                job_done(stream_id, character_ref_job(project_path, char_id,
                                                      slot, seed, stream_id));
            } catch (const std::exception& e) {
                job_error(stream_id, e.what());
            }
        });
        return {202, {{"started", true}, {"stream", stream_id}}};
    }

    return {200, character_ref_job(project_path, char_id, slot, seed, stream_id)};
}

namespace {

/// 真画一张空景图并存盘。见 character_ref_job 上面那段。
json location_ref_job(const std::string& project_path,
                      const std::string& location_id, std::int64_t seed,
                      const std::string& stream_id) {
    ProjectStore store = open_project(project_path);
    AssetLibrary assets = store.load_assets();
    const auto it = assets.locations.find(location_id);
    if (it == assets.locations.end()) {
        throw ApiError(404, "没有场景 " + location_id);
    }

    Location& l = it->second;
    const Rendered out = render_ref(
        store, location_id + "_empty",
        stages::build_location_ref_prompt(l, assets.style),
        stages::ref_negative(assets.style), seed, assets.style.aspect_ratio,
        stream_id);

    l.ref_empty = out.rel;
    store.save_assets(assets);

    return {
        {"saved", out.rel},
        {"seed", out.seed},
        {"width", out.width},
        {"height", out.height},
        {"steps", out.steps},
        {"seconds", out.seconds},
        {"reset_shots", reset_all_shots(store)},
    };
}

}  // namespace

ApiResult post_location_reference_generate(const json& body) {
    ProjectStore store = open_project(need_str(body, "project"));
    AssetLibrary assets = store.load_assets();
    const std::string location_id = need_str(body, "location_id");
    if (assets.locations.count(location_id) == 0) {
        throw ApiError(404, "没有场景 " + location_id);
    }

    const std::string project_path = paths::to_utf8(store.root());
    const std::int64_t seed = ref_seed(body, location_id + "_empty");
    const std::string stream_id = opt_str(body, "stream");

    if (opt_bool(body, "async") && !stream_id.empty()) {
        Offload::instance().post([project_path, location_id, seed, stream_id] {
            try {
                job_done(stream_id, location_ref_job(project_path, location_id,
                                                     seed, stream_id));
            } catch (const std::exception& e) {
                job_error(stream_id, e.what());
            }
        });
        return {202, {{"started", true}, {"stream", stream_id}}};
    }

    return {200, location_ref_job(project_path, location_id, seed, stream_id)};
}

}  // namespace changji::http
