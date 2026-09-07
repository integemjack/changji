#pragma once

// 跨模型的显存调度。
//
// 这一层管**槽位**，不管张量。
//
// 张量级的换入换出 sd.cpp 上游已经做完了（见 device_residency_manager.h：
// 分段执行、异步预取、按压力从最后一段倒着驱逐），而且默认开启。
// 那些东西这里一概不碰——它需要的信息（图切在哪、下一段要哪些张量）
// 这一层根本拿不到。
//
// 这一层回答的是另一个问题，也是 sd.cpp 和 llama.cpp 都回答不了的：
// **哪个模型现在该在显存里**。它们各管各的 context，谁也不知道
// "这一集的分镜已经定稿，语言模型那 9 GB 可以整个还回去了"。
// 只有编排层知道。
//
// ---
//
// 刻意不依赖 sd.cpp 和 llama.cpp：加载和卸载是注入的回调。
// 一是分层，二是很实际的原因——接上真模型之后每个用例要跑几分钟，
// 策略层的 bug 就没法反复撞了。策略要在接推理之前测死。

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace changji::infer {

/// 模型槽位。一个槽位是一个能独立加载和卸载的模型。
///
/// 粒度取在这里，是因为这正好是"编排层知道、推理库不知道"的边界：
/// 编排层知道分镜写完了、首帧出完了；推理库只看得见自己那个 context。
enum class Slot {
    LLM,    ///< 剧本、分镜。一集用一次
    Image,  ///< 首帧生成与图像编辑。每镜一次
    Video,  ///< 文生视频。每镜一次，最贵
    TTS,    ///< 配音。每句一次
};

const char* to_string(Slot s);

/// 驻留策略。
enum class Residency {
    /// 用完就放。适合"一集只用一次"的槽——留着纯粹是占地方。
    Ephemeral,
    /// 留着，显存不够时才被驱逐。适合每个镜头都要用的。
    Cached,
};

/// 一个槽的注册信息。
struct SlotSpec {
    Slot slot = Slot::LLM;
    Residency residency = Residency::Cached;

    /// 估计要占多少显存（字节）。
    ///
    /// 是**估计**不是精确值，用途只有一个：判断"再加载一个装不装得下"。
    /// 估高了会多驱逐几次（慢），估低了会 OOM（崩）。所以宁可估高。
    std::size_t vram_estimate = 0;

    /// 驱逐优先级。**数字小的先被驱逐。**
    ///
    /// 默认按"重新加载有多贵"排：视频模型最贵所以最后驱逐。
    int evict_priority = 0;

    /// 加载。抛异常表示加载失败，调度器会把槽留在未加载状态。
    std::function<void()> load;
    /// 卸载。不允许抛异常——卸载路径上抛异常会让调度器的账对不上。
    std::function<void()> unload;
};

class Scheduler;

/// 借出的槽。析构时归还。
///
/// 拿着它的期间，这个槽**不会被驱逐**——哪怕别的槽因此加载失败。
/// 这是刻意的：正在用的模型被抽走，表现是段错误而不是一个能读的报错。
class Lease {
public:
    Lease() = default;
    ~Lease();

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;

    bool valid() const { return sched_ != nullptr; }
    Slot slot() const { return slot_; }

    /// 提前归还。析构时会自动做，这个是给需要精确控制时机的地方用的。
    void release();

private:
    friend class Scheduler;
    Lease(Scheduler* s, Slot slot) : sched_(s), slot_(slot) {}

    Scheduler* sched_ = nullptr;
    Slot slot_ = Slot::LLM;
};

/// 调度器。线程安全。
class Scheduler {
public:
    Scheduler() = default;
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// 显存预算（字节）。
    ///
    /// 这个数**不是"显卡有多少"**，是"给推理留多少"。要减去驱动上下文
    /// 和别的程序占的。设小了只是多分段（慢），设大了是 OOM（崩），
    /// 所以宁可保守。0 表示不限制。
    void set_budget(std::size_t vram_bytes);
    std::size_t budget() const;

    /// 注册一个槽。同一个槽重复注册会覆盖，但**只在它没加载时**——
    /// 已加载时覆盖会让 load/unload 配不上对。
    void register_slot(SlotSpec spec);

    /// 借一个槽出来。需要时才加载，装不下就先驱逐别的。
    ///
    /// 加载失败会抛异常，而且**已经被驱逐的槽不会自动加载回来**——
    /// 那会把一次失败变成一串连锁加载。
    Lease acquire(Slot slot);

    /// 卸载一个槽。正在被借用时返回 false，不强卸。
    bool evict(Slot slot);

    /// 卸载所有没被借用的槽。跑完一集、或者要交出显存时用。
    void evict_all();

    bool loaded(Slot slot) const;
    int lease_count(Slot slot) const;
    /// 当前估计占用的显存。
    std::size_t resident_bytes() const;

    /// 已加载的槽，按驱逐顺序（先被驱逐的在前）。给测试和诊断用。
    std::vector<Slot> loaded_slots() const;

private:
    friend class Lease;

    struct Entry {
        SlotSpec spec;
        bool is_loaded = false;
        int leases = 0;
        /// 单调递增的使用序号，同优先级时用它做 LRU。
        std::uint64_t last_used = 0;
    };

    void give_back(Slot slot);
    Entry* find(Slot slot);
    const Entry* find(Slot slot) const;
    /// 腾出 need 字节。腾不出来返回 false。调用方必须持锁。
    bool make_room(std::size_t need, Slot keep);
    void do_unload(Entry& e);

    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    std::size_t budget_ = 0;
    std::uint64_t clock_ = 0;
};

/// 全局单例。流水线各处都要借模型，逐层传引用不划算。
Scheduler& scheduler();

}  // namespace changji::infer
