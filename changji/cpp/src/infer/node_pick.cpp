#include "infer/node_pick.hpp"

#include <algorithm>

#include "infer/worker_pool.hpp"   // kLocalEndpoint

namespace changji::infer {

std::vector<const NodeState*> candidates_for(const std::vector<NodeState>& nodes,
                                             Capability c) {
    std::vector<const NodeState*> out;
    for (const NodeState& n : nodes) {
        if (!n.online) continue;
        if (n.off.count(c) != 0) continue;
        if (n.able.count(c) == 0) continue;
        out.push_back(&n);
    }
    return out;
}

std::vector<std::string> remote_slots_for(const std::vector<NodeState>& nodes,
                                          Capability c) {
    std::vector<std::string> out;
    for (const NodeState* n : candidates_for(nodes, c)) {
        if (n->url == kLocalEndpoint) continue;
        // 报 0 按 1：一台在线的机器不能因为少报一个数就从池里消失
        const std::size_t k = std::max<std::size_t>(1, n->slots);
        for (std::size_t i = 0; i < k; ++i) out.push_back(n->url);
    }
    return out;
}

std::optional<std::string> pick_for(const std::vector<NodeState>& nodes,
                                    Capability c) {
    const auto cands = candidates_for(nodes, c);
    if (cands.empty()) return std::nullopt;
    for (const NodeState* n : cands) {
        if (!n->busy) return n->url;
    }
    // 全忙。**回第一个去排队，不是失败**——一镜一两分钟等得起，
    // 当场失败的话用户得到的是一章里随机几镜没了。
    return cands.front()->url;
}

std::string why_no_node(const std::vector<NodeState>& nodes, Capability c) {
    const std::string what = label_of(c);
    if (nodes.empty()) {
        return "没有任何一台机器登记在案，" + what + "这一步没地方派";
    }

    int offline = 0, turned_off = 0, cannot = 0;
    for (const NodeState& n : nodes) {
        if (!n.online) {
            ++offline;
        } else if (n.off.count(c) != 0) {
            ++turned_off;
        } else if (n.able.count(c) == 0) {
            ++cannot;
        }
    }

    // **要说清是哪一层拦的。** 只说"没有可用节点"的话，用户唯一能做的
    // 就是挨个去翻配置——而这三种情况要做的事完全不同：等一等／去表上
    // 打开／去装模型。
    std::string out = what + "这一步一台都派不出去：";
    bool first = true;
    const auto add = [&](int n, const std::string& why) {
        if (n <= 0) return;
        if (!first) out += "；";
        out += std::to_string(n) + " 台" + why;
        first = false;
    };
    add(offline, "连不上");
    add(turned_off, "被你在表上关掉了");
    add(cannot, "自己说干不了（缺模型或者没编进去，看那台的状态）");
    return out;
}

}  // namespace changji::infer
