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
#include "models/project.hpp"
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
        {"/**/project",
         "同上。写接口那一轮里它还会出现在 422 的 input 回显里——"
         "两个后端各用一份项目副本，回显的自然是各自那份的路径"},
        // ⚠️ 下面这几条只在**录制模式**下忽略。实时模式两个后端跑在
        // 同一台机器上，硬件字段必须一致——不一致说明探测逻辑有分歧。
        {"/gpu", "录制那台是别的显卡"},
        {"/vram_gb", "同上"},
        {"/detected", "同上"},
        {"/tiers/**", "画质档位由显存推导，跟着显存走"},
    };
}

/// 两边**有意不一样**的地方。每一条都对应方案里记过的一个决定。
///
/// 和上面那组不同：上面是"环境不同所以值不同"，这里是"我们决定让它不一样"。
/// 混在一起的话，将来想收窄环境那组时会连这组一起碰。
std::vector<compat::IgnoreRule> intentional_ignores() {
    return {
        {"/checks/**",
         "体检项两边查的东西不同：C++ 侧没有 Python 解释器、多了 sd.cpp 后端和"
         "本地模型两项。形状（name/level/detail/fix）是一致的，前端照渲染。"},
        {"/**/error",
         "错误消息的文字不算契约（方案第三节「校验错误的消息文字不算契约」）。"
         "这里两边的差异来自各自的 HTTP 库：httpx 说 All connection attempts "
         "failed，httplib 说 Could not establish connection。"},
        {"/local",
         "C++ 侧多出来的键：本地模型清单。Python 那边模型归 ComfyUI 管，"
         "没有这个概念。多一个键不影响前端（它按名字取字段）。"},
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
    const auto intentional = intentional_ignores();
    v.insert(v.end(), intentional.begin(), intentional.end());
    return v;
}

/// 把生效的忽略项打出来。
///
/// **不打的话，忽略就是隐形的。** 一份"全部一致"的报告后面藏着十条忽略规则，
/// 而看报告的人不知道——那比对拍不通过更危险。
void print_ignores(const std::vector<compat::IgnoreRule>& rules) {
    std::cout << "\n忽略 " << rules.size() << " 条（每条都有理由）：\n";
    for (const auto& r : rules) {
        std::cout << "  " << r.pattern << "\n      " << r.why << "\n";
    }
}

struct Args {
    std::string cpp_url;
    std::string python_url;
    std::string project;
    std::string golden = "tests/golden";
    std::string filter;
    /// 把每条请求的地址打出来。差异出在"发的不是同一个请求"时，
    /// 没有它只能靠猜。
    bool verbose = false;
    /// 假大模型的工作目录（tools/fake_llm.py 的第二个参数）。
    /// 给了才跑 LLM 阶段的对拍。
    std::string llm_work;
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

/// 给一个案例准备一份干净的项目副本。
///
/// **每个案例各一份，两个后端各一份。** 写接口会改状态：共用一份的话，
/// 前一个案例改过的东西会成为后一个案例的输入，而两个后端还会互相踩。
/// 那时候的"不一致"是自己造出来的，查半天最后发现是对拍的错。
fs::path fresh_copy(const fs::path& src, const std::string& tag) {
    std::error_code ec;
    const fs::path dst = fs::temp_directory_path() /
                         paths::from_utf8("changji_对拍_写") /
                         paths::from_utf8(tag);
    fs::remove_all(dst, ec);
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(src, dst, fs::copy_options::recursive, ec);
    return ec ? fs::path() : dst;
}

/// 把请求里指向项目的那几个键换成这一侧的副本。
json retarget(const json& src, const std::string& project) {
    if (!src.is_object()) return src;
    json out = src;
    for (const char* key : {"project", "path"}) {
        if (out.contains(key) && out[key].is_string()) out[key] = project;
    }
    return out;
}

Response send(const std::string& base, const std::string& method,
              const std::string& path_with_query, const json& body) {
    const auto [origin, prefix] = split_origin(base);
    httplib::Client cli(origin);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(120, 0);

    Response out;
    httplib::Result res =
        method == "POST"
            ? cli.Post(prefix + path_with_query, body.is_null() ? "" : body.dump(),
                       "application/json")
            : (method == "DELETE" ? cli.Delete(prefix + path_with_query)
                                  : cli.Get(prefix + path_with_query));
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

/// 写接口的对拍：同一个请求发给两个后端，比响应，**再比改完之后的项目文件**。
///
/// 比项目文件是这一模式真正的价值：响应一样不代表做的事一样。
/// 一个接口可以回 {"saved": true} 然后什么都没写，或者写错了字段——
/// 那要等到下一次打开项目才发现。
int run_post(const Args& args, Tally& tally) {
    if (args.python_url.empty() || args.project.empty()) {
        std::cout << "\n写接口对拍要 --python 和 --project 都给\n";
        return 0;
    }
    const json corpus = read_json(fs::path(args.golden) / "endpoints_episodes.json");
    if (corpus.is_null()) {
        std::cout << "读不到 endpoints_episodes.json\n";
        return 0;
    }

    compat::CompareOptions opts;
    opts.ignore = live_ignores();
    // 项目文件里的时间戳和绝对路径两边必然不同（各自一份副本）。
    opts.ignore.push_back({"/updated_at", "存盘刷新它"});
    opts.ignore.push_back({"/created_at", "拷贝出来的时刻"});

    const fs::path src = paths::from_utf8(args.project);
    std::cout << "\n== 写接口：" << corpus.at("cases").size() << " 条 ==\n";

    int index = 0;
    for (const auto& c : corpus.at("cases")) {
        const std::string name = c.value("name", "?");
        ++index;
        if (!args.filter.empty() && name.find(args.filter) == std::string::npos) {
            continue;
        }
        // prep 是导出脚本在本机做的前置操作（建集、塞文件），没法通用重放。
        // 跳过而不是硬猜——猜错的话对拍报的差异全是假的。
        if (c.contains("prep") && !c.at("prep").is_null()) {
            tally.skip(name, "有 prep 前置步骤，没法通用重放");
            continue;
        }

        const std::string tag = "c" + std::to_string(index);
        const fs::path py_dir = fresh_copy(src, tag + "_py");
        const fs::path cp_dir = fresh_copy(src, tag + "_cpp");
        if (py_dir.empty() || cp_dir.empty()) {
            tally.skip(name, "拷项目副本失败");
            continue;
        }

        const std::string method = c.value("method", "POST");
        const std::string url = c.at("url").get<std::string>();

        // 查询参数和请求体里的项目路径各自指向自己那一份。
        const json query = c.value("query", json(nullptr));
        const auto build = [&](const fs::path& dir) {
            std::map<std::string, std::string> params;
            if (query.is_object()) {
                // ⚠️ **又是 items() 那个坑，这次是我在同一个文件里犯第二遍。**
                // retarget() 返回的是临时 json，`.items()` 返回引用它的代理；
                // range-for 延长的是代理不是那个 json。上一次的表现是崩溃，
                // 这一次是**静默地一个参数都不产出**——后者更糟：
                // 对拍照跑，报出来的是"两边都说缺参数"，看着像后端的问题。
                //
                // 规矩：`.items()` 只对具名变量调，不对函数返回值调。
                const json aimed = retarget(query, paths::to_utf8(dir));
                for (const auto& kv : aimed.items()) {
                    if (kv.value().is_string()) {
                        params[kv.key()] = kv.value().get<std::string>();
                    }
                }
            }
            return with_query(url, params);
        };
        const json body = c.value("body", json(nullptr));

        const std::string py_url = build(py_dir);
        const std::string cp_url = build(cp_dir);
        if (args.verbose) {
            std::cout << "      -> " << method << " " << cp_url << "\n";
        }
        const Response py = send(args.python_url, method, py_url,
                                 retarget(body, paths::to_utf8(py_dir)));
        const Response cp = send(args.cpp_url, method, cp_url,
                                 retarget(body, paths::to_utf8(cp_dir)));
        if (!py.error.empty()) {
            tally.skip(name, "Python 侧：" + py.error);
            continue;
        }
        if (!cp.error.empty()) {
            tally.skip(name, "C++ 侧：" + cp.error);
            continue;
        }

        std::vector<compat::Difference> diffs;
        if (py.status != cp.status) {
            diffs.push_back({"（状态码）", "Python " + std::to_string(py.status) +
                                              "，C++ " + std::to_string(cp.status)});
        }
        const auto body_diffs = compat::compare(py.body, cp.body, opts);
        diffs.insert(diffs.end(), body_diffs.begin(), body_diffs.end());

        // **再比改完之后的项目文件。** 响应一样不代表做的事一样：
        // 一个接口可以回 {"saved": true} 然后什么都没写。
        const json py_proj = read_json(py_dir / "project.json");
        const json cp_proj = read_json(cp_dir / "project.json");
        if (!py_proj.is_null() && !cp_proj.is_null()) {
            for (auto& d : compat::compare(py_proj, cp_proj, opts)) {
                diffs.push_back({"project.json" + d.path, d.detail});
            }
        }
        tally.report(name, diffs);
    }

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / paths::from_utf8("changji_对拍_写"),
                   ec);
    return 0;
}

/// 读 fake_llm 记下来的提示词。每行一条。
std::vector<std::string> read_prompts(const fs::path& jsonl) {
    std::vector<std::string> out;
    std::ifstream in(jsonl, std::ios::binary);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.contains("prompt")) continue;
        out.push_back(j.at("prompt").get<std::string>());
    }
    return out;
}

/// LLM 阶段对拍。
///
/// **这一模式验的是方案里那条"提示词的拼接必须逐字一致"。**
///
/// 单元测试比的是 C++ 自己拼出来的串和录好的串——那验的是"C++ 没改过"，
/// 验不了"Python 现在还是这么拼的"。这里把假模型夹在中间，
/// 同一个请求让两个后端各发一次，然后逐字节比两条提示词。
///
/// 要三样东西：两个后端都把 [llm].base_url 指向 --fake-llm，
/// 以及 --llm-work 指向假模型的工作目录。
int run_llm(const Args& args, Tally& tally) {
    if (args.python_url.empty() || args.llm_work.empty()) {
        std::cout << "\nLLM 对拍要 --python 和 --llm-work 都给\n";
        return 0;
    }
    const fs::path work = paths::from_utf8(args.llm_work);
    const fs::path reply_file = work / "reply.txt";
    const fs::path prompts_file = work / "prompts.jsonl";

    compat::CompareOptions opts;
    opts.ignore = live_ignores();

    const fs::path src = paths::from_utf8(args.project);
    int index = 0;

    for (const char* corpus_name : {"endpoints_scripting.json",
                                    "endpoints_planning.json"}) {
        const json corpus = read_json(fs::path(args.golden) / corpus_name);
        if (corpus.is_null()) {
            std::cout << "\n读不到 " << corpus_name << "，跳过\n";
            continue;
        }
        std::cout << "\n== LLM 阶段：" << corpus_name << "，"
                  << corpus.at("cases").size() << " 条 ==\n";

        for (const auto& c : corpus.at("cases")) {
            const std::string name = c.value("name", "?");
            ++index;
            if (!args.filter.empty() &&
                name.find(args.filter) == std::string::npos) {
                continue;
            }
            if (!c.contains("llm_reply") || !c.contains("url")) {
                tally.skip(name, "语料里没有 llm_reply 或 url");
                continue;
            }

            // 事先把答案放好。假模型不消耗它，两个后端能各拿一次。
            {
                std::ofstream f(reply_file, std::ios::binary | std::ios::trunc);
                if (!f) {
                    tally.skip(name, "写不了 " + paths::to_utf8(reply_file));
                    continue;
                }
                f << c.at("llm_reply").get<std::string>();
            }

            const std::string tag = "L" + std::to_string(index);
            const fs::path py_dir = fresh_copy(src, tag + "_py");
            const fs::path cp_dir = fresh_copy(src, tag + "_cpp");
            if (py_dir.empty() || cp_dir.empty()) {
                tally.skip(name, "拷项目副本失败");
                continue;
            }

            const std::string url = c.at("url").get<std::string>();
            const json body = c.value("body", json::object());

            // 清空录音，然后一边发一次。**顺序固定：先 Python 后 C++**，
            // 这样 prompts.jsonl 里第 0 条一定是 Python 的。
            { std::ofstream clear(prompts_file, std::ios::trunc); }
            const Response py = send(args.python_url, "POST", url,
                                     retarget(body, paths::to_utf8(py_dir)));
            const Response cp = send(args.cpp_url, "POST", url,
                                     retarget(body, paths::to_utf8(cp_dir)));
            if (!py.error.empty()) {
                tally.skip(name, "Python 侧：" + py.error);
                continue;
            }
            if (!cp.error.empty()) {
                tally.skip(name, "C++ 侧：" + cp.error);
                continue;
            }

            std::vector<compat::Difference> diffs;
            if (py.status != cp.status) {
                diffs.push_back({"（状态码）",
                                 "Python " + std::to_string(py.status) + "，C++ " +
                                     std::to_string(cp.status)});
            }
            for (auto& d : compat::compare(py.body, cp.body, opts)) {
                diffs.push_back({"响应" + d.path, d.detail});
            }

            // **逐字节比提示词。** 这是这一模式存在的理由。
            const auto prompts = read_prompts(prompts_file);
            if (prompts.size() < 2) {
                // 有的案例是校验失败，根本没走到模型那一步——那也是一致的一种，
                // 只要两边都没发。发了一次说明只有一边走到了模型。
                if (prompts.size() == 1) {
                    diffs.push_back({"提示词",
                                     "只有一边发了请求给模型（另一边在到模型之前就返回了）"});
                }
            } else if (prompts[0] != prompts[1]) {
                std::size_t i = 0;
                while (i < prompts[0].size() && i < prompts[1].size() &&
                       prompts[0][i] == prompts[1][i]) {
                    ++i;
                }
                const std::size_t from = i > 60 ? i - 60 : 0;
                diffs.push_back(
                    {"提示词",
                     "第 " + std::to_string(i) + " 字节起不同（长度 " +
                         std::to_string(prompts[0].size()) + " vs " +
                         std::to_string(prompts[1].size()) + "）\n      Python …" +
                         prompts[0].substr(from, 120) + "\n      C++    …" +
                         prompts[1].substr(from, 120)});
            }
            tally.report(name, diffs);
        }
    }

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / paths::from_utf8("changji_对拍_写"),
                   ec);
    return 0;
}

/// 数据层对拍：读一份项目文件再写回去，逐字段比。
///
/// **这是阶段 1 完成标志的端到端版本。** 单元测试逐个字段查过了，但那是
/// 挑着查的——真正要保证的是"一个字段都没丢、一个字段都没多"。
/// 少一个字段的表现是：C++ 存过一次之后，Python 那边再打开就丢了那项设置。
///
/// 不需要起服务。
int run_data(const Args& args, Tally& tally) {
    if (args.project.empty()) {
        std::cout << "\n数据层对拍要 --project 指一个项目\n";
        return 0;
    }
    std::cout << "\n== 数据层：读写往返 ==\n";

    // 拷一份再操作。**绝不在原项目上写**——对拍不该改用户的数据，
    // 而且改坏了之后下一次对拍比的是被改过的东西。
    std::error_code ec;
    const fs::path src = paths::from_utf8(args.project);
    const fs::path work = fs::temp_directory_path() /
                          paths::from_utf8("changji_对拍_数据层");
    fs::remove_all(work, ec);
    fs::copy(src, work, fs::copy_options::recursive, ec);
    if (ec) {
        tally.skip("拷贝项目", ec.message());
        return 0;
    }

    compat::CompareOptions opts;
    opts.ignore = {
        {"/updated_at",
         "存盘会刷新它。这正是它存在的意义，不该要求往返之后不变"},
    };

    for (const char* name : {"project.json", "assets.json"}) {
        const json before = read_json(src / name);
        if (before.is_null()) {
            tally.skip(name, "读不出来或者不是 JSON");
            continue;
        }

        try {
            // 读进来再写回去。中间什么都不改。
            const models::ProjectStore store(work);
            if (std::string(name) == "project.json") {
                models::Project p = store.load_project();
                store.save_project(p);
            } else {
                const models::AssetLibrary a = store.load_assets();
                store.save_assets(a);
            }
        } catch (const std::exception& e) {
            tally.report(name, {{"", std::string("读写时抛了：") + e.what()}});
            continue;
        }

        const json after = read_json(work / name);
        tally.report(name, compat::compare(before, after, opts), 20);
    }
    fs::remove_all(work, ec);
    return 0;
}

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
        else if (a == "--verbose") args.verbose = true;
        else if (a == "--llm-work") args.llm_work = next();
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
    run_data(args, tally);
    run_recorded(args, tally);
    if (!args.python_url.empty()) {
        run_live(args, tally);
        run_post(args, tally);
        if (!args.llm_work.empty()) run_llm(args, tally);
    }

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
