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

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "infer/capability.hpp"

namespace changji::infer {

/// 一台节点在调度器眼里的样子。
struct NodeState {
    /// `http://gpu-box:8080`。**同时是它的身份**——一台机器一条；那台
    /// 有几张卡是它自己的事，报在 `slots` 里。
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

    /// 干不了的那几样，各自为什么。**这句话是用户唯一的线索**——
    /// 界面上那个灰格子除了"灰"以外什么都不说，悬停看到的就是这一句。
    ///
    /// 话是**产出那一端**算的（`missing_for`，跑在那台机器上），不是这边
    /// 猜的：缺哪个模型文件、编没编进 sd.cpp，只有那台自己知道。
    /// 能干的不在表里；整台连不上时每一格都是那句连接错误。
    std::map<Capability, std::string> why;

    /// 关掉的，**两处的并集**：配置文件里那份，加界面上点的那份。
    /// "这台留着写剧本，不许拿去出片"就是往这儿加一项。
    std::set<Capability> off;

    /// 上面那些里，哪些是**配置文件**关的。
    ///
    /// 界面靠它决定这个格子点不点得动：配置关的要去改配置（那是写配置
    /// 的人的决定），界面关的点一下就开。两种显示成一样的话，用户会在
    /// 一个点不动的格子上反复点。
    std::set<Capability> off_locked;

    /// 此刻有活在跑。
    bool busy = false;

    /// 同时收得下几件活。**是它报的**（`/status` 里的 `slots`），等于那台
    /// `POST /task` 开始回 409 的门槛：主程序带几张卡就拉起几个子进程，
    /// 报的是活着的子进程数；`--worker` 单进程报 1；没报按 1。
    ///
    /// 派活那边按这个数开槽（`remote_slots_for`）。开多了不是"多等一会儿"
    /// ——池收到 409 是判那一镜失败；开少了就是「只用了一张卡」。
    std::size_t slots = 1;

    /// 连不上／口令不对／答的不是 JSON 时那句话。在线时是空的。
    ///
    /// **要分得出是哪一种**：口令不对和连不上要做的事完全不同，
    /// 显示成同一句话的话用户会去查网络。
    std::string error;
};

/// 能干这个能力、没被关掉、而且在线的那些，按传入顺序。
std::vector<const NodeState*> candidates_for(
    const std::vector<NodeState>& nodes, Capability c);

/// 派活时别的机器各占几个槽：候选里每台 `url` 重复 `slots` 次，本机那条不算。
///
/// 池那边一个下标一个槽、按下标记忙闲，所以同一个 url 出现两次就是两条
/// 独立的通道——**这就是一台双卡机两张卡一起干的全部机制**，池和协议都
/// 不用知道"卡"这回事。
std::vector<std::string> remote_slots_for(const std::vector<NodeState>& nodes,
                                          Capability c);

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
