#include "http/llm_info.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
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

    // 超时写死 10 秒，不用 llm.timeout_s。那个是给生成用的，默认 300 秒——
    // 拿它来问一个列表，服务不在的时候设置页会转五分钟圈。
    const llm::HttpResponse r = fetch(url, headers, 10.0);

    // 失败的三种情况都返回空列表加一句原因，界面退回手打。
    // **不抛异常**：列不出来不该让整个设置页打不开。
    if (r.transport_error.has_value()) {
        return {200, {{"models", json::array()},
                      {"error", "连不上 " + url + "：" + *r.transport_error},
                      {"local", local}}};
    }
    if (r.status >= 400) {
        return {200, {{"models", json::array()},
                      {"error", url + " 返回 " + std::to_string(r.status)},
                      {"local", local}}};
    }
    const json body = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) {
        return {200, {{"models", json::array()},
                      {"error", url + " 返回的不是 JSON"},
                      {"local", local}}};
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
    }};
}

}  // namespace changji::http
