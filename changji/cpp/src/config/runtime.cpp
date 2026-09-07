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
    {
        std::lock_guard lg(mu_);
        vram = settings_.vram_gb_override;
        overrides = tier_overrides_;
    }
    // detect() 会跑 nvidia-smi，别拿着锁做。
    models::HardwareProfile p = models::HardwareProfile::detect(vram);
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
