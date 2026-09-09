#include "config/runtime.hpp"

namespace changji::config {

Settings Runtime::snapshot() const {
    std::lock_guard lg(mu_);
    return settings_;
}

void Runtime::replace(Settings s) {
    std::lock_guard lg(mu_);
    settings_ = std::move(s);
}

void Runtime::set_tier_override(models::Tier tier,
                                const models::TierSpec& spec) {
    std::lock_guard lg(mu_);
    tier_overrides_[tier] = spec;
}

void Runtime::clear_tier_overrides() {
    std::lock_guard lg(mu_);
    tier_overrides_.clear();
}

models::HardwareProfile Runtime::profile() const {
    std::optional<double> vram;
    std::map<models::Tier, models::TierSpec> overrides;
    TiersConfig from_file;
    {
        std::lock_guard lg(mu_);
        vram = settings_.vram_gb_override;
        overrides = tier_overrides_;
        from_file = settings_.tiers;
    }
    // detect() 会跑 nvidia-smi，别拿着锁做。
    models::HardwareProfile p = models::HardwareProfile::detect(vram);

    // **配置文件里的 [tiers] 先落，进程内的覆盖后落。**
    // 顺序不能反：进程内那份是用户刚在设置页改的，比文件新。
    // 0 表示"这一项没填"，照旧用按显存推出来的值。
    const auto apply = [&p](models::Tier t, int w, int h, int st) {
        const auto it = p.tiers.find(t);
        if (it == p.tiers.end()) return;
        if (w > 0) it->second.width = w;
        if (h > 0) it->second.height = h;
        if (st > 0) it->second.steps = st;
    };
    apply(models::Tier::DRAFT, from_file.draft_width, from_file.draft_height,
          from_file.draft_steps);
    apply(models::Tier::FINAL, from_file.final_width, from_file.final_height,
          from_file.final_steps);

    for (const auto& kv : overrides) {
        const auto it = p.tiers.find(kv.first);
        if (it == p.tiers.end()) continue;
        // 只覆盖三个字段，measured_seconds 保留——那是标定出来的，
        // 换个分辨率不代表标定结果作废，而重新标定要跑一遍真实渲染。
        it->second.width = kv.second.width;
        it->second.height = kv.second.height;
        it->second.steps = kv.second.steps;
    }
    return p;
}

Runtime& runtime() {
    static Runtime r;
    return r;
}

}  // namespace changji::config
