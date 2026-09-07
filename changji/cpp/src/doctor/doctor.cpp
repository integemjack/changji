#include "doctor/doctor.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "util/paths.hpp"
#include "util/proc.hpp"

namespace changji::doctor {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char* to_string(Level level) {
    switch (level) {
        case Level::OK:   return "ok";
        case Level::WARN: return "warn";
        case Level::FAIL: return "fail";
    }
    return "fail";
}

bool Report::can_run() const {
    return std::none_of(checks.begin(), checks.end(),
                        [](const Check& c) { return c.level == Level::FAIL; });
}

namespace {

/// 把 http://host:port/path 拆成 httplib 要的两段。
///
/// httplib 的 Client 构造函数吃 "scheme://host:port"，路径要单独传给 Get。
/// LLM 的地址通常带 /v1 后缀，不拆开会把它拼到 host 里去。
struct SplitUrl {
    std::string origin;  ///< http://127.0.0.1:11434
    std::string prefix;  ///< /v1，可能为空
    bool ok = false;
};

SplitUrl split_url(const std::string& url) {
    SplitUrl r;
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return r;
    size_t host_start = scheme_end + 3;
    size_t slash = url.find('/', host_start);
    if (slash == std::string::npos) {
        r.origin = url;
        r.prefix = "";
    } else {
        r.origin = url.substr(0, slash);
        r.prefix = url.substr(slash);
        while (!r.prefix.empty() && r.prefix.back() == '/') r.prefix.pop_back();
    }
    r.ok = true;
    return r;
}

/// 发一个 GET 并解析 JSON。任何一步失败都返回 nullopt——
/// 调用方只关心「拿没拿到」，不关心是连不上还是解析失败。
std::optional<json> get_json(const std::string& url, const std::string& path,
                             int timeout_s,
                             const httplib::Headers& headers = {}) {
    SplitUrl s = split_url(url);
    if (!s.ok) return std::nullopt;
    // 阶段 0 未启用 OpenSSL，https 地址连不上。体检里这会表现为
    // 「连不上」，与真的连不上无法区分。接云端 LLM 时必须打开。
    if (s.origin.rfind("https://", 0) == 0) return std::nullopt;

    httplib::Client cli(s.origin);
    cli.set_connection_timeout(timeout_s, 0);
    cli.set_read_timeout(timeout_s, 0);
    auto res = cli.Get(s.prefix + path, headers);
    if (!res || res->status < 200 || res->status >= 300) return std::nullopt;
    return json::parse(res->body, nullptr, false).is_discarded()
               ? std::nullopt
               : std::optional<json>(json::parse(res->body));
}

Check check_runtime() {
    std::ostringstream os;
#if defined(_WIN32)
    os << "Windows";
#elif defined(__APPLE__)
    os << "macOS";
#elif defined(__linux__)
    os << "Linux";
#else
    os << "未知平台";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    os << " arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    os << " x86_64";
#endif
    os << "  C++" << (__cplusplus / 100 % 100);
    return {"运行时", Level::OK, os.str(), ""};
}

Check check_ffmpeg(const config::Settings& s) {
    auto found = proc::which(s.assembly.ffmpeg_path);
    if (found) return {"FFmpeg", Level::OK, *found, ""};
    return {"FFmpeg", Level::FAIL, "未找到",
            "装配环节的硬依赖。\n"
            "Windows: winget install Gyan.FFmpeg\n"
            "macOS:   brew install ffmpeg\n"
            "Debian:  sudo apt install ffmpeg\n"
            "装好后仍找不到就在配置里填 assembly.ffmpeg_path"};
}

Check check_fonts(const config::Settings& s) {
#if defined(_WIN32) || defined(__APPLE__)
    (void)s;
    return {"中文字体", Level::OK, "系统自带", ""};
#else
    if (!proc::which("fc-list")) {
        return {"中文字体", Level::WARN, "无法检测", ""};
    }
    auto r = proc::run("fc-list", {});
    std::string low = r.out;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const char* k : {"noto sans cjk", "source han", "wqy", "pingfang"}) {
        if (low.find(k) != std::string::npos) {
            return {"中文字体", Level::OK, "已安装", ""};
        }
    }
    return {"中文字体", Level::WARN, "没找到 " + s.assembly.subtitle_font,
            "中文字幕会渲染成方框。\nDebian: sudo apt install fonts-noto-cjk"};
#endif
}

Check check_infer(const config::Settings& s) {
    const std::string& url = s.comfy.base_url;
    auto body = get_json(url, "/system_stats", 8);
    if (!body) {
        return {"推理服务", Level::FAIL, "连不上 " + url,
                "出图和出视频都要用它。\n"
                "用 Docker: docker compose up -d comfyui\n"
                "已经装在别处就改配置：\n"
                "  export CHANGJI_COMFY_BASE_URL=http://某台机器:8188"};
    }
    if (body->contains("devices") && (*body)["devices"].is_array()
        && !(*body)["devices"].empty()) {
        const auto& d = (*body)["devices"][0];
        double vram = d.value("vram_total", 0.0) / (1024.0 * 1024.0 * 1024.0);
        std::ostringstream os;
        os << url << "  " << d.value("name", std::string{}) << "  ";
        os.setf(std::ios::fixed);
        os.precision(1);
        os << vram << " GB";
        return {"推理服务", Level::OK, os.str(), ""};
    }
    return {"推理服务", Level::WARN, url + " 已连接但没有报告显卡",
            "可能在用 CPU 推理，会非常慢"};
}

Check check_tts(const config::Settings& s, bool infer_ok) {
    if (s.tts.backend == "http") {
        if (!s.tts.base_url || s.tts.base_url->empty()) {
            return {"配音", Level::FAIL, "配了 http 后端但没填地址",
                    "在配置里填 tts.base_url"};
        }
        return {"配音", Level::OK, "独立服务 " + *s.tts.base_url, ""};
    }
    if (!infer_ok) {
        return {"配音", Level::WARN, "推理服务连不上，无法判断",
                "先解决推理服务的连接问题"};
    }
    auto body = get_json(s.comfy.base_url, "/object_info", 30);
    if (!body || !body->is_object()) {
        return {"配音", Level::WARN, "查不到推理服务的节点清单", ""};
    }
    std::vector<std::string> engines;
    bool has_tts = false;
    for (auto it = body->begin(); it != body->end(); ++it) {
        const std::string& n = it.key();
        if (n == "UnifiedTTSTextNode") has_tts = true;
        if (n.size() > 10 && n.compare(n.size() - 10, 10, "EngineNode") == 0) {
            engines.push_back(n.substr(0, n.size() - 10));
        }
    }
    if (has_tts) {
        std::sort(engines.begin(), engines.end());
        std::string list;
        for (size_t i = 0; i < engines.size() && i < 5; ++i) {
            if (i) list += "、";
            list += engines[i];
        }
        return {"配音", Level::OK, "配音节点可用，引擎：" + list, ""};
    }
    return {"配音", Level::WARN, "推理服务上没有本地配音节点",
            "成片会是静音。本地方案要装节点包：\n"
            "  cd ComfyUI/custom_nodes\n"
            "  git clone https://github.com/diodiogod/TTS-Audio-Suite.git\n"
            "  cd TTS-Audio-Suite && python install.py\n"
            "装完重启。用 Docker 的话直接重新 build 更稳，\n"
            "手动装的依赖在容器重建时会丢。"};
}

Check check_llm(const config::Settings& s) {
    const std::string& url = s.llm.base_url;
    const std::string& model = s.llm.model;
    httplib::Headers h{{"Authorization", "Bearer " + s.llm.api_key}};
    auto body = get_json(url, "/models", 8, h);
    if (!body) {
        return {"大模型", Level::WARN, "连不上 " + url,
                "剧本和分镜要用它。没有它也能手写分镜表。\n"
                "用 Docker: docker compose up -d ollama\n"
                "本机装了 Ollama 就确认它已启动"};
    }
    std::vector<std::string> names;
    if (body->contains("data") && (*body)["data"].is_array()) {
        for (const auto& m : (*body)["data"]) {
            names.push_back(m.value("id", std::string{}));
        }
    }
    std::string family = model.substr(0, model.find(':'));
    for (const auto& n : names) {
        if (n == model || n.rfind(family, 0) == 0) {
            return {"大模型", Level::OK, url + "  " + model, ""};
        }
    }
    if (!names.empty()) {
        // 服务在跑，只是没有配置里指定的那个模型。
        // 这不是错误，用现有的任何一个都能出分镜。
        std::string list;
        for (size_t i = 0; i < names.size() && i < 5; ++i) {
            if (i) list += "、";
            list += names[i];
        }
        return {"大模型", Level::WARN, url + " 上没有 " + model,
                "服务正常，现有模型：" + list + "\n"
                "二选一：\n"
                "  用现有的：export CHANGJI_LLM_MODEL=" + names[0] + "\n"
                "  或拉取指定的：ollama pull " + model};
    }
    return {"大模型", Level::WARN, url + " 一个模型都没有",
            "拉取一个：ollama pull " + model};
}

Check check_gpu(const config::Settings& s) {
    if (proc::which("nvidia-smi")) {
        auto r = proc::run("nvidia-smi",
                           {"--query-gpu=name,memory.total",
                            "--format=csv,noheader,nounits"});
        if (r.launched && !r.out.empty()) {
            std::string first = r.out.substr(0, r.out.find('\n'));
            size_t comma = first.find(',');
            if (comma != std::string::npos) {
                std::string name = first.substr(0, comma);
                double mb = 0;
                try { mb = std::stod(first.substr(comma + 1)); } catch (...) {}
                std::ostringstream os;
                os << name << "  ";
                os.setf(std::ios::fixed);
                os.precision(1);
                os << (mb / 1024.0) << " GB";
                return {"显卡", Level::OK, os.str(), ""};
            }
        }
    }
    if (s.vram_gb_override) {
        std::ostringstream os;
        os << "按配置的 " << static_cast<int>(*s.vram_gb_override) << " GB 推导档位";
        return {"显卡", Level::OK, os.str(), ""};
    }
    return {"显卡", Level::WARN, "本机未探测到，按 12 GB 估算",
            "推理服务如果在别的机器上，这是正常的。\n"
            "为了让画质档位推导正确，在配置里填 vram_gb_override"};
}

Check check_workspace(const config::Settings& s) {
    fs::path path = s.workspace_path();
    std::error_code ec;
    fs::create_directories(path, ec);
    fs::path probe = path / ".changji_write_test";
    {
        std::ofstream out(probe, std::ios::binary);
        if (!out) {
            return {"项目目录", Level::FAIL, paths::to_utf8(path) + " 不可写",
                    ec ? ec.message() : "创建测试文件失败"};
        }
        out << "ok";
    }
    fs::remove(probe, ec);
    return {"项目目录", Level::OK, paths::to_utf8(path), ""};
}

/// 本地模型文件。
///
/// 一项都没配是 OK 不是 WARN——走 ComfyUI 那条路的用户根本不需要这一节，
/// 给他们报警告等于让体检报告长期挂着一条永远不会去处理的黄字，
/// 报告里有常驻噪音之后，真正的警告就没人看了。
///
/// 配了但文件不在才报警告。这种情况几乎一定是笔误或者模型没下完。
Check check_models(const config::Settings& s) {
    const fs::path ws = s.workspace_path();
    const auto& m = s.models;

    const std::vector<std::pair<const char*, const std::string*>> entries = {
        {"llm", &m.llm},
        {"video", &m.video},
        {"video_vae", &m.video_vae},
        {"video_text_encoder", &m.video_text_encoder},
        {"image", &m.image},
    };

    std::vector<std::string> configured, missing;
    for (const auto& [key, val] : entries) {
        if (val->empty()) continue;
        configured.push_back(key);
        std::error_code ec;
        const fs::path p = m.resolve(*val, ws);
        if (!fs::is_regular_file(p, ec)) {
            missing.push_back(std::string(key) + " -> " + paths::to_utf8(p));
        }
    }

    if (configured.empty()) {
        return {"本地模型", Level::OK, "没配，走推理服务", ""};
    }

    if (!missing.empty()) {
        std::string detail = "配了 " + std::to_string(configured.size()) +
                             " 项，其中 " + std::to_string(missing.size()) +
                             " 项的文件不存在：";
        for (const auto& x : missing) detail += "\n  " + x;
        return {"本地模型", Level::WARN, detail,
                "检查 [models] 里的文件名，以及 dir 指的目录对不对。\n"
                "相对路径是相对 dir 解析的，dir 留空时是项目库下的 models/。\n"
                "当前 dir：" + paths::to_utf8(m.dir_path(ws))};
    }

    return {"本地模型", Level::OK,
            std::to_string(configured.size()) + " 个模型文件都在（" +
                paths::to_utf8(m.dir_path(ws)) + "）",
            ""};
}

}  // namespace

namespace {

/// 跑一项检查，异常不外泄。
///
/// 体检的意义就是"环境不对时告诉你哪里不对"，所以它自己最不能因为环境不对
/// 而崩掉。实测踩到过：某一项抛了 std::system_error，异常一路穿到
/// std::terminate，进程以 0xC0000409 消失、一个字不打印，而那个错误码
/// 字面意思是"栈缓冲区溢出"，排查方向完全被带偏。
///
/// 现在单项失败降级成一条 WARN，其余检查照跑。
template <typename F>
Check guarded(const char* name, F&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        return {name, Level::WARN, std::string("这一项检查自己出错了：") + e.what(),
                "这是 changji 的问题不是你的环境问题，请把这条报上来。\n"
                "其余检查不受影响。"};
    } catch (...) {
        return {name, Level::WARN, "这一项检查自己抛了未知异常",
                "这是 changji 的问题不是你的环境问题，请把这条报上来。"};
    }
}

}  // namespace

Report run_checks(const config::Settings& settings) {
    Report r;
    r.checks.push_back(guarded("运行时", [&] { return check_runtime(); }));
    r.checks.push_back(guarded("FFmpeg", [&] { return check_ffmpeg(settings); }));
    r.checks.push_back(guarded("中文字体", [&] { return check_fonts(settings); }));

    Check infer = guarded("推理服务", [&] { return check_infer(settings); });
    bool infer_ok = infer.level == Level::OK;
    r.checks.push_back(std::move(infer));

    r.checks.push_back(guarded("配音", [&] { return check_tts(settings, infer_ok); }));
    r.checks.push_back(guarded("大模型", [&] { return check_llm(settings); }));
    r.checks.push_back(guarded("本地模型", [&] { return check_models(settings); }));
    r.checks.push_back(guarded("显卡", [&] { return check_gpu(settings); }));
    r.checks.push_back(guarded("项目目录", [&] { return check_workspace(settings); }));
    return r;
}

}  // namespace changji::doctor
