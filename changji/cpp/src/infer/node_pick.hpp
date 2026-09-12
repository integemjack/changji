#pragma once

// 「这个能力派给哪台」——**纯逻辑，不含网络代码**，所以能测。
//
// 这就是方案里那三层决定的落地点：
//
//   能不能  ← 节点自检报上来的（`capability.hpp`，量出来的事实）
//   准不准  ← 用户在那张表上勾的（关掉某台的某个能力）
//   该不该  ← 这个文件：谁在线、谁空着
//
// **「全自动」= 三层都开；「钉死某台」= 第二层只勾一个。同一套代码，
// 不是两种模式。** 这一条是这个设计能成立的关键——分成两条路的话，
// 「自动」和「手动」迟早在某个边角上行为不一致，而那种不一致查起来
// 要先怀疑调度、再怀疑配置、最后才想到是两份代码。
//
// 挑错了不会报错：挑了台干不了的，是那一镜失败；该挑的没挑，是那台
// 一直闲着而界面上看不出为什么。所以这一段必须能反复撞。

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "infer/capability.hpp"

namespace changji::infer {

/// 一台节点在调度器眼里的样子。
struct NodeState {
    /// `http://gpu-box:9001`。**同时是它的身份**——同一台机器上的两张卡
    /// 是两个节点，端口不同。
    std::string url;
    /// 界面上显示的名字，节点自己报的。
    std::string name;

    /// 上一次问 `/status` 通了没有。
    ///
    /// **连不上不等于永久出局**：网络抖一下、对面在重启，过一会儿还要再问。
    /// 出局与否由 `WorkerRoster` 那边的冷却管，这里只记当下这一眼。
    bool online = false;

    /// 这台自检说能干的。**是它说的，不是我们猜的。**
    std::set<Capability> able;

    /// 用户关掉的。"这台留着写剧本，不许拿去出片"就是往这儿加一项。
    std::set<Capability> off;

    /// 此刻有活在跑。
    bool busy = false;
};

/// 能干这个能力、没被关掉、而且在线的那些，按传入顺序。
std::vector<const NodeState*> candidates_for(
    const std::vector<NodeState>& nodes, Capability c);

/// 从候选里挑一台。
///
/// **空闲的优先；全忙也要回一台**（排队等它，而不是判这一镜失败）——
/// 一镜一两分钟，等得起；当场失败的话用户得到的是一集里随机几镜没了。
/// 一台候选都没有才回空。
std::optional<std::string> pick_for(const std::vector<NodeState>& nodes,
                                    Capability c);

/// 这个能力一台都派不出去时，给用户看的那句话。
///
/// **要说清是哪一层拦的**：没有这台机器、还是它干不了、还是你自己关了。
/// 只说"没有可用节点"的话，用户唯一能做的就是挨个去翻配置。
std::string why_no_node(const std::vector<NodeState>& nodes, Capability c);

}  // namespace changji::infer
