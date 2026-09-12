#include "config/runtime.hpp"

#include "stages/limits.hpp"

namespace {

/// 这张卡有多少显存。**探测一次就记住**：探显卡要跑 nvidia-smi，而
/// Runtime::replace 在一次运行里会被调好几次（起服务、改设置、换机器）。
/// 一次运行里显卡不会变。
double detected_vram_gb() {
    static const double v = [] {
        const auto p = changji::models::HardwareProfile::detect(std::nullopt);
        return p.vram_gb;
    }();
    return v;
}

}  // namespace

namespace changji::config {

Settings Runtime::snapshot() const {
    std::lock_guard lg(mu_);
    return settings_;
}

void Runtime::replace(Settings s) {
    // 视频模型的限制跟着配置走。**放在这一处而不是每个调用方**：能改配置的
    // 入口有五个（起服务、/api/settings、/api/connections、初始化页……），
    // 漏掉任何一个都是"配置改了但分镜还按老上限排"，而且不报错。
    //
    // **先按模型自己认，配置里填了才覆盖。** 换模型时人改的是 [models].video
    // 那一行，不会想起来还有帧数格子要跟着改——所以默认让它自己认，
    // 那三项留 0 就是"你看着办"。
    stages::VideoLimits limits = stages::guess_video_limits(
        s.models.video, !s.models.video_llm.empty());
    // 再按这张卡夹一道：**模型能出 15 秒不等于这张卡能出 15 秒**。
    // 探测不到显卡（单元测试、没装驱动）时 cap_by_vram 原样返回，不放开。
    //
    // 常驻权重按"全放内存"算（0），也就是显存全给计算缓冲——5090 上
    // 1280×704 就是这么配的（weights = "cpu"）。真常驻一部分权重时可用的
    // 更少，但那种配置本来也跑不了长镜头，夹得更短没坏处。
    limits = stages::cap_by_vram(limits, detected_vram_gb(), 0.0);
    if (s.models.video_max_frames > 0) limits.max_frames = s.models.video_max_frames;
    if (s.models.video_frame_step > 0) limits.frame_step = s.models.video_frame_step;
    if (s.models.video_frame_base >= 0 && s.models.video_frame_step > 0) {
        limits.frame_base = s.models.video_frame_base;
    }
    stages::set_video_limits(limits);

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
