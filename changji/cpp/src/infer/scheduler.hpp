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
#include <map>
#include <optional>
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

/// 某个槽借不到显存时该对用户说什么。
///
/// **每个槽的出路不一样**，笼统一句"显存不够"用户无从下手：配音和大模型
/// 都能换成外部服务（改配置就行，不用改代码），而出图出片躲不掉，
/// 只能在"权重放内存"和"降分辨率"之间挑。
std::string out_of_vram_message(Slot s);

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

    /// 这个槽跑起来**实际**要占的显存。0 = 没有，就拿 `vram_estimate` 顶。
    ///
    /// **只用在问过卡之后那条分支上。** `vram_estimate` 是按整份预算估的，
    /// 生产里它就等于整卡的九成——拿它去问"卡上空着的够不够"，答案永远是
    /// 不够：另一个槽一装上，就再也凑不出第二份九成。于是那条"显存够就别卸"
    /// 的路从来没走通过，每次点出片都把大模型卸掉，
    /// 而用户要的恰恰是"如果显存够的就不用清理"。
    ///
    /// 所以再带一个老实数：常驻权重 + 计算缓冲，由
    /// `ModelsConfig::video_live_vram_gb` / `image_live_vram_gb` 算，
    /// 和决定权重放哪用的是同一组实测常数。
    ///
    /// **静态那条路仍然用 `vram_estimate`**，一点没放松：问不到卡的时候
    /// 保守是唯一安全的选择。这个数只在真问到了空闲显存时才拿出来比。
    /// **是个函数，不是一个数。** 模型可以在运行中被换掉（初始化页就能
    /// 换），而槽**一个进程只注册一次**（已加载的槽重新注册会抛）。存成
    /// 定值的话：开机时还没配模型 → 算出来只有计算缓冲那几 GB → 用户下了
    /// 一份 20 GB 的 fp8 → 这个数还停在开机那一刻 → 调度器以为够、不腾地方
    /// → CUDA OOM。估低了是崩，方向恰恰是最不能错的那个。
    /// 每次问一遍就没有这个问题。
    std::function<std::size_t()> live_vram;

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

    /// 换掉"现在还空多少显存"的问法。默认问 nvidia-smi。
    ///
    /// **给测试用的**：单元测试里不该真去跑 nvidia-smi（跑不跑得动看机器，
    /// 而且慢）。回 nullopt 表示问不到，那时按静态估算走。
    using FreeVramProbe = std::function<std::optional<double>()>;
    void set_free_vram_probe(FreeVramProbe probe);

    /// 记下某个槽**实测**跑起来占了多少显存（字节）。
    ///
    /// **这是"根据实时的显存情况来判断"里最要紧的一块。** 静态估算靠不住：
    /// 2026-09-11 在 96 GB 卡上实测，`weights="cpu"` 那一路我算 14.6 GB、
    /// 真实峰值 **74 GB**，差五倍；`te=cpu` 那一路我算 81.8 GB，实际超过
    /// 95.6 GB 直接 CUDA OOM 把整个进程带走。原因是权重放不放显存并不能
    /// 决定占用——ggml 仍然按层往显存搬，分配器还留着大池子，而这些随
    /// 模型大小、画幅、帧数剧烈变化，不是一两个常数估得准的。
    ///
    /// 所以改成**量**：跑第一次时按保守估算办事（该卸就卸，绝不冒险），
    /// 跑的过程中记下真实占用，之后就按这个数判断。自校准，且**永远不会
    /// 因为乐观而 OOM**——没量到之前一律走保守那条。
    void record_measured_vram(Slot slot, std::size_t bytes);

    /// 取实测值。没量过回 0。
    std::size_t measured_vram(Slot slot) const;

    /// 现在量到的全部，按槽列出来。给持久化用。
    std::map<Slot, std::size_t> all_measured() const;

    /// 有新的高水位时叫一声。**用来落盘**——量到的数只活在进程里的话，
    /// 每次重启后的头一次出片都会白白卸掉大模型（那时候还没量到，
    /// 走的是保守那条），而这个进程可能一天重启好几次。
    ///
    /// 回调放在调度器外面，是不想让它碰文件系统：它在借槽的关键路径上，
    /// 而那条路上已经有一次 nvidia-smi 了，不该再加一次磁盘 IO 的不确定性。
    /// **回调是在锁外调的**，实现里可以放心写文件。
    using MeasuredSink = std::function<void(Slot, std::size_t)>;
    void set_measured_sink(MeasuredSink sink);

    /// 整张卡有多少显存（字节）。**问不到卡时的退路要靠它。**
    ///
    /// nvidia-smi 不是永远问得到：这个进程初始化 CUDA 之后映射着十几 GB，
    /// 而 free_vram_gb 是 fork + exec 去跑 nvidia-smi 的，在这种进程里
    /// fork 本来就是 NVIDIA 明确不支持的做法。问不到就一路走到驱逐，
    /// 于是"显存够就不清理"在真机上等于从来没生效过。
    ///
    /// 有了总量，就能拿**已经量到的**那些槽推算空闲：
    ///     空闲 ≈ 总量 − Σ(已装着的槽各自量到的占用)
    /// 只用量到的数，不猜——有任何一个装着的槽没量过，就老老实实回到
    /// 保守那条去卸。这样永远不会比事实更乐观。
    void set_total_vram(std::size_t bytes);

    /// 最近一次"要不要腾地方"的判断过程，给界面看的。
    ///
    /// **为什么要把它露出来。** 这个判断出错的表现是"该留的时候卸了"或者
    /// "该卸的时候没卸"，前者只是慢，后者是 CUDA OOM 把整个服务带走。
    /// 而它一直只能靠登上机器看 stderr 才查得到——2026-09-11 服务器连不上
    /// 的那几个钟头里，这条线索完全断了。
    /// 记在进程里、从接口读得到，比什么都强。
    struct RoomDecision {
        bool valid = false;       ///< 还没发生过判断时是假
        Slot slot = Slot::Image;  ///< 当时要借哪个槽
        std::size_t need = 0;     ///< 保守估值
        std::size_t live = 0;     ///< 真正拿来比的那个数
        std::size_t free_seen = 0;///< 当时认为空闲多少。0 = 没拿到
        bool probed = false;      ///< 空闲是问卡问来的（否则是推算的）
        bool kept = false;        ///< 结论：够，没卸
        int evicted = 0;          ///< 卸掉了几个槽
        /// live 是量出来的（真），还是估出来的（假）。
        ///
        /// **这一位决定上面那个"够"值不值得信。** 估算在这一路上错得很离谱
        /// ——video 走 weights="cpu" 时算 14.6 GB、实测 74 GB。拿这种数判出
        /// 来的"够，不卸"，后果是 CUDA OOM，而 OOM 走 GGML_ASSERT，
        /// abort() 把整个服务带走，不是一条能读的报错。
        bool live_measured = false;
    };
    RoomDecision last_room_decision() const;
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
    FreeVramProbe free_vram_;
    /// 每个槽实测的占用。见 record_measured_vram。
    std::map<Slot, std::size_t> measured_;
    MeasuredSink measured_sink_;
    /// 整张卡的显存。0 = 不知道，那时候没有退路可走。
    std::size_t total_vram_ = 0;
    RoomDecision last_decision_;
    std::uint64_t clock_ = 0;
};

/// 全局单例。流水线各处都要借模型，逐层传引用不划算。
Scheduler& scheduler();

}  // namespace changji::infer
