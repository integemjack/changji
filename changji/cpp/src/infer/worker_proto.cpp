#include "infer/worker_proto.hpp"

#include <stdexcept>

#include "util/text.hpp"

namespace changji::infer {

using nlohmann::json;

const char* to_string(TaskKind k) {
    switch (k) {
        case TaskKind::Video: return "video";
        case TaskKind::Tts: return "tts";
        case TaskKind::Frame: break;
    }
    return "frame";
}

std::optional<TaskKind> task_kind_from(const std::string& s) {
    if (s == "frame") return TaskKind::Frame;
    if (s == "video") return TaskKind::Video;
    if (s == "tts") return TaskKind::Tts;
    return std::nullopt;
}

namespace {

/// 取必填字段。**缺了就抛，别用默认值糊过去**——
/// 一个没带 dest 的任务默默写到当前目录去，比当场报错难查得多。
const json& need(const json& j, const char* key) {
    if (!j.is_object()) throw std::runtime_error("任务不是一个对象");
    const auto it = j.find(key);
    if (it == j.end()) {
        throw std::runtime_error(std::string("任务里缺 ") + key);
    }
    return *it;
}

}  // namespace

json to_json(const Task& t) {
    json j{
        {"kind", to_string(t.kind)},
        {"shot_id", t.shot_id},
        {"prompts",
         json{{"positive", t.prompts.positive},
              {"negative", t.prompts.negative},
              {"reference_images", t.prompts.reference_images}}},
        {"spec", t.spec},
        {"frames", t.frames},
        {"motion", t.motion},
        {"style_line", t.style_line},
        {"tier", t.tier},
        {"dest", t.dest},
        {"seed", t.seed},
        {"return_artifact", t.return_artifact},
        {"text", t.text},
        {"voice_id", t.voice_id},
        {"emotion", t.emotion},
        {"intensity", t.intensity},
    };
    if (t.start_image) j["start_image"] = *t.start_image;
    return j;
}

Task task_from_json(const json& j) {
    Task t;
    const auto kind = task_kind_from(need(j, "kind").get<std::string>());
    if (!kind) {
        throw std::runtime_error("任务的 kind 只能是 frame / video / tts");
    }
    t.kind = *kind;
    t.shot_id = need(j, "shot_id").get<std::string>();
    t.dest = need(j, "dest").get<std::string>();

    const auto& p = need(j, "prompts");
    t.prompts.positive = p.value("positive", std::string());
    t.prompts.negative = p.value("negative", std::string());
    if (p.contains("reference_images") && p["reference_images"].is_array()) {
        t.prompts.reference_images =
            p["reference_images"].get<std::vector<std::string>>();
    }

    t.spec = need(j, "spec").get<models::TierSpec>();
    t.frames = j.value("frames", 0);
    t.motion = j.value("motion", std::string());
    if (j.contains("style_line")) {
        t.style_line = j["style_line"].get<models::StyleLine>();
    }
    if (j.contains("tier")) t.tier = j["tier"].get<models::Tier>();
    if (j.contains("start_image") && j["start_image"].is_string()) {
        t.start_image = j["start_image"].get<std::string>();
    }
    // **种子必须带**。让工作进程自己算的话它不知道 attempts，
    // 算出来的图和串行跑的不一样——那样并行就不是"更快"，是"结果变了"。
    t.seed = need(j, "seed").get<std::int64_t>();
    // **默认假**：老版本的派活方不带这个字段，那时候它要的就是老行为
    // （直接写 dest，两头共享文件系统）。
    t.return_artifact = j.value("return_artifact", false);
    t.text = j.value("text", std::string());
    t.voice_id = j.value("voice_id", std::string());
    t.emotion = j.value("emotion", std::string());
    t.intensity = j.value("intensity", 0.0);

    // **在入口处判掉非法 UTF-8。** nlohmann 解析时照单全收，dump 时才抛
    // type_error.316——那时候已经出了这儿的 try 块，派活方拿到的是一个
    // 空白的 500。跨机时两头编码不一样是迟早的事（2026-09-12 实撞：
    // 请求体是 GBK 的中文台词，日志里只有一行 invalid UTF-8 byte）。
    for (const auto& [what, s] : std::vector<std::pair<const char*, const std::string*>>{
             {"shot_id", &t.shot_id},
             {"dest", &t.dest},
             {"text", &t.text},
             {"voice_id", &t.voice_id},
             {"emotion", &t.emotion},
             {"motion", &t.motion},
             {"prompts.positive", &t.prompts.positive},
             {"prompts.negative", &t.prompts.negative},
         }) {
        if (!text::is_valid_utf8(*s)) {
            throw std::runtime_error(
                std::string(what) +
                " 不是合法的 UTF-8。派活那头多半没按 UTF-8 编码"
                "（Windows 上用 GBK 发中文就会这样）");
        }
    }
    return t;
}

json to_json(const TaskResult& r) {
    return json{{"ok", r.ok},
                {"error", r.error},
                {"dest", r.dest},
                {"artifact_id", r.artifact_id},
                {"duration_s", r.duration_s}};
}

TaskResult task_result_from_json(const json& j) {
    TaskResult r;
    r.ok = j.value("ok", false);
    r.error = j.value("error", std::string());
    r.dest = j.value("dest", std::string());
    r.artifact_id = j.value("artifact_id", std::string());
    r.duration_s = j.value("duration_s", 0.0);
    return r;
}

json to_json(const TaskProgress& p) {
    json j{{"state", p.state},
           {"step", p.step},
           {"steps", p.steps},
           {"loading", p.loading}};
    if (p.result) j["result"] = to_json(*p.result);
    return j;
}

TaskProgress task_progress_from_json(const json& j) {
    TaskProgress p;
    p.state = j.value("state", std::string("queued"));
    p.step = j.value("step", 0);
    p.steps = j.value("steps", 0);
    p.loading = j.value("loading", false);
    if (j.contains("result") && j["result"].is_object()) {
        p.result = task_result_from_json(j["result"]);
    }
    return p;
}

std::string worker_rejected_message(int status, const std::string& body) {
    // 截 200 字符。**按字符，不按字节**——见头文件里那段。
    return "工作进程拒了这个任务（" + std::to_string(status) + "）：" +
           text::truncate_utf8(body, 200);
}

std::string worker_bad_accept_message(const std::string& body) {
    return "工作进程回的不是 {\"id\":...}：" + text::truncate_utf8(body, 200);
}

}  // namespace changji::infer
