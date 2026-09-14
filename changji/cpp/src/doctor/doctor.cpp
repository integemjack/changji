#include "doctor/doctor.hpp"



#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "util/httplib.hpp"
#include <nlohmann/json.hpp>

#include "infer/ggml_abi.hpp"
#include "infer/llama_tts.hpp"
// 画布上限跟着模型走
#include "stages/limits.hpp"
#include "models/hardware.hpp"
#include "llm/client.hpp"
#include "infer/sd_backend.hpp"
#include "util/paths.hpp"
#include "util/proc.hpp"

namespace changji::doctor {

namespace fs = std::filesystem;
using json = nlohmann::json;

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

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    // 这一版没编进 OpenSSL，httplib 发不了 https。
    //
    // **原来这一句是无条件的**，注释写着"阶段 0 未启用 OpenSSL"——而
    // CHANGJI_SSL 早就默认 ON 了。2026-09-13 大模型的默认地址改成云端
    // （https）之后，这句无条件的 return 会让体检对一个**完全正常**的
    // 配置一律报"连不上"。加上这道 #ifndef 之后，编进了 SSL 的版本
    // 照常去连，没编进去的版本才走这条早退。
    if (s.origin.rfind("https://", 0) == 0) return std::nullopt;
#endif

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

Check check_tts(const config::Settings& s) {
    if (s.tts.backend == "http") {
        if (!s.tts.base_url || s.tts.base_url->empty()) {
            // **两条路都说。** 这段话既给 `changji --doctor`（那儿只能改
            // 配置），也给网页上的体检——而网页那一页往下翻两节就是「配音」
            // 的「服务地址」输入框。只说"去配置里填"的人正对着那个框。
            return {"配音", Level::FAIL, "配了 http 后端但没填地址",
                    "界面上：设置页「配音」那一节填「服务地址」。\n"
                    "命令行：配置里填 tts.base_url。"};
        }
        return {"配音", Level::OK, "独立服务 " + *s.tts.base_url, ""};
    }
    // 细节由「进程内配音」那一项报（模型在不在、编没编进来），
    // 这里只说清这条路归谁管。
    if (s.tts.backend == "local") {
        return {"配音", Level::OK, "走进程内（详见「进程内配音」那一项）", ""};
    }
    // 走到这里就只剩 http 那条，而它上面已经答过了。
    // **ComfyUI 那条 2026-09-10 拆了**，原来这里会去问它的节点清单，
    // 判断装没装 TTS 节点包。
    return {"配音", Level::WARN, "配音后端认不出：" + s.tts.backend,
            "只能是 local（进程内）或 http（外部服务）"};
}

Check check_llm(const config::Settings& s) {
    // **进程内那条 2026-09-14 删了。** 配着 backend = "local" 的老机器
    // 落到这儿不该静默走远端体检——那样报的是一个他没打算起的服务连不上。
    if (s.llm.backend == "local") {
        return {"大模型", Level::FAIL,
                "配的是进程内跑，而这条路已经没有了",
                "配置里把 [llm].backend 改成 remote。\n"
                // 项目页那一行上没有「大模型」这三个字：四个按钮各写着
                // 模型名，鼠标停上去才是组名，而编剧那一组叫「编剧模型」
                // （catalog.cpp 的 g.title）。同一个检查里另外两条分支早就
                // 改成说「编剧模型的名字」了，这条漏了。
                "地址和密钥两条路都能填——界面上是项目页「模型」那一行点一下\n"
                "编剧模型的名字，命令行是 [llm].base_url 和 api_key。\n"
                "默认走智谱（bigmodel.cn），glm-4.7-flash 不要钱。"};
    }

    const std::string& url = s.llm.base_url;
    const std::string& model = s.llm.model;

    // **密钥没填就别去连。** 刚装好的机器就是这个状态（默认是远端的
    // glm-4.7-flash，密钥要用户自己去领）。不分这一支的话，401 会被
    // get_json 当成失败，报出来是"连不上 https://api.z.ai/…"
    // 外加一句"用 Docker 起 ollama"——三样东西全指错方向，而这正是
    // 上面那段注释说的"报告说错了比不说更糟"。
    if (s.llm.needs_api_key() && s.llm.api_key.empty()) {
        return {"大模型", Level::WARN, "还没填 API Key（" + url + "）",
                "去项目页「模型」那一行点一下编剧模型的名字，在弹出来的窗口里填。\n"
                "默认走智谱：去 bigmodel.cn 控制台领一把，默认挑的 glm-4.7-flash 本身不要钱。\n"
                "只有远端这一条路了。"};
    }

    httplib::Headers h{{"Authorization", "Bearer " + s.llm.api_key}};
    auto body = get_json(url, "/models", 8, h);
    if (!body) {
        // 本机服务和云服务该做的事不一样，一句话糊过去会把人支错方向。
        return {"大模型", Level::WARN, "连不上 " + url,
                "剧本和分镜要用它。没有它也能手写分镜表。\n" +
                    (s.llm.needs_api_key()
                         // **不是设置页。** 大模型那一节 2026-09-14 从设置页
                         // 整个搬走了（地址、模型名、密钥、温度都在项目页
                         // 那个弹窗里），这句话还在把人往一个没有这些框的
                         // 页面送——而同一个检查上面那条分支（缺 api_key）
                         // 早就改成指项目页了，两条自相矛盾。
                         ? std::string("地址和密钥在项目页「模型」那一行点一下"
                                       "编剧模型的名字，在弹出来的窗口里核一眼；"
                                       "国内直连不通的服务要自备网络。")
                         : std::string("用 Docker: docker compose up -d ollama\n"
                                       "本机装了 Ollama 就确认它已启动"))};
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
    // **在我们那本小抄上的，不算"没有"。** 见 llm::known_models：
    // 智谱的 /models 只列收费那几个，而默认那个 glm-4.7-flash 正是
    // 免费的、不在列表里、却能用——照 names 判的话，一台配置完全正确
    // 的机器每次体检都要挨这一句，而"报告说错了比不说更糟"。
    for (const auto& [id, note] : llm::known_models(url)) {
        if (id != model) continue;
        return {"大模型", Level::OK, url + "  " + model,
                "这家的 /models 没把它列出来（免费档常这样），但它是能用的。\n" + note};
    }

    if (!names.empty()) {
        // 服务在跑，只是这份清单里没有配置指定的那个。
        //
        // **不能一口咬定"没有这个模型"。** 2026-09-13 实测：智谱的 /models
        // 只列 glm-4.5 ~ glm-5.3-flash 这些收费的，**免费的 glm-4.7-flash
        // 根本不在里面，而它是能用的**（发过去回 200，服务端回的 model 字段
        // 就是 glm-4.7-flash）。照老话术报的话，一台配置完全正确的机器会被
        // 告知"上面没有这个模型"，还附一句 ollama pull——而这是个云服务。
        std::string list;
        for (size_t i = 0; i < names.size() && i < 5; ++i) {
            if (i) list += "、";
            list += names[i];
        }
        if (names.size() > 5) list += " 等 " + std::to_string(names.size()) + " 个";
        return {"大模型", Level::WARN, model + " 不在 " + url + " 的清单里",
                "有些平台的 /models 不列免费模型（智谱就是），那样的话这条可以"
                "不管——写剧本时真调得通就行。\n"
                "清单上有的：" + list + "\n"
                "确实写错了的话：去项目页那个模型窗口里改，或者 "
                "export CHANGJI_LLM_MODEL=" + names[0]};
    }
    // **云服务和本机服务该做的事不一样**，一句话糊过去会把人支错方向——
    // 上面「连不上」那条早就按 `needs_api_key()` 分了两支，这条漏了：
    // 对着智谱（或者任何一个 /models 回空清单的云服务）说一句
    // 「ollama pull glm-4.7-flash」，照着做只会得到一句找不到命令。
    if (s.llm.needs_api_key()) {
        return {"大模型", Level::WARN, url + " 的模型清单是空的",
                "有些平台的 /models 本来就不列（智谱免费档就这样），那样的话\n"
                "这条可以不管——写剧本时真调得通就行。\n"
                "真连错了地址的话：项目页「模型」那一行点一下编剧模型的名字，\n"
                "在弹出来的窗口里核一眼。"};
    }
    return {"大模型", Level::WARN, url + " 一个模型都没有",
            "拉取一个：ollama pull " + model};
}

Check check_gpu(const config::Settings& s) {
    // **走 models::detect_gpu()，不再自己问一遍 nvidia-smi。**
    //
    // 这里原来是一份独立的探测：直接 `nvidia-smi --query-gpu=...`。
    // 于是探测逻辑有了两份，而它们会分岔——2026-09-11 在 Mac 上就分岔了：
    // detect_gpu() 已经会按统一内存算苹果芯片的显存了，doctor 却还只认
    // nvidia-smi，于是一台 Mac 上「显卡」那行永远是"本机未探测到"，
    // 而同一个进程里的档位推导用的是另一个数。
    //
    // 现在只有一份。加一种新硬件只要改 detect_gpu()，体检跟着就对。
    if (const auto gpu = models::detect_gpu(); gpu.has_value()) {
        std::ostringstream os;
        os << gpu->name << "  ";
        os.setf(std::ios::fixed);
        os.precision(1);
        os << gpu->vram_gb() << " GB";
        // **统一内存上这两个数都要写出来。**
        // 只写"107.5 GB"，用户看到的是"我买的明明是 128"；只写 128，
        // 预算又会按 128 算，而超过 Metal 那条线系统就开始压缩换页。
        // 说清楚哪个是哪个，比选一个显示省事得多。
        if (gpu->unified()) {
            os << "（整机 " << static_cast<double>(gpu->unified_mb) / 1024.0
               << " GB，其余留给系统）";
        }
        if (gpu->count > 1) {
            os << "（共 " << gpu->count << " 张，显存是单卡的）";
        }
        return {"显卡", Level::OK, os.str(), ""};
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

/// 进程内出图后端。
///
/// 没链的话不是错误，是一种部署形态：走 ComfyUI 那条路的用户不需要它，
/// 交叉编译到某些平台时也可能关掉。所以是 OK 加一句说明，不是 WARN——
/// 报告里挂一条永远不会去处理的黄字，会让真正的警告没人看。
/// ggml 的 ABI 自检。
///
/// 这一项和别的体检项不一样：**它查的不是环境，是这个二进制自己编得对不对。**
/// 放进体检报告是因为它防的那类问题没有别的信号——编译过、链接过、
/// 起得来，只有读写张量时慢慢踩坏内存。详见 infer/ggml_abi.hpp。
Check check_ggml() {
    if (!infer::ggml_available()) {
        return {"ggml ABI", Level::OK, "没链 ggml（出图和配音都走外部服务）", ""};
    }
    const auto abi = infer::check_ggml_abi();
    if (abi.ok) return {"ggml ABI", Level::OK, abi.detail, ""};
    // FAIL 不是 WARN：结构体大小对不上之后，这个进程做的任何推理
    // 都不值得相信，继续跑只会把损坏推到更远的地方。
    return {"ggml ABI", Level::FAIL, abi.detail,
            "多半是构建脚本改动引起的。顶层要有 "
            "add_compile_definitions(GGML_MAX_NAME=160)，"
            "而且 ggml 的头和库必须来自同一份源码树。"};
}

/// 进程内配音编进来了没有。
///
/// **不是 WARN 也不是 FAIL。** 没编进来是完全正常的形态——配音走独立
/// HTTP 服务是相当长一段时间的实际形态（ComfyUI 那条 2026-09-10 拆了）。
/// 报警告等于让报告长期挂一条永远不会去处理的黄字。
Check check_local_tts(const config::Settings& settings) {
    const auto probe = infer::probe_llama_tts();
    if (!probe.ok) return {"进程内配音", Level::WARN, probe.detail, ""};
    if (!infer::llama_tts_available()) {
        return {"进程内配音", Level::OK, probe.detail, ""};
    }

    // 编进来了，接着查配置。**只有 backend 真选了 local 才判警告**——
    // 编进来但不用它是完全正常的形态。
    const bool selected = settings.tts.backend == "local";
    const auto ws = settings.workspace_path();
    const auto backbone = settings.models.resolve(settings.models.tts, ws);
    const auto decoder =
        settings.models.resolve(settings.models.tts_decoder, ws);

    std::error_code ec;
    const bool has_b =
        !backbone.empty() && std::filesystem::is_regular_file(backbone, ec);
    const bool has_d =
        !decoder.empty() && std::filesystem::is_regular_file(decoder, ec);

    if (has_b && has_d) {
        return {"进程内配音", Level::OK,
                probe.detail + "；两份模型都在" +
                    (selected ? "，[tts].backend = local" : "（当前没选它）"),
                ""};
    }
    // 缺哪一份要分别点名：只填一个是最常见的配错法。
    const std::string missing =
        std::string(has_b ? "" : "[models].tts ") + (has_d ? "" : "[models].tts_decoder");
    return {"进程内配音", selected ? Level::FAIL : Level::OK,
            probe.detail + "；缺模型：" + missing +
                (selected ? "，配音会退回估算后端（出静音）" : "（当前没选它）"),
            // **两条路都要说。** 只说"下模型"的话，小卡上的用户下完才
            // 发现配音和出片挤不进同一张卡——而外接一个配音服务不用改
            // 一行代码、也不占本机显存，那多半才是他要的那条。
            selected ? "两条路挑一条：\n"
                       "  本机跑：填 [models].tts（Qwen3-TTS 的 talker）和 "
                       "[models].tts_decoder（tokenizer/解码器），两份都是 "
                       "GGUF。\n"
                       "  外接：把上面的「后端」改成「独立 HTTP 服务」并填"
                       "服务地址，本机就不用装配音模型、也不占显存。"
                     : ""};
}

Check check_sd() {
    if (!infer::sd_available()) {
        // **拆掉 ComfyUI 之后这就不再是 OK 了。** 原来的话是"出图走推理
        // 服务"——那个服务没了，没编进 sd.cpp 就是一张图都出不来。
        return {"出图后端", Level::FAIL, "没编进来，出图出片都跑不了",
                "这份二进制构建时 CHANGJI_SD=OFF。换一份编了的，"
                "或者自己编时打开 CHANGJI_SD，再按显卡挑一个 GPU 后端："
                "N 卡 CHANGJI_SD_CUDA、A 卡 CHANGJI_SD_HIP、"
                "Intel CHANGJI_SD_SYCL，三家都能用的 CHANGJI_SD_VULKAN。"
                "一个都不开就只能用 CPU 跑，一镜要几小时。"};
    }
    // 系统信息里带着编进去的后端和 CPU 特性（AVX2、CUDA 之类）。
    // 这一行是出画质问题时第一个要看的东西：同一份模型在 AVX2 和
    // 纯标量上出的图不一样，而用户不会想到去问"你编的时候开了什么"。
    std::string detail = "sd.cpp " + infer::sd_version();
    const std::string info = infer::sd_system_info();
    if (!info.empty()) detail += "\n" + info;
    return {"出图后端", Level::OK, detail, ""};
}

/// 本地模型文件。
///
/// **一项都没配现在是 WARN。** 以前是 OK，理由是"走 ComfyUI 那条路的用户
/// 根本不需要这一节"——那条路 2026-09-10 拆了。现在出图出片全在进程内，
/// 一项都没配就等于什么都出不来，报绿是在骗人。
///
/// 配了但文件不在也报警告。这种情况几乎一定是笔误或者模型没下完。
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
        // 是 WARN 不是 FAIL：装好程序还没下模型是常态，那时候用户仍然
        // 要能进界面、能写剧本分镜。FAIL 会把开工按钮一起锁掉。
        return {"本地模型", Level::WARN, "一个都没配，出图出片跑不了",
                "至少要 [models].image（首帧）和 [models].video + "
                "video_vae（视频）。\n"
                "相对路径是相对 dir 解析的，dir 留空时是项目库下的 models/。\n"
                "当前 dir：" + paths::to_utf8(m.dir_path(ws))};
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

/// 权重放哪这件事，配置写死的值和这台机器对不对得上。
///
/// **这一项是给"配置跟着人换了机器"准备的。** 2026-09-11 实测撞到：
/// 一份为 96 GB 卡写的 `[models].weights = "cpu"` 跟着配置文件到了一张
/// 32 GB 卡上，而那时模型早换成 Q4 了。后果不报错——sd.cpp 老老实实照做，
/// 把 42 GB 权重全放内存（日志里是 `VRAM 0.00MB`），每一步靠 PCIe 搬，
/// 同一台机器上出首帧那条（没写死，走 smart）却是扩散常驻显存、GPU 99%。
///
/// 程序拦不住人写死，但能说一句"这个值和这台机器算出来的不一样"。
/// **只在真的不一样时才说**：写死成和 smart 一致的值是没问题的，
/// 长期挂一条没用的黄字比不说更糟。
Check check_weights(const config::Settings& s) {
    const auto profile = models::HardwareProfile::detect(s.vram_gb_override);
    const double card = profile.gpu.has_value() ? profile.gpu->vram_gb()
                                                : profile.vram_gb;
    if (card <= 0) {
        return {"权重放哪", Level::OK, "没探到显卡，这一项无从比起", ""};
    }
    const auto ws = s.workspace_path();
    const auto size_gb = [&](const std::string& rel) {
        std::error_code ec;
        const auto p = s.models.resolve(rel, ws);
        if (p.empty()) return 0.0;
        const auto n = std::filesystem::file_size(p, ec);
        return (!ec && n > 0) ? static_cast<double>(n) / (1024.0 * 1024 * 1024)
                              : 0.0;
    };

    std::vector<std::string> off;
    const auto one = [&](const char* label, const std::string& set,
                         const std::string& want) {
        // smart 和 auto 本来就是"让程序/sd.cpp 自己定"，没有对不上这回事。
        if (set == "smart" || set == "auto") return;
        if (set == want) return;
        off.push_back(std::string(label) + "：配置写的是 \"" + set +
                      "\"，按这张卡和这个模型算出来该是 \"" + want + "\"");
    };
    // unified 要传下去，不然这一项在苹果机器上会一直报"对不上"——
    // 它算的是独显那套，而引擎跑的是统一内存那套。
    const bool unified = profile.gpu.has_value() && profile.gpu->unified();
    // 画布也要传：体检说的"该是什么"必须和真跑那条路算的是同一个数，
    // 否则选了 2K 之后这里会一直报"对不上"（或者反过来一直报"一致"而实际
    // 会 OOM）。
    const auto [cw, ch] = s.video.size();
    const double canvas_px = static_cast<double>(cw) * ch;
    one("出片", s.models.weights,
        s.models.weights_for(card, size_gb(s.models.video), unified, canvas_px));
    one("出首帧", s.models.image_weights,
        s.models.image_weights_for(card, size_gb(s.models.image), unified));

    if (off.empty()) {
        return {"权重放哪", Level::OK, "配置和这台机器算出来的一致", ""};
    }
    std::string msg;
    for (std::size_t i = 0; i < off.size(); ++i) {
        if (i) msg += "；";
        msg += off[i];
    }
    return {"权重放哪", Level::WARN, msg,
            "多半是配置从别的机器带过来的。写死的值不会跟着卡变——\n"
            "换成 \"smart\"（程序按卡和模型大小算）或者 \"auto\"\n"
            "（装载时让 sd.cpp 按实际显存自己塞）就不用管了。\n"
            "确实想按现在这样固定的话，这条忽略即可。"};
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

/// 出片的画布有没有超出这个模型能画的范围。
///
/// **超出去不是慢一点，是画面坏掉。** MiniMax-H3 的开源权重把画布钉在
/// canvas_max_pixels = 1032192（1344 × 768，官方 diffusers 的
/// MiniMaxH3Blocks 配置），短边 768；商业 API 主打的 2K（1440 短边）靠的是
/// 一个叫 H3-Regenerate-2K 的模块，**不在开源发布里**。我们的 2k 档
/// （1440 × 2560 = 368 万像素）是它的 3.57 倍，等于让模型在训练分布之外跑。
///
/// **只报警，不悄悄降档。** 人选了 2K 却拿到标准档而且没有提示，比出一条
/// 烂片更糟——config/settings.cpp 的 VideoConfig::size 注释里写过这一条，
/// 这里守着它。
Check check_canvas(const config::Settings& s) {
    const auto& limits = stages::video_limits();
    const auto [w, h] = s.video.size();
    const long px = static_cast<long>(w) * h;
    const std::string got = std::to_string(w) + "×" + std::to_string(h);

    if (limits.max_pixels <= 0) {
        return {"出片画布", Level::OK, got + "（这个模型没给画布上限）", ""};
    }
    if (px <= limits.max_pixels) {
        return {"出片画布", Level::OK, got, ""};
    }
    const double times = static_cast<double>(px) /
                         static_cast<double>(limits.max_pixels);
    char ratio[32];
    std::snprintf(ratio, sizeof(ratio), "%.2f", times);
    return {"出片画布", Level::WARN,
            got + " 超出这个模型的画布上限 " +
                std::to_string(limits.max_pixels) + " 像素（" + ratio + " 倍）",
            // **建议要能照着做。** 上一版只说"出完再跑 changji --upscale"，
            // 可这条命令还要一个 ESRGAN 权重，而那个文件**不在首次运行
            // 的下载清单里**（setup/catalog.cpp 一个超分条目都没有）——
            // 照着做的人会卡在"--upscale 还要 --upscale-model"这句上，
            // 而它没说该去下哪个文件。
            "模型在训练分布之外跑，出来多半是伪影，不是糊一点。\n"
            "把 [video].quality 调回 hd（704×1280）。要 2K 就先出标准档，"
            "再单独超分：\n"
            "  changji --upscale 成片.mp4 出来的.mp4 \\\n"
            "    --upscale-model <RealESRGAN_x4plus.pth 的路径> \\\n"
            "    --upscale-size 1440x2560\n"
            "那个权重要自己下（Real-ESRGAN 的 v0.1.0 release，67 MB），"
            "清单里没有。逐帧超分没有帧间一致性，做完要看片子。"};
}

Report run_checks(const config::Settings& settings) {
    Report r;
    r.checks.push_back(guarded("运行时", [&] { return check_runtime(); }));
    r.checks.push_back(guarded("FFmpeg", [&] { return check_ffmpeg(settings); }));
    r.checks.push_back(guarded("中文字体", [&] { return check_fonts(settings); }));

    // 「推理服务」那一项 2026-09-10 随 ComfyUI 一起去掉了：出图出片都在
    // 进程内，没有外部服务要连。出图那条由「出图后端」和「本地模型」两项管。
    r.checks.push_back(guarded("配音", [&] { return check_tts(settings); }));
    r.checks.push_back(guarded("大模型", [&] { return check_llm(settings); }));
    r.checks.push_back(guarded("ggml ABI", [&] { return check_ggml(); }));
    r.checks.push_back(guarded("进程内配音", [&] { return check_local_tts(settings); }));
    r.checks.push_back(guarded("出图后端", [&] { return check_sd(); }));
    r.checks.push_back(guarded("本地模型", [&] { return check_models(settings); }));
    r.checks.push_back(guarded("显卡", [&] { return check_gpu(settings); }));
    r.checks.push_back(guarded("权重放哪", [&] { return check_weights(settings); }));
    r.checks.push_back(guarded("出片画布", [&] { return check_canvas(settings); }));
    r.checks.push_back(guarded("项目目录", [&] { return check_workspace(settings); }));
    return r;
}

}  // namespace changji::doctor
