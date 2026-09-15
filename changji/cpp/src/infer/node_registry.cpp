#include "infer/node_registry.hpp"

#include "config/runtime.hpp"
#include "infer/local_exec.hpp"
#include "infer/node_prefs.hpp"
#include "infer/node_status.hpp"
#include "util/httplib.hpp"
#include "util/paths.hpp"

namespace changji::infer {

namespace {

using nlohmann::json;

/// 问一台的 `/status` 要等多久。
///
/// **短一点。** 这几个请求是串着发的，而它们挡在页面前面：三台不通、
/// 每台等十秒，用户看到的是一个转了半分钟的圈。一台机器答不出一个
/// 自我介绍，多半也接不了活。
constexpr int kProbeTimeoutS = 3;

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
        if (r.able) n.able.insert(r.cap);
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
                n.error = "答的不是 JSON，那头多半不是 changji";
            } else {
                n.online = true;
                n.name = js.value("name", cfg.url);
                n.busy = js.value("busy", false);
                if (js.contains("capabilities") &&
                    js["capabilities"].is_array()) {
                    for (const auto& item : js["capabilities"]) {
                        if (!item.value("able", false)) continue;
                        if (const auto c =
                                capability_from(item.value("cap", ""))) {
                            n.able.insert(*c);
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
    const auto nodes = node_registry().snapshot(s);

    json rows = json::array();
    for (const NodeState& n : nodes) {
        json caps = json::array();
        for (const Capability c : all_capabilities()) {
            const bool able = n.able.count(c) != 0;
            const bool off = n.off.count(c) != 0;
            // 三态：干不了（灰）／能干但你关了（空心）／参与调度（实心）。
            // 界面照这个画，不自己推。
            const bool locked = n.off_locked.count(c) != 0;
            caps.push_back({{"cap", to_string(c)},
                            {"label", label_of(c)},
                            {"able", able},
                            {"off", off},
                            // 配置文件关的，界面上点不动
                            {"locked", locked},
                            {"on", able && off == false && n.online}});
        }
        rows.push_back({{"url", n.url},
                        {"name", n.name},
                        {"online", n.online},
                        {"busy", n.busy},
                        {"error", n.error},
                        {"local", n.url == "local"},
                        {"capabilities", caps}});
    }

    // 每个能力现在有几台能接。界面上那句"出片：2 台可用"用它，
    // 派不出去时那句话也在这儿拼好——两边各算一次迟早对不上。
    json summary = json::array();
    for (const Capability c : all_capabilities()) {
        const auto cands = candidates_for(nodes, c);
        summary.push_back(
            {{"cap", to_string(c)},
             {"label", label_of(c)},
             {"count", static_cast<int>(cands.size())},
             {"why", cands.empty() ? why_no_node(nodes, c) : std::string()}});
    }

    return json{{"nodes", rows}, {"summary", summary}};
}

}  // namespace changji::infer
