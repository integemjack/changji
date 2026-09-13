#include "http/llm_info.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "http/llm_providers.inc.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::http {

namespace {

/// 目录里的 gguf 文件，按名字排序。
///
/// 只扫一层，不递归。模型文件几个 G，用户不会把它们塞进深层目录树；
/// 而递归扫一个指错的目录（比如整个 C 盘）会卡住这个请求几十秒。
std::vector<std::string> list_gguf(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const std::string name = paths::to_utf8(e.path().filename());
        // 大小写都收：Windows 上文件名大小写不敏感，用户可能存成 .GGUF
        std::string lower = name;
        for (char& c : lower) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
        }
        if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".gguf") == 0) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// 我们认识的这家的模型，每个配一句**选它还是不选它**的话。
///
/// 两件事让这份清单非有不可，都是 2026-09-13 实测撞出来的：
///
///   1. **智谱的 `/models` 不列免费模型。** z.ai 和 bigmodel.cn 都只回
///      glm-4.5 / 4.5-air / 4.6 / 4.7 / 5 / 5-turbo / 5.1 / 5.2 / 5.3 /
///      5.3-flash——**glm-4.7-flash 不在里面，而它能用**（服务端回的
///      `model` 字段就是它），它还正好是我们的默认。只照 `/models` 渲染
///      下拉的话，默认那个模型在自己的下拉里是找不到的。
///   2. **光有名字选不动。** 一串 glm-4.5/4.6/4.7/5/5.1/5.2/5.3 摆在那儿，
///      谁也不知道该点哪个。要紧的两件事——**哪个不要钱、哪个会写**——
///      名字上一个字都看不出来。
///
/// 分数是 EQ-Bench 长文创作榜（2026-09-14 抓的，见
/// config::LLMConfig::task_models）。**不写具体单价**：那些数我只在
/// 转售的网关上核过，各家官网价会变，写进界面就是在替服务商报价。
///
/// 认不出的地址回空数组——这是本**我们自己维护的小抄**，不是模型总表，
/// 真实能用什么以 `/models` 拉回来的为准。
std::vector<std::pair<std::string, std::string>> known_models(
    const std::string& base_url) {
    const bool zhipu = base_url.find("bigmodel.cn") != std::string::npos ||
                       base_url.find("z.ai") != std::string::npos;
    if (!zhipu) return {};
    return {
        {"glm-4.7-flash",
         "免费 · 默认。限流很紧，成批写会慢；写作榜 47.8"},
        {"glm-5.3", "这一档最会写：写作榜 81.8、套话 7.09，八章几乎不降"},
        {"glm-5.3-flash", "便宜档。⚠️ 写作榜上没测过，别照 5.3 的分想当然"},
        {"glm-5.2", "写作榜 77.9"},
        {"glm-5", "写作榜 70.9"},
        {"glm-4.7", "写作榜 66.0"},
        {"glm-4.6", "写作榜 57.3"},
        {"glm-4.5", "写作榜 55.5"},
    };
}

}  // namespace

ApiResult get_llm_providers() {
    static const json providers = json::parse(stages::prompt::kLlmProvidersJson);
    return {200, {{"providers", providers}}};
}

ApiResult get_llm_models(const config::Settings& settings, const HttpGet& fetch) {
    const std::string url = settings.llm.base_url + "/models";
    const std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + settings.llm.api_key}};

    // 本机的 gguf 先列出来。这一段和远端问不问得通无关——
    // 用户可能压根没配远端服务，那时候这个列表就是他唯一能选的东西。
    const fs::path dir = settings.models.dir_path(settings.workspace_path());
    json local = {
        {"dir", paths::to_utf8(dir)},
        {"files", list_gguf(dir)},
        {"current", settings.models.llm},
    };

    // 我们认识的这家有什么，见 known_models。**四条返回路径都带上它**，
    // 尤其是失败那三条：刚装好还没填密钥时 `/models` 必然 401，而那正是
    // 用户最需要「这家都有什么、该挑哪个」的时候。
    //
    // 它和 `models` **是两个字段，不合并**。`models` 是这台服务此刻真答
    // 应的东西，前端那句「这台服务上没有 X」靠它判；混进我们的小抄之后
    // 那句话就会在模型真的不存在时也不吭声。合并交给前端去做。
    json known = json::array();
    for (const auto& [id, note] : known_models(settings.llm.base_url)) {
        known.push_back({{"id", id}, {"note", note}});
    }

    // 超时写死 10 秒，不用 llm.timeout_s。那个是给生成用的，默认 300 秒——
    // 拿它来问一个列表，服务不在的时候设置页会转五分钟圈。
    const llm::HttpResponse r = fetch(url, headers, 10.0);

    // 失败的三种情况都返回空列表加一句原因，界面退回手打。
    // **不抛异常**：列不出来不该让整个设置页打不开。
    if (r.transport_error.has_value()) {
        return {200, {{"models", json::array()},
                      {"error", "连不上 " + url + "：" + *r.transport_error},
                      {"local", local},
                      {"known", known}}};
    }
    if (r.status >= 400) {
        return {200, {{"models", json::array()},
                      {"error", url + " 返回 " + std::to_string(r.status)},
                      {"local", local},
                      {"known", known}}};
    }
    const json body = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) {
        return {200, {{"models", json::array()},
                      {"error", url + " 返回的不是 JSON"},
                      {"local", local},
                      {"known", known}}};
    }

    // OpenAI 兼容接口回 {"data": [{"id": ...}]}，也有直接回数组的
    const json items = (body.is_object() && body.contains("data"))
                           ? body["data"]
                           : body;
    std::set<std::string> names;   // set 顺带做了去重和排序
    if (items.is_array()) {
        for (const auto& item : items) {
            std::string name;
            if (item.is_object()) {
                const auto it = item.find("id");
                if (it != item.end() && !it->is_null()) {
                    name = it->is_string() ? it->get<std::string>() : it->dump();
                }
            } else if (item.is_string()) {
                name = item.get<std::string>();
            } else if (!item.is_null()) {
                name = item.dump();
            }
            if (!name.empty()) names.insert(name);
        }
    }

    // 成功时**没有 error 键**，失败时没有 current 键。两种形状不一样，
    // 照抄 Python。前端两个都用 ?? 兜着，但形状是契约。
    return {200, {
        {"models", std::vector<std::string>(names.begin(), names.end())},
        {"current", settings.llm.model},
        {"local", local},
        {"known", known},
    }};
}

}  // namespace changji::http
