#include "infer/node_prefs.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "util/paths.hpp"

namespace changji::infer {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path node_prefs_path(const fs::path& workspace) {
    return workspace / "nodes.json";
}

NodePrefs load_node_prefs(const fs::path& workspace) {
    NodePrefs out;
    const auto p = node_prefs_path(workspace);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return out;

    std::ifstream in(p);
    if (!in) return out;
    std::ostringstream ss;
    ss << in.rdbuf();

    // 坏了就当没关任何东西，见头文件：默认值挑"不改变行为"的那边。
    const auto j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return out;
    const auto it = j.find("off");
    if (it == j.end() || !it->is_object()) return out;

    for (const auto& [url, caps] : it->items()) {
        if (!caps.is_array()) continue;
        std::set<Capability> set;
        for (const auto& c : caps) {
            if (!c.is_string()) continue;
            // 认不出的能力名跳过。**这儿不报错**：这份文件是程序自己写的，
            // 认不出多半是降级回了老版本，那时候忽略比拒绝启动好。
            if (const auto cap = capability_from(c.get<std::string>())) {
                set.insert(*cap);
            }
        }
        if (!set.empty()) out[url] = std::move(set);
    }
    return out;
}

void save_node_prefs(const fs::path& workspace, const NodePrefs& prefs) {
    json off = json::object();
    for (const auto& [url, caps] : prefs) {
        if (caps.empty()) continue;   // 空的就别留一行噪音
        json arr = json::array();
        for (const Capability c : caps) arr.push_back(to_string(c));
        off[url] = std::move(arr);
    }

    const auto p = node_prefs_path(workspace);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    // 先写临时文件再改名：写到一半断电留下半截 JSON 的话，下次读回来
    // 是"没关任何东西"——静悄悄地把用户的设置丢了。
    const auto tmp = p.parent_path() / "nodes.json.part";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            throw std::runtime_error("写不了 " + paths::to_utf8(tmp));
        }
        f << json{{"off", off}}.dump(2) << "\n";
        if (!f) {
            throw std::runtime_error("写坏了 " + paths::to_utf8(tmp));
        }
    }
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
    if (ec) {
        throw std::runtime_error("改名失败：" + paths::to_utf8(p));
    }
}

}  // namespace changji::infer
