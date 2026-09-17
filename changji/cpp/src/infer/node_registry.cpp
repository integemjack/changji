#include "infer/node_registry.hpp"

#include "config/runtime.hpp"
#include "infer/local_exec.hpp"
#include "infer/node_prefs.hpp"
#include "infer/node_status.hpp"
#include "util/httplib.hpp"
#include "util/paths.hpp"

#include <algorithm>

namespace changji::infer {

namespace {

using nlohmann::json;

/// 问一台的 `/status` 要等多久。
///
/// **短一点。** 这几个请求是串着发的，而它们挡在页面前面：三台不通、
/// 每台等十秒，用户看到的是一个转了半分钟的圈。一台机器答不出一个
/// 自我介绍，多半也接不了活。
constexpr int kProbeTimeoutS = 3;

/// 这段回包是不是场记自己的网页界面。
///
/// 只认嵌进二进制的那张 index.html 的两个固定标记，不做宽松匹配：
/// 宽松了就会把别人家的 HTML 也说成"你起错模式了"，那比原来那句还糟。
bool looks_like_webapp(const std::string& body) {
    return body.find("<div id=\"app\">") != std::string::npos &&
           body.find("场记") != std::string::npos;
}

std::pair<std::string, std::string> split_url(const std::string& url) {
    const auto pos = url.find("://");
    const std::string rest =
        pos == std::string::npos ? url : url.substr(pos + 3);
    const auto slash = rest.find('/');
    if (slash == std::string::npos) return {url, ""};
    return {url.substr(0, pos == std::string::npos ? slash : pos + 3 + slash),
            rest.substr(slash)};
}

/// 把配置里那几个 off 翻成能力。
///
/// **认不出的名字要说出来**，不能当没看见——那会变成"我明明关了出片，
/// 它还是派过去了"，而用户完全不知道是自己拼错了。
std::set<Capability> parse_off(const std::vector<std::string>& off,
                               std::string& complaint) {
    std::set<Capability> out;
    for (const std::string& s : off) {
        if (const auto c = capability_from(s)) {
            out.insert(*c);
        } else {
            if (!complaint.empty()) complaint += "；";
            complaint += "配置里的 off 有个认不出的能力名「" + s + "」";
        }
    }
    return out;
}

/// 本机这一行。
NodeState local_node(const config::Settings& s) {
    NodeState n;
    n.url = "local";
    const auto facts = probe_facts(s);
    for (const auto& r : capabilities_of(facts)) {
        if (r.able) {
            n.able.insert(r.cap);
        } else {
            n.why[r.cap] = r.why;
        }
    }
    n.online = true;   // 自己总是在线的
    n.busy = local_exec().busy();
    // 名字从自我介绍里取，和别的机器显示成同一种东西
    const auto js = node_status_json(s, config::runtime().profile());
    n.name = js.value("name", std::string("本机"));
    return n;
}

}  // namespace

std::vector<NodeState> NodeRegistry::snapshot(const config::Settings& s,
                                              std::chrono::seconds max_age) {
    std::vector<NodeState> out;
    {
        std::lock_guard lg(mu_);
        const auto age = std::chrono::steady_clock::now() - fetched_at_;
        if (!nodes_.empty() && age < max_age) out = nodes_;
    }
    if (out.empty()) {
        refresh(s);
        std::lock_guard lg(mu_);
        out = nodes_;
    }

    // **开关不进缓存，每次现算。** 缓存的是"问出来的事实"（在线没有、
    // 能干什么），那要发 HTTP 所以值得缓；开关是本地读一个小文件，
    // 而且点一下就该立刻生效——进了缓存的话，用户点完要等五秒才看得到。
    const NodePrefs prefs = load_node_prefs(s.workspace_path());
    for (NodeState& n : out) {
        const auto it = prefs.find(n.url);
        n.off = n.off_locked;
        if (it != prefs.end()) n.off.insert(it->second.begin(), it->second.end());
    }
    return out;
}

void NodeRegistry::refresh(const config::Settings& s) {
    std::vector<NodeState> fresh;
    fresh.push_back(local_node(s));

    for (const config::PeerNodeConfig& cfg : s.peer.nodes) {
        NodeState n;
        n.url = cfg.url;
        n.name = cfg.url;   // 连上了再换成它自报的名字
        std::string complaint;
        // 配置里那份是锁着的：改它要动配置文件。界面上点的那份在
        // snapshot 里合进来。
        n.off_locked = parse_off(cfg.off, complaint);
        n.off = n.off_locked;

        const auto [origin, prefix] = split_url(cfg.url);
        httplib::Client cli(origin);
        cli.set_connection_timeout(kProbeTimeoutS, 0);
        cli.set_read_timeout(kProbeTimeoutS, 0);
        const std::string token = cfg.token.empty() ? s.peer.token : cfg.token;
        if (!token.empty()) cli.set_bearer_token_auth(token);

        auto res = cli.Get(prefix + "/status");
        if (!res) {
            n.online = false;
            n.error = "连不上：" + httplib::to_string(res.error());
        } else if (res->status == 401) {
            // **单独认这一种。** 「口令不对」和「连不上」要做的事完全不同，
            // 而两边显示成同一句话的话，用户会去查网络。
            n.online = false;
            n.error = "口令不对。这台的 [peer].token 和你这边填的对不上";
        } else if (res->status != 200) {
            n.online = false;
            n.error = "答的不是 200：" + std::to_string(res->status);
        } else {
            const auto js = json::parse(res->body, nullptr, false);
            if (js.is_discarded()) {
                n.online = false;
                // **最常犯的那个错要单独认出来。** 理由同上面 401 那一条。
                //
                // 用户手上刚装好、刚在浏览器里打开的那一个，就是完整服务
                // （`changji --port 8080`）。把它的地址填到这张表里是第一
                // 反应——而完整服务的 `/status` 落在前端的兜底路由上，
                // 回的是 200 + 那张 index.html。于是这里解析失败，原来一律
                // 说「那头多半不是 changji」：**结论正好说反了**，对面正是
                // changji，只是起错了模式。用户照这句话去查地址、查端口、
                // 查防火墙，而要改的是那台的起法。
                if (looks_like_webapp(res->body)) {
                    n.error =
                        "这台起的是完整服务，不是工作进程。派活要的是 "
                        "`changji --worker --port 9101`（只算不发界面）；"
                        "现在这个端口上是网页界面，填它没用";
                } else {
                    n.error = "答的不是 JSON，那头多半不是 changji";
                }
            } else {
                n.online = true;
                n.name = js.value("name", cfg.url);
                n.busy = js.value("busy", false);
                // 那台同时收得下几件。没报（老版本）按 1。
                n.slots = std::max<std::size_t>(
                    1, js.value("slots", std::size_t{1}));
                if (js.contains("capabilities") &&
                    js["capabilities"].is_array()) {
                    for (const auto& item : js["capabilities"]) {
                        const auto c = capability_from(item.value("cap", ""));
                        if (!c) continue;
                        if (item.value("able", false)) {
                            n.able.insert(*c);
                        } else {
                            // **那句话得跟着一起过来。** 它是那台自己算的
                            // （缺哪个文件、编没编进去，只有它知道），这边
                            // 除了原样传没有别的办法补出来。
                            n.why[*c] = item.value("why", std::string());
                        }
                    }
                }
            }
        }
        if (!complaint.empty()) {
            n.error = n.error.empty() ? complaint : n.error + "；" + complaint;
        }
        fresh.push_back(std::move(n));
    }

    std::lock_guard lg(mu_);
    nodes_ = std::move(fresh);
    fetched_at_ = std::chrono::steady_clock::now();
}

NodeRegistry& node_registry() {
    static NodeRegistry one;
    return one;
}

json nodes_json(const config::Settings& s) {
    return nodes_json(node_registry().snapshot(s));
}

// `nodes_json(const std::vector<NodeState>&)` **搬到 node_json.cpp 去了**。
// 它是纯的（NodeState → JSON，不碰网络），而这个文件因为 node_registry()
// 要去问每一台的 /status 而 include 了 httplib——测试目标那一列上面写着
// 「一个网络库都不链」，所以只要它留在这儿，test_node_table.cpp 就永远
// 链不起来（实测：undefined reference 到 nodes_json）。


}  // namespace changji::infer
