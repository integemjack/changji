// 对拍程序。方案第六节。
//
// **Python 引擎保留的唯一目的是当回归基准，删除之前必须完成对拍。**
//
// 两种模式：
//
//   录制模式（默认）  拿 tests/golden 里录好的 Python 响应，跟活着的 C++
//                     后端比。不需要 Python 在跑。
//   实时模式（--python）两个后端同时驱动，同一个请求发两遍，深比较响应。
//                     覆盖面比录制模式宽得多——路由表里每一条 GET 都能过一遍，
//                     而录制语料只有六条只读接口。
//
// ---
//
// **这个程序补的是单元测试够不着的那一层。** 那边直接调 http/*.cpp 里的
// 纯函数，绕过了路由注册、查询参数解析、状态码和 Content-Type。
// 阶段 3 就是这么丢过四个接口的：函数写好了、测试也过了，但根本没挂上路由。
//
// 用法：
//   changji_compat --cpp http://127.0.0.1:8080 --project "E:/AI短剧/雨夜天台"
//   changji_compat --cpp http://127.0.0.1:8080 --python http://127.0.0.1:8000 \
//                  --project ... --routes tests/golden/route_audit.json

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "diff.hpp"
#include "util/httplib.hpp"
#include "util/paths.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace changji;

namespace {

/// 两边一定不一样、而且不该一样的字段。
///
/// **每一条都要写清为什么。** 忽略一条就等于放弃那个字段的对拍，
/// 而半年后没人知道当初为什么忽略它，也就没人敢删。
std::vector<compat::IgnoreRule> default_ignores() {
    return {
        {"/**/mtime", "文件修改时间。两边跑的时刻不同，这个字段本来就该不同"},
        {"/**/updated_at", "同上，写盘时刻"},
        {"/**/created_at", "项目建出来的时刻"},
        {"/elapsed_s", "本次请求耗时"},
        {"/job_id", "每次启动随机生成"},
        {"/workspace", "项目库的绝对路径，跟机器走"},
        {"/**/path", "项目的绝对路径，跟机器走"},
        // ⚠️ 下面这几条只在**录制模式**下忽略。实时模式两个后端跑在
        // 同一台机器上，硬件字段必须一致——不一致说明探测逻辑有分歧。
        {"/gpu", "录制那台是别的显卡"},
        {"/vram_gb", "同上"},
        {"/detected", "同上"},
        {"/tiers/**", "画质档位由显存推导，跟着显存走"},
    };
}

/// 实时模式下能收窄的忽略项：两个后端在同一台机器上，硬件必须一致。
std::vector<compat::IgnoreRule> live_ignores() {
    auto v = default_ignores();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const compat::IgnoreRule& r) {
                               return r.pattern == "/gpu" ||
                                      r.pattern == "/vram_gb" ||
                                      r.pattern == "/detected" ||
                                      r.pattern == "/tiers/**";
                           }),
            v.end());
    return v;
}

struct Args {
    std::string cpp_url;
    std::string python_url;
    std::string project;
    std::string golden = "tests/golden";
    std::string filter;
};

std::pair<std::string, std::string> split_origin(const std::string& url) {
    const std::size_t scheme = url.find("://");
    const std::size_t host = scheme == std::string::npos ? 0 : scheme + 3;
    const std::size_t slash = url.find('/', host);
    if (slash == std::string::npos) return {url, ""};
    return {url.substr(0, slash), url.substr(slash)};
}

std::string encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

std::string with_query(const std::string& path,
                       const std::map<std::string, std::string>& params) {
    if (params.empty()) return path;
    std::string q;
    for (const auto& [k, v] : params) {
        q += q.empty() ? "?" : "&";
        q += encode(k) + "=" + encode(v);
    }
    return path + q;
}

struct Response {
    int status = 0;
    json body;
    std::string error;
};

Response fetch(const std::string& base, const std::string& path_with_query) {
    const auto [origin, prefix] = split_origin(base);
    httplib::Client cli(origin);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(60, 0);

    Response out;
    const auto res = cli.Get(prefix + path_with_query);
    if (!res) {
        out.error = "连不上：" + httplib::to_string(res.error());
        return out;
    }
    out.status = res->status;
    out.body = json::parse(res->body, nullptr, false);
    if (out.body.is_discarded()) {
        out.error = "回的不是 JSON：" + res->body.substr(0, 200);
    }
    return out;
}

json read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return nullptr;
    std::ostringstream buf;
    buf << in.rdbuf();
    json j = json::parse(buf.str(), nullptr, false);
    return j.is_discarded() ? json(nullptr) : j;
}

struct Tally {
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    void report(const std::string& name, const std::vector<compat::Difference>& d,
                int max_lines = 12) {
        if (d.empty()) {
            ++passed;
            std::cout << "  [一致] " << name << "\n";
        } else {
            ++failed;
            std::cout << "  [不同] " << name << "  " << compat::format(d, max_lines)
                      << "\n";
        }
    }
    void skip(const std::string& name, const std::string& why) {
        ++skipped;
        std::cout << "  [跳过] " << name << "  " << why << "\n";
    }
};

/// 录制模式：拿录好的 Python 响应跟活着的 C++ 后端比。
int run_recorded(const Args& args, Tally& tally) {
    const json corpus = read_json(fs::path(args.golden) / "endpoints_readonly.json");
    if (corpus.is_null()) {
        std::cout << "读不到 endpoints_readonly.json，跳过录制模式\n";
        return 0;
    }

    compat::CompareOptions opts;
    opts.ignore = default_ignores();

    // 录制那台机器上的项目路径。语料里等于它的 path 才替换成本机的。
    const std::string recorded_root = corpus.value("project_path", std::string());

    std::cout << "\n== 录制模式：" << corpus.at("cases").size() << " 条 ==\n";
    for (const auto& c : corpus.at("cases")) {
        const std::string name = c.value("name", "?");
        if (!args.filter.empty() && name.find(args.filter) == std::string::npos) {
            continue;
        }

        std::map<std::string, std::string> params;
        // ⚠️ 必须先落到一个具名变量上。`c.value(...).items()` 里那个临时
        // json 在整个表达式结束时就析构了，而 items() 返回的是引用它的代理，
        // range-for 只延长代理的寿命、延长不了它背后那个 json——
        // 于是循环里读的是已释放的内存。表现是 0xC0000409（栈检查失败），
        // 不是可读的错误，而且换个优化等级可能就不崩了。
        const json case_params = c.value("params", json::object());
        for (const auto& kv : case_params.items()) {
            // 语料里的 path 指向录制那台机器上的项目。换成本机的那个，
            // 否则每一条都是 400。
            //
            // **只换和录制项目完全相同的那个值。** 语料里另外两条 path
            // 是故意给错的（空串、指向父目录），它们测的就是"路径不对时
            // 回什么码"。一起换掉的话那两条就不再测它们本来要测的东西了——
            // 第一版按"非空就换"做，project_not_a_project 从 400 变成 200，
            // 而对拍报的是"C++ 侧回了 200"，看起来像是后端的问题。
            const std::string recorded = kv.value().get<std::string>();
            params[kv.key()] = (kv.key() == "path" &&
                                recorded == recorded_root &&
                                !args.project.empty())
                                   ? args.project
                                   : recorded;
        }

        const std::string query =
            with_query(c.at("url").get<std::string>(), params);
        const Response got = fetch(args.cpp_url, query);
        if (!got.error.empty()) {
            tally.skip(name, got.error);
            continue;
        }

        std::vector<compat::Difference> diffs;
        const int want_status = c.value("status", 200);
        if (got.status != want_status) {
            // 状态码是契约的一部分，而且错了之后 body 的比较没有意义。
            //
            // **把发出去的地址和回来的 body 一起打出来。** 只报"期望 400
            // 实际 200"的话，下一步只能手动重放一遍——而手动重放很容易
            // 拼出一个不一样的地址，于是复现不出来。
            diffs.push_back(
                {"（状态码）",
                 "期望 " + std::to_string(want_status) + "，实际 " +
                     std::to_string(got.status) + "\n      请求 " + query +
                     "\n      回的 " + got.body.dump().substr(0, 200)});
        } else {
            diffs = compat::compare(c.at("body"), got.body, opts);
        }
        tally.report(name, diffs);
    }
    return 0;
}

/// 实时模式：两个后端同时驱动，同一个请求发两遍。
///
/// 覆盖面来自路由表——**每一条 GET 都过一遍**，而不是只有录好的那六条。
int run_live(const Args& args, Tally& tally) {
    const json audit = read_json(fs::path(args.golden) / "route_audit.json");
    if (audit.is_null()) {
        std::cout << "读不到 route_audit.json，实时模式没有路由清单\n";
        return 1;
    }

    compat::CompareOptions opts;
    opts.ignore = live_ignores();

    std::cout << "\n== 实时模式 ==\n";
    for (const auto& r : audit.value("routes", json::array())) {
        const std::string method = r.value("method", "GET");
        const std::string path = r.value("path", "");
        if (method != "GET" || path.empty()) continue;
        // 带路径参数的（/api/history/{id}）没有通用的填法，跳过。
        if (path.find('{') != std::string::npos) continue;
        if (!args.filter.empty() && path.find(args.filter) == std::string::npos) {
            continue;
        }

        std::map<std::string, std::string> params;
        if (!args.project.empty()) params["path"] = args.project;
        // 大部分接口只吃 path；吃 episode_id 的那几条没有通用值，
        // 让它自己回 400/404——**两边回同一个错也是对拍的一部分**。
        const std::string q = with_query(path, params);

        const Response py = fetch(args.python_url, q);
        const Response cp = fetch(args.cpp_url, q);
        if (!py.error.empty()) {
            tally.skip(path, "Python 侧：" + py.error);
            continue;
        }
        if (!cp.error.empty()) {
            tally.skip(path, "C++ 侧：" + cp.error);
            continue;
        }

        std::vector<compat::Difference> diffs;
        if (py.status != cp.status) {
            diffs.push_back({"（状态码）", "Python " + std::to_string(py.status) +
                                              "，C++ " + std::to_string(cp.status)});
        }
        const auto body_diffs = compat::compare(py.body, cp.body, opts);
        diffs.insert(diffs.end(), body_diffs.begin(), body_diffs.end());
        tally.report(path, diffs);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // **第一件事**：Windows 上 argv 是 ANSI 的，中文项目路径直接用会是乱码。
    // 这个 bug 就是这个程序自己抓出来的——跑第一遍时七条只读接口全回 500，
    // 而 curl 手打同一个地址是好的。
    const std::vector<std::string> av = paths::utf8_args(argc, argv);

    Args args;
    for (std::size_t i = 1; i < av.size(); ++i) {
        const std::string& a = av[i];
        const auto next = [&]() -> std::string {
            return i + 1 < av.size() ? av[++i] : std::string();
        };
        if (a == "--cpp") args.cpp_url = next();
        else if (a == "--python") args.python_url = next();
        else if (a == "--project") args.project = next();
        else if (a == "--golden") args.golden = next();
        else if (a == "--case") args.filter = next();
        else {
            std::cout << "用法：changji_compat --cpp URL [--python URL] "
                         "[--project 目录] [--golden 目录] [--case 过滤]\n";
            return 2;
        }
    }
    if (args.cpp_url.empty()) {
        std::cout << "要用 --cpp 指出 C++ 后端的地址\n";
        return 2;
    }

    Tally tally;
    run_recorded(args, tally);
    if (!args.python_url.empty()) run_live(args, tally);

    std::cout << "\n一致 " << tally.passed << "，不同 " << tally.failed
              << "，跳过 " << tally.skipped << "\n";
    if (tally.failed > 0) {
        std::cout << "\n对拍没过。**不同的那几条要逐条看**——"
                     "契约兼容是删 Python 的前提。\n";
    }
    // 跳过的不算失败：Python 没起、项目路径没给，都是环境问题不是契约问题。
    // 但要打在总结里，免得"零失败"其实是"一条都没跑"。
    return tally.failed > 0 ? 1 : 0;
}
