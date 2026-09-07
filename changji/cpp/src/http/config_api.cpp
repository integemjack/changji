#include "http/config_api.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "config/field_names.inc.hpp"
#include "config/runtime.hpp"
#include "config/writeback.hpp"
#include "pipeline/jobs.hpp"
#include "util/paths.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

using namespace changji::config;

/// 字段名 -> 中文标签。表是从 Python 的 _FIELD_NAMES 生成的。
const std::map<std::string, std::string>& field_names() {
    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> out;
        for (const auto& row : stages::prompt::kFieldNames) {
            out[row[0]] = row[1];
        }
        return out;
    }();
    return m;
}

/// 字段名 -> {配置节, 节内键名}。
const std::map<std::string, std::pair<std::string, std::string>>&
setting_sections() {
    static const std::map<std::string, std::pair<std::string, std::string>> m =
        [] {
            std::map<std::string, std::pair<std::string, std::string>> out;
            for (const auto& row : stages::prompt::kSettingSections) {
                out[row[0]] = {row[1], row[2]};
            }
            return out;
        }();
    return m;
}

/// 报「已应用 min_frame_similarity」不如报「与首帧相似度下限」。
/// 界面上的标签是中文，回执用英文字段名，用户得自己对着猜是哪一项。
json labels_for(const std::vector<std::string>& keys) {
    json out = json::array();
    for (const auto& k : keys) {
        const auto it = field_names().find(k);
        out.push_back(it != field_names().end() ? it->second : k);
    }
    return out;
}

json checks_json(const doctor::Report& r) {
    json out = json::array();
    for (const auto& c : r.checks) {
        out.push_back({{"name", c.name},
                       {"level", doctor::to_string(c.level)},
                       {"detail", c.detail},
                       {"fix", c.fix}});
    }
    return out;
}

void forbid_extra(const json& body, const std::set<std::string>& allowed,
                  const char* where) {
    if (!body.is_object()) throw ApiError(400, "请求体要是一个对象");
    for (const auto& kv : body.items()) {
        if (allowed.count(kv.key()) == 0) {
            throw unprocessable_top(std::string(where) + kv.key(),
                                    "Extra inputs are not permitted",
                                    kv.value(), "extra_forbidden");
        }
    }
}

bool opt_bool(const json& body, const char* key, bool def) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

/// 取一个数，类型不对抛 422。
double need_num(const json& v, const std::string& key) {
    if (!v.is_number()) {
        throw unprocessable_top(key, "Input should be a valid number", v,
                                "float_type");
    }
    return v.get<double>();
}

int need_int(const json& v, const std::string& key) {
    if (!v.is_number_integer()) {
        throw unprocessable_top(key, "Input should be a valid integer", v,
                                "int_type");
    }
    return v.get<int>();
}

std::string need_string(const json& v, const std::string& key) {
    if (!v.is_string()) {
        throw unprocessable_top(key, "Input should be a valid string", v,
                                "string_type");
    }
    return v.get<std::string>();
}

bool need_bool(const json& v, const std::string& key) {
    if (!v.is_boolean()) {
        throw unprocessable_top(key, "Input should be a valid boolean", v,
                                "bool_type");
    }
    return v.get<bool>();
}

/// 把 validate() 收集的错误压成一句人话，字段名换成中文。
///
/// 原样抛出去的话用户看到的是一串英文字段名，
/// 真正有用的那句提示埋在中间，等于没提示。
std::string readable(const std::vector<std::string>& errs) {
    std::string out;
    for (std::size_t i = 0; i < errs.size(); ++i) {
        if (i) out += "；";
        std::string line = errs[i];
        // 错误消息形如 "llm.temperature 必须在 0 和 2 之间"，
        // 把前面那个字段名换成中文标签
        const std::size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            std::string field = line.substr(0, sp);
            const std::size_t dot = field.rfind('.');
            const std::string bare =
                dot == std::string::npos ? field : field.substr(dot + 1);
            const auto it = field_names().find(bare);
            if (it != field_names().end()) {
                line = it->second + line.substr(sp);
            }
        }
        out += line;
    }
    return out;
}

/// patch 里那个 key 归哪个配置节。connections 的键都是 `节_字段` 的形式。
std::pair<std::string, std::string> split_conn_key(const std::string& key) {
    const std::size_t us = key.find('_');
    if (us == std::string::npos) return {key, ""};
    return {key.substr(0, us), key.substr(us + 1)};
}

}  // namespace

ApiResult get_connections() {
    const Settings s = runtime().snapshot();
    const std::string key = s.llm.api_key;

    // 只回一个"设了没有"和一个掐头去尾的提示。回明文的话它会进浏览器的
    // 网络面板、进前端的状态、进任何一次截图。
    const std::string hint =
        key.size() > 6 ? key.substr(0, 2) + "***" + key.substr(key.size() - 2)
                       : "***";

    json env = json::object();
    for (const auto& kv : env_overridden()) env[kv.first] = kv.second;

    return {200, {
        {"comfy_base_url", s.comfy.base_url},
        {"comfy_job_timeout_s", s.comfy.job_timeout_s},
        {"comfy_max_retries", s.comfy.max_retries},
        {"llm_base_url", s.llm.base_url},
        {"llm_model", s.llm.model},
        {"llm_api_key_set", !key.empty()},
        {"llm_api_key_hint", hint},
        {"llm_temperature", s.llm.temperature},
        {"tts_backend", s.tts.backend},
        {"tts_base_url", s.tts.base_url.value_or("")},
        {"vram_gb_override", s.vram_gb_override.has_value()
                                 ? json(*s.vram_gb_override)
                                 : json(nullptr)},
        {"tts_engine", s.tts.engine},
        {"config_file", paths::to_utf8(user_config_path())},
        // 被环境变量顶住的字段，改了也是白改，界面要说出来
        {"env_locked", env},
    }};
}

ApiResult post_connections(const json& body, const DoctorFn& check) {
    forbid_extra(body, {"patch", "persist"}, "");
    const auto pit = body.find("patch");
    if (pit == body.end() || !pit->is_object()) {
        throw unprocessable_top("patch", "Field required", nullptr, "missing");
    }
    const json& patch = *pit;
    static const std::set<std::string> kAllowed = {
        "comfy_base_url", "comfy_job_timeout_s", "comfy_max_retries",
        "llm_base_url", "llm_model", "llm_api_key", "llm_temperature",
        "tts_backend", "tts_base_url", "tts_engine", "vram_gb_override"};
    forbid_extra(patch, kAllowed, "patch.");

    // 正在跑的时候换机器会把这一集跑坏：前半集是一台机器出的，
    // 后半集是另一台，画风对不上。
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, "正在跑，这时候换机器会把这一集跑坏");
    }

    // 值为 null 的当作没给，对应 pydantic 的 exclude_none=True
    std::map<std::string, json> data;
    for (const auto& kv : patch.items()) {
        if (!kv.value().is_null()) data[kv.key()] = kv.value();
    }
    if (data.empty()) throw ApiError(400, "没有要改的项");

    Settings s = runtime().snapshot();
    std::vector<std::string> changed;

    const auto note = [&changed](const std::string& key, bool differs) {
        if (differs) changed.push_back(key);
    };

    for (const auto& kv : data) {
        const std::string& key = kv.first;
        const json& v = kv.second;
        if (key == "vram_gb_override") continue;   // 下面单独处理

        const auto [section, field] = split_conn_key(key);
        if (section == "comfy") {
            if (field == "base_url") {
                const auto x = need_string(v, key);
                note(key, s.comfy.base_url != x);
                s.comfy.base_url = x;
            } else if (field == "job_timeout_s") {
                const auto x = need_num(v, key);
                note(key, s.comfy.job_timeout_s != x);
                s.comfy.job_timeout_s = x;
            } else if (field == "max_retries") {
                const auto x = need_int(v, key);
                note(key, s.comfy.max_retries != x);
                s.comfy.max_retries = x;
            }
        } else if (section == "llm") {
            if (field == "base_url") {
                const auto x = need_string(v, key);
                note(key, s.llm.base_url != x);
                s.llm.base_url = x;
            } else if (field == "model") {
                const auto x = need_string(v, key);
                note(key, s.llm.model != x);
                s.llm.model = x;
            } else if (field == "api_key") {
                const auto x = need_string(v, key);
                note(key, s.llm.api_key != x);
                s.llm.api_key = x;
            } else if (field == "temperature") {
                const auto x = need_num(v, key);
                note(key, s.llm.temperature != x);
                s.llm.temperature = x;
            }
        } else if (section == "tts") {
            if (field == "backend") {
                const auto x = need_string(v, key);
                note(key, s.tts.backend != x);
                s.tts.backend = x;
            } else if (field == "engine") {
                const auto x = need_string(v, key);
                note(key, s.tts.engine != x);
                s.tts.engine = x;
            } else if (field == "base_url") {
                const auto x = need_string(v, key);
                note(key, s.tts.base_url.value_or("") != x);
                // 空串当作没配。留一个空串的话，backend 选 http 时
                // 会拿它去连，报的错是"连不上 "——后面什么都没有。
                s.tts.base_url = x.empty() ? std::optional<std::string>()
                                           : std::optional<std::string>(x);
            }
        }
    }

    if (data.count("vram_gb_override")) {
        const json& v = data.at("vram_gb_override");
        const double x = need_num(v, "vram_gb_override");
        if (s.vram_gb_override.value_or(0.0) != x) {
            changed.push_back("vram_gb_override");
        }
        // 对应 Python 的 `data[...] or None`：0 当作没设
        s.vram_gb_override = x == 0.0 ? std::optional<double>()
                                      : std::optional<double>(x);
    }

    const auto errs = s.validate();
    if (!errs.empty()) throw ApiError(400, readable(errs));
    if (s.tts.backend != "comfy" && s.tts.backend != "http") {
        throw ApiError(400, "配音后端只能是 comfy 或 http");
    }
    if (s.tts.backend == "http" && !s.tts.base_url.has_value()) {
        throw ApiError(400, "配音后端选 http 就必须填地址");
    }

    runtime().replace(s);

    json saved_to = nullptr;
    if (opt_bool(body, "persist", true) && !changed.empty()) {
        json payload = json::object();
        for (const auto& key : changed) {
            if (key == "vram_gb_override") {
                payload["vram_gb_override"] =
                    s.vram_gb_override.has_value() ? json(*s.vram_gb_override)
                                                   : json("");
                continue;
            }
            const auto [section, field] = split_conn_key(key);
            json value;
            if (section == "comfy") {
                if (field == "base_url") value = s.comfy.base_url;
                else if (field == "job_timeout_s") value = s.comfy.job_timeout_s;
                else if (field == "max_retries") value = s.comfy.max_retries;
            } else if (section == "llm") {
                if (field == "base_url") value = s.llm.base_url;
                else if (field == "model") value = s.llm.model;
                else if (field == "api_key") value = s.llm.api_key;
                else if (field == "temperature") value = s.llm.temperature;
            } else if (section == "tts") {
                if (field == "backend") value = s.tts.backend;
                else if (field == "engine") value = s.tts.engine;
                // 对应 Python 的 "" if value is None else value
                else if (field == "base_url") value = s.tts.base_url.value_or("");
            }
            payload[section][field] = value;
        }
        try {
            saved_to = paths::to_utf8(save_user_config(payload));
        } catch (const std::exception& e) {
            throw ApiError(500, std::string("配置写不进去：") + e.what());
        }
    }

    // 改完立刻重新体检。比让用户自己去点一下体检按钮有用得多——
    // 大部分人改完就走了，不通要到跑流水线时才发现。
    const doctor::Report report = check(s);

    const auto locked = env_overridden();
    json env_changed = json::array();
    for (const auto& key : changed) {
        if (locked.count(key)) env_changed.push_back(key);
    }
    std::sort(env_changed.begin(), env_changed.end());

    return {200, {
        {"changed", changed},
        {"labels", labels_for(changed)},
        {"saved_to", saved_to},
        // 写进文件了，但下次启动环境变量还是会把它顶回去
        {"env_locked", env_changed},
        {"can_run", report.can_run()},
        {"checks", checks_json(report)},
    }};
}

ApiResult post_settings(const json& body) {
    if (!body.is_object()) throw ApiError(400, "请求体要是一个对象");

    // 正在跑的时候改参数会让这一集前后不一致：前十个镜头一种码率，
    // 后十个另一种，拼起来能看出接缝。
    if (pipeline::jobs().running(pipeline::JobKind::Run)) {
        throw ApiError(409, "正在跑，改参数会让这一集前后不一致");
    }

    // 老写法是把字段直接摊在请求体里，新写法包在 patch 里。两种都收，
    // 免得刷新慢一步的页面点一下就报 422。
    json patch;
    bool persist = false;
    if (body.contains("patch")) {
        forbid_extra(body, {"patch", "persist"}, "");
        if (!body.at("patch").is_object()) {
            throw unprocessable_top("patch", "Input should be a valid dictionary",
                                    body.at("patch"), "dict_type");
        }
        patch = body.at("patch");
        persist = opt_bool(body, "persist", false);
    } else {
        patch = body;
    }

    static const std::set<std::string> kAllowed = {
        "draft_width", "draft_height", "draft_steps",
        "final_width", "final_height", "final_steps",
        "fps", "crf", "subtitle_font", "subtitle_max_chars_per_line",
        "subtitle_max_lines", "scene_transition_s",
        "gates_enabled", "max_attempts_per_shot", "min_pixel_std",
        "min_frame_similarity", "max_audio_drift_s", "target_lufs",
        "fallback_on_exhausted", "tts_tolerance_s", "tts_max_tempo_shift"};
    forbid_extra(patch, kAllowed, "patch.");

    std::map<std::string, json> data;
    for (const auto& kv : patch.items()) {
        if (!kv.value().is_null()) data[kv.key()] = kv.value();
    }

    std::vector<std::string> changed;
    Settings s = runtime().snapshot();
    models::HardwareProfile profile = runtime().profile();

    // ---- 画质档位 ----
    //
    // 只在进程内生效，不写回配置：档位是按显存推出来的，写死在配置里
    // 等于把这台机器的显存刻进项目，换台机器就不对了。
    for (const auto& [tier, prefix] :
         std::vector<std::pair<models::Tier, std::string>>{
             {models::Tier::DRAFT, "draft"}, {models::Tier::FINAL, "final"}}) {
        const auto tit = profile.tiers.find(tier);
        if (tit == profile.tiers.end()) continue;
        models::TierSpec spec = tit->second;
        bool touched = false;
        for (const char* field : {"width", "height", "steps"}) {
            const std::string key = prefix + "_" + field;
            const auto dit = data.find(key);
            if (dit == data.end()) continue;
            const int v = need_int(dit->second, key);
            // 分辨率必须是 32 的倍数，否则 Wan 的潜空间对不齐
            if (std::string(field) == "width" && v % 32) {
                throw ApiError(400, "宽度必须是 32 的倍数");
            }
            if (std::string(field) == "height" && v % 32) {
                throw ApiError(400, "高度必须是 32 的倍数");
            }
            if (std::string(field) == "width") spec.width = v;
            else if (std::string(field) == "height") spec.height = v;
            else spec.steps = v;
            changed.push_back(key);
            touched = true;
        }
        if (touched) runtime().set_tier_override(tier, spec);
    }

    // ---- 装配、闸门、配音 ----
    const auto take_int = [&](const char* key, int& dest) {
        const auto it = data.find(key);
        if (it == data.end()) return;
        dest = need_int(it->second, key);
        changed.push_back(key);
    };
    const auto take_num = [&](const char* key, double& dest) {
        const auto it = data.find(key);
        if (it == data.end()) return;
        dest = need_num(it->second, key);
        changed.push_back(key);
    };
    const auto take_str = [&](const char* key, std::string& dest) {
        const auto it = data.find(key);
        if (it == data.end()) return;
        dest = need_string(it->second, key);
        changed.push_back(key);
    };
    const auto take_bool = [&](const char* key, bool& dest) {
        const auto it = data.find(key);
        if (it == data.end()) return;
        dest = need_bool(it->second, key);
        changed.push_back(key);
    };

    take_int("fps", s.assembly.fps);
    take_int("crf", s.assembly.crf);
    take_str("subtitle_font", s.assembly.subtitle_font);
    take_int("subtitle_max_chars_per_line", s.assembly.subtitle_max_chars_per_line);
    take_int("subtitle_max_lines", s.assembly.subtitle_max_lines);
    take_num("scene_transition_s", s.assembly.scene_transition_s);

    take_int("max_attempts_per_shot", s.gates.max_attempts_per_shot);
    take_num("min_pixel_std", s.gates.min_pixel_std);
    take_num("min_frame_similarity", s.gates.min_frame_similarity);
    take_num("max_audio_drift_s", s.gates.max_audio_drift_s);
    take_num("target_lufs", s.gates.target_lufs);
    take_bool("fallback_on_exhausted", s.gates.fallback_on_exhausted);
    take_bool("gates_enabled", s.gates.enabled);

    take_num("tts_tolerance_s", s.tts.tolerance_s);
    take_num("tts_max_tempo_shift", s.tts.max_tempo_shift);

    // 校验不过就整体回滚——半套改动比不改更糟，用户看到"已应用"
    // 但配置是残的。
    const auto errs = s.validate();
    if (!errs.empty()) throw ApiError(400, readable(errs));
    runtime().replace(s);

    json saved_to = nullptr;
    if (persist && !changed.empty()) {
        json payload = json::object();
        for (const auto& key : changed) {
            const auto it = setting_sections().find(key);
            if (it == setting_sections().end()) continue;   // 档位不写回
            const auto& [section, field] = it->second;
            json value;
            if (section == "assembly") {
                if (field == "fps") value = s.assembly.fps;
                else if (field == "crf") value = s.assembly.crf;
                else if (field == "subtitle_font") value = s.assembly.subtitle_font;
                else if (field == "subtitle_max_chars_per_line")
                    value = s.assembly.subtitle_max_chars_per_line;
                else if (field == "subtitle_max_lines")
                    value = s.assembly.subtitle_max_lines;
                else if (field == "scene_transition_s")
                    value = s.assembly.scene_transition_s;
            } else if (section == "gates") {
                if (field == "enabled") value = s.gates.enabled;
                else if (field == "max_attempts_per_shot")
                    value = s.gates.max_attempts_per_shot;
                else if (field == "min_pixel_std") value = s.gates.min_pixel_std;
                else if (field == "min_frame_similarity")
                    value = s.gates.min_frame_similarity;
                else if (field == "max_audio_drift_s")
                    value = s.gates.max_audio_drift_s;
                else if (field == "target_lufs") value = s.gates.target_lufs;
                else if (field == "fallback_on_exhausted")
                    value = s.gates.fallback_on_exhausted;
            } else if (section == "tts") {
                if (field == "tolerance_s") value = s.tts.tolerance_s;
                else if (field == "max_tempo_shift") value = s.tts.max_tempo_shift;
            }
            if (!value.is_null()) payload[section][field] = value;
        }
        if (!payload.empty()) {
            try {
                saved_to = paths::to_utf8(save_user_config(payload));
            } catch (const std::exception& e) {
                throw ApiError(500, std::string("配置写不进去：") + e.what());
            }
        }
    }

    return {200, {
        {"changed", changed},
        {"labels", labels_for(changed)},
        {"saved_to", saved_to},
    }};
}

}  // namespace changji::http
