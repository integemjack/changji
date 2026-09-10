#include "infer/scheduler.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>

#include "util/paths.hpp"

namespace changji::infer {

const char* to_string(Slot s) {
    switch (s) {
        case Slot::LLM:   return "LLM";
        case Slot::Image: return "图像";
        case Slot::Video: return "视频";
        case Slot::TTS:   return "配音";
    }
    return "?";
}

// ---- Lease ----

Lease::~Lease() { release(); }

Lease::Lease(Lease&& other) noexcept : sched_(other.sched_), slot_(other.slot_) {
    other.sched_ = nullptr;
}

Lease& Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        sched_ = other.sched_;
        slot_ = other.slot_;
        other.sched_ = nullptr;
    }
    return *this;
}

void Lease::release() {
    if (sched_) {
        sched_->give_back(slot_);
        sched_ = nullptr;
    }
}

// ---- Scheduler ----

Scheduler::~Scheduler() {
    // 析构时把还加载着的都卸掉。引用计数不为零也卸——
    // 到这个时候进程要么在退出要么在重建调度器，留着更糟。
    std::lock_guard lg(mu_);
    for (Entry& e : entries_) {
        if (e.is_loaded) do_unload(e);
    }
}

void Scheduler::set_budget(std::size_t vram_bytes) {
    std::lock_guard lg(mu_);
    budget_ = vram_bytes;
}

std::size_t Scheduler::budget() const {
    std::lock_guard lg(mu_);
    return budget_;
}

void Scheduler::register_slot(SlotSpec spec) {
    std::lock_guard lg(mu_);
    for (Entry& e : entries_) {
        if (e.spec.slot == spec.slot) {
            if (e.is_loaded) {
                // 覆盖一个已加载的槽，新的 unload 会去卸一个不是它加载的东西。
                // 让它响亮地失败，比留一个对不上的账好。
                throw std::runtime_error(
                    std::string("槽 ") + to_string(spec.slot) +
                    " 已经加载了，不能重新注册。先 evict 再注册。");
            }
            e.spec = std::move(spec);
            return;
        }
    }
    Entry e;
    e.spec = std::move(spec);
    entries_.push_back(std::move(e));
}

Scheduler::Entry* Scheduler::find(Slot slot) {
    for (Entry& e : entries_) {
        if (e.spec.slot == slot) return &e;
    }
    return nullptr;
}

const Scheduler::Entry* Scheduler::find(Slot slot) const {
    for (const Entry& e : entries_) {
        if (e.spec.slot == slot) return &e;
    }
    return nullptr;
}

void Scheduler::do_unload(Entry& e) {
    if (!e.is_loaded) return;
    // unload 声明为不抛异常。真抛了也不能让它穿出去——
    // 这个函数在析构和驱逐路径上都被调用，那两条路上抛异常都是灾难。
    if (e.spec.unload) {
        try {
            e.spec.unload();
        } catch (...) {
            // 吞掉。账还是要平，否则后面的预算计算全错。
        }
    }
    e.is_loaded = false;
}

void Scheduler::set_free_vram_probe(FreeVramProbe probe) {
    std::lock_guard lg(mu_);
    free_vram_ = std::move(probe);
}

void Scheduler::record_measured_vram(Slot slot, std::size_t bytes) {
    if (bytes == 0) return;
    MeasuredSink sink;
    {
        std::lock_guard lg(mu_);
        // **只往上记，不往下调。** 同一个槽不同镜头的占用会有出入（帧数、
        // 分辨率、有没有挂 LoRA），取见过的最大值才安全——按最近一次记的话，
        // 一个小镜头会把上限拉低，下一个大镜头就 OOM 了。
        auto& cur = measured_[slot];
        if (bytes <= cur) return;   // 没长高，不用惊动落盘
        cur = bytes;
        sink = measured_sink_;
    }
    // **锁外调。** 落盘要写文件，拿着调度器的锁做 IO 会把别的借槽请求
    // 一起卡住，而借槽是出图出片的关键路径。
    if (sink) sink(slot, bytes);
}

std::map<Slot, std::size_t> Scheduler::all_measured() const {
    std::lock_guard lg(mu_);
    return measured_;
}

void Scheduler::set_measured_sink(MeasuredSink sink) {
    std::lock_guard lg(mu_);
    measured_sink_ = std::move(sink);
}

void Scheduler::set_total_vram(std::size_t bytes) {
    std::lock_guard lg(mu_);
    total_vram_ = bytes;
}

std::size_t Scheduler::measured_vram(Slot slot) const {
    std::lock_guard lg(mu_);
    const auto it = measured_.find(slot);
    return it == measured_.end() ? 0 : it->second;
}

std::string out_of_vram_message(Slot slot) {
    // **每个槽给的出路不一样。** 只说一句"显存不够"的话，用户下一步
    // 无从下手——尤其是配音：它是唯一一个有现成外部服务可换的，
    // 而那条路不用改一行代码，改个配置就行。
    const std::string base =
        std::string("显存不够加载 ") + to_string(slot) +
        "。腾不出空间——别的槽正被借用着，或者这张卡确实太小。";
    switch (slot) {
        case Slot::TTS:
            return base +
                   "\n配音这一步**不必占显存**：把 changji.toml 里的 "
                   "[tts].backend 改成 \"http\"，再填 [tts].base_url "
                   "指向一个外部配音服务，本机就不用装配音模型了。"
                   // **别提 comfy。** 那条路 2026-09-10 拆了，现在填它
                   // 连配置校验都过不去。把人指到一个不存在的取值上，
                   // 比只说一句"显存不够"更糟。
                   "\n（另一个取值是 \"local\"，就是现在这条、进程内跑的。）";
        case Slot::LLM:
            return base +
                   "\n写剧本这一步也可以不占显存：在设置页把大模型的"
                   "「跑在哪」改成外接 API，或者直接填 [llm].base_url——"
                   "任何一个兼容 OpenAI 接口的服务都行，本地的云上的都可以。";
        case Slot::Image:
        case Slot::Video:
            // 分辨率**不在设置页了**：2026-09-10 搬到项目的「画面」那张卡
            // （竖屏/横屏 + 标准/2K）。指错地方用户会在设置页翻半天。
            //
            // **档位的名字要和界面上写的一模一样。** 取值仍然叫 "720p"
            // （存在项目的 changji.toml 里，改了名老项目读不出来），但
            // 下拉框里显示的是「标准（544×928）」——2026-09-10 画幅从
            // 704×1280 改成 544×928 之后，再管它叫 720p 就是假的
            // （720p 是 720 行）。这句话是用户此刻唯一的线索，写 "720p"
            // 他会在那张卡上找一个不存在的选项。
            return base +
                   "\n出图出片是躲不掉的显存开销。能调的两处：在项目页的"
                   "「画面」那张卡把清晰度降一档——三档分别是"
                   "「标准（544×928）」「高清（704×1280）」「2K（2560×1440）」，"
                   // **别再教人手填 weights。** 它默认是 "smart"，装不下时
                   // 程序自己就会把权重放内存——用户 2026-09-10 的原话：
                   // "都应该让程序自己算"。这里只对"手动写死过"的人有意义。
                   "或者检查 [models].weights 是不是被手动写死了——"
                   "它默认 \"smart\"，会按这张卡和模型大小自己决定权重放哪，"
                   "写死成别的值就把这份判断关掉了。";
    }
    return base;
}

bool Scheduler::make_room(std::size_t need, Slot keep) {
    if (budget_ == 0) return true;  // 不限制

    std::size_t used = 0;
    for (const Entry& e : entries_) {
        if (e.is_loaded) used += e.spec.vram_estimate;
    }
    if (used + need <= budget_) return true;

    // **静态估算说装不下之前，先真去问一眼卡上还空着多少。**
    //
    // vram_estimate 是每个槽按整份预算估的（"同时只装得下一个"），
    // 那是保守的：权重放内存时显存里其实只有计算缓冲，两个槽同时在也没事。
    // 只信估算的话，每次切阶段都要卸一个再装一个——一次重装是几十秒到几分钟，
    // 而卡上可能一直空着一大半。
    //
    // 问不到就退回估算。**"问不到"不等于"没空间"**，但那时也没有更好的依据。
    const bool dbg = !paths::env("CHANGJI_DEBUG_VRAM").empty();
    const FreeVramProbe probe = free_vram_;
    if (dbg) {
        std::fprintf(stderr,
                     "[vram] make_room slot=%s used=%.1fG need=%.1fG budget=%.1fG probe=%s\n",
                     to_string(keep), used / 1073741824.0, need / 1073741824.0,
                     budget_ / 1073741824.0, probe ? "有" : "没装");
    }
    // 这个槽跑起来到底要多少：实测优先，没量过才退回估算。
    // 估算那条只会让我们更保守——多卸一次，不会 OOM。
    const Entry* self = find(keep);
    std::size_t live = need;
    if (const auto it = measured_.find(keep);
        it != measured_.end() && it->second > 0) {
        live = it->second;
    } else if (self && self->spec.live_vram) {
        // 每次现问：模型可能已经被换过了。见 SlotSpec::live_vram。
        const std::size_t got = self->spec.live_vram();
        if (got > 0) live = got;
    }

    // 先问卡。
    std::optional<std::size_t> free_bytes;
    if (probe) {
        if (const auto free_gb = probe(); free_gb.has_value()) {
            free_bytes = static_cast<std::size_t>(
                *free_gb * 1024.0 * 1024.0 * 1024.0);
        }
    }
    if (dbg) {
        std::fprintf(stderr, "[vram]   探到空闲=%s\n",
                     free_bytes ? (std::to_string(*free_bytes / 1073741824.0)
                                   + "G").c_str()
                                : "问不到");
    }

    // **问不到就用量到的推算。**
    //
    // nvidia-smi 不是永远问得到：free_vram_gb 是 fork + exec 去跑它的，
    // 而这个进程初始化 CUDA 之后映射着十几 GB，在这种进程里 fork 本来就是
    // NVIDIA 明确不支持的做法。以前问不到就一路走到驱逐，于是
    // "显存够就不清理"在真机上等于从来没生效过。
    //
    // 推算只用**已经量到的**数，不猜：装着的槽里只要有一个没量过，
    // 就放弃推算、回到保守那条去卸。这样永远不会比事实更乐观。
    if (!free_bytes && total_vram_ > 0) {
        std::size_t others = 0;
        bool all_known = true;
        for (const Entry& e : entries_) {
            if (!e.is_loaded || e.spec.slot == keep) continue;
            const auto it = measured_.find(e.spec.slot);
            if (it == measured_.end() || it->second == 0) {
                all_known = false;
                break;
            }
            others += it->second;
        }
        if (all_known && others < total_vram_) {
            free_bytes = total_vram_ - others;
            if (dbg) {
                std::fprintf(stderr,
                             "[vram]   问不到，按量到的推算空闲=%.1fG"
                             "（总量 %.1fG − 别人占的 %.1fG）\n",
                             *free_bytes / 1073741824.0,
                             total_vram_ / 1073741824.0,
                             others / 1073741824.0);
            }
        } else if (dbg) {
            std::fprintf(stderr,
                         "[vram]   问不到，且有槽没量过，推算不了 -> 保守驱逐\n");
        }
    }

    if (free_bytes) {
        if (dbg) {
            std::fprintf(stderr, "[vram]   老实数=%.1fG 空闲=%.1fG -> %s\n",
                         live / 1073741824.0, *free_bytes / 1073741824.0,
                         live <= *free_bytes ? "够，不卸" : "不够，要卸");
        }
        if (live <= *free_bytes) return true;
    }

    // 候选：已加载、没被借用、不是要保住的那个。
    //
    // 被借用的一律不动——正在用的模型被抽走，表现是段错误，
    // 而不是一个能读的报错。宁可这次加载失败。
    std::vector<Entry*> cands;
    for (Entry& e : entries_) {
        if (!e.is_loaded || e.leases > 0 || e.spec.slot == keep) continue;
        cands.push_back(&e);
    }

    // 先按驱逐优先级（小的先走），同优先级按最久没用的先走。
    std::sort(cands.begin(), cands.end(), [](const Entry* a, const Entry* b) {
        if (a->spec.evict_priority != b->spec.evict_priority) {
            return a->spec.evict_priority < b->spec.evict_priority;
        }
        return a->last_used < b->last_used;
    });

    for (Entry* e : cands) {
        if (used + need <= budget_) break;
        const std::size_t freed = e->spec.vram_estimate;
        do_unload(*e);
        used -= std::min(used, freed);
    }
    return used + need <= budget_;
}

Lease Scheduler::acquire(Slot slot) {
    std::unique_lock lk(mu_);
    Entry* e = find(slot);
    if (!e) {
        throw std::runtime_error(std::string("槽 ") + to_string(slot) + " 还没注册");
    }

    e->last_used = ++clock_;

    if (e->is_loaded) {
        ++e->leases;
        return Lease(this, slot);
    }

    if (!make_room(e->spec.vram_estimate, slot)) {
        throw std::runtime_error(out_of_vram_message(slot));
    }

    // 加载可能很慢（要读几个 GB），不能一直占着锁。
    // 先标成已加载再解锁，防止另一个线程同时也去加载同一个槽。
    e->is_loaded = true;
    ++e->leases;
    auto load_fn = e->spec.load;
    lk.unlock();

    if (load_fn) {
        try {
            load_fn();
        } catch (...) {
            // 加载失败，把账退回去。**不自动重试也不自动加载回被驱逐的槽**——
            // 那会把一次失败变成一串连锁加载，而且日志里看不出源头。
            std::lock_guard lg(mu_);
            Entry* again = find(slot);
            if (again) {
                again->is_loaded = false;
                if (again->leases > 0) --again->leases;
            }
            throw;
        }
    }
    return Lease(this, slot);
}

void Scheduler::give_back(Slot slot) {
    std::lock_guard lg(mu_);
    Entry* e = find(slot);
    if (!e || e->leases <= 0) return;
    --e->leases;
    // 用完就放的槽，最后一个借用还回来时立刻卸载。
    // 留着纯粹是占地方——它按定义不会再被用了。
    if (e->leases == 0 && e->spec.residency == Residency::Ephemeral) {
        do_unload(*e);
    }
}

bool Scheduler::evict(Slot slot) {
    std::lock_guard lg(mu_);
    Entry* e = find(slot);
    if (!e) return false;
    if (e->leases > 0) return false;  // 正在用，不强卸
    do_unload(*e);
    return true;
}

void Scheduler::evict_all() {
    std::lock_guard lg(mu_);
    for (Entry& e : entries_) {
        if (e.leases == 0) do_unload(e);
    }
}

bool Scheduler::loaded(Slot slot) const {
    std::lock_guard lg(mu_);
    const Entry* e = find(slot);
    return e && e->is_loaded;
}

int Scheduler::lease_count(Slot slot) const {
    std::lock_guard lg(mu_);
    const Entry* e = find(slot);
    return e ? e->leases : 0;
}

std::size_t Scheduler::resident_bytes() const {
    std::lock_guard lg(mu_);
    std::size_t used = 0;
    for (const Entry& e : entries_) {
        if (e.is_loaded) used += e.spec.vram_estimate;
    }
    return used;
}

std::vector<Slot> Scheduler::loaded_slots() const {
    std::lock_guard lg(mu_);
    std::vector<const Entry*> loaded;
    for (const Entry& e : entries_) {
        if (e.is_loaded) loaded.push_back(&e);
    }
    std::sort(loaded.begin(), loaded.end(), [](const Entry* a, const Entry* b) {
        if (a->spec.evict_priority != b->spec.evict_priority) {
            return a->spec.evict_priority < b->spec.evict_priority;
        }
        return a->last_used < b->last_used;
    });
    std::vector<Slot> out;
    out.reserve(loaded.size());
    for (const Entry* e : loaded) out.push_back(e->spec.slot);
    return out;
}

Scheduler& scheduler() {
    static Scheduler s;
    return s;
}

}  // namespace changji::infer
