#include "gates/checks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::gates {

namespace {

/// 保留 n 位小数。**对齐 Python 的 round()，也就是银行家舍入**——
/// 这些数会进 metrics 然后被对拍逐个比，差一个末位就是不通过。
double round_to(double v, int digits) {
    const double f = std::pow(10.0, digits);
    return std::nearbyint(v * f) / f;
}

std::string fmt(const char* spec, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), spec, v);
    return buf;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

GateResult fail(const std::string& shot_id, Verdict v, const std::string& gate,
                std::vector<std::string> reasons,
                std::map<std::string, double> metrics = {}) {
    GateResult r;
    r.shot_id = shot_id;
    r.verdict = v;
    r.gate = gate;
    r.reasons = std::move(reasons);
    r.metrics = std::move(metrics);
    return r;
}

}  // namespace

const char* to_string(Verdict v) {
    switch (v) {
        case Verdict::Pass:     return "pass";
        case Verdict::Retry:    return "retry";
        case Verdict::Regress:  return "regress";
        case Verdict::Fallback: return "fallback";
    }
    return "?";
}

std::string GateResult::describe() const {
    if (ok()) return shot_id + " 通过" + gate;
    return shot_id + " 未过" + gate + "：" + join(reasons, "；");
}

std::string position_name(int index, int total) {
    if (total <= 1) return "画面";
    if (index == 0) return "片头";
    if (index == total - 1) return "片尾";
    return "片中";
}

GateResult gate_video(const models::Shot& shot, const fs::path& video_path,
                      const media::FFmpeg& ff, const config::GateConfig& cfg,
                      std::optional<double> expected_duration_s,
                      std::optional<std::pair<int, int>> expected_size,
                      const std::string& gate_name) {
    std::vector<std::string> reasons;
    std::map<std::string, double> metrics;

    std::error_code ec;
    if (!fs::is_regular_file(video_path, ec)) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, {"视频文件不存在"});
    }
    // 1KB 以下不可能是一段视频。先看大小再去 probe，省掉一次子进程——
    // 而且 ffprobe 对一个零字节文件的报错很难懂。
    if (fs::file_size(video_path, ec) < 1024) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, {"视频文件几乎是空的"});
    }

    media::MediaInfo info;
    try {
        info = ff.probe(video_path);
    } catch (const media::FFmpegError& e) {
        return fail(shot.shot_id, Verdict::Retry, gate_name,
                    {std::string("视频文件读不出来：") + e.what()});
    }

    if (!info.has_video) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, {"文件里没有视频轨"});
    }

    metrics["duration_s"] = info.duration_s;
    metrics["width"] = static_cast<double>(info.width);
    metrics["height"] = static_cast<double>(info.height);

    // 时长。**差太多是 REGRESS 不是 RETRY**：帧数算错了，换个种子重跑
    // 还是一样的帧数。要退回上一阶段重新算。
    if (expected_duration_s.has_value() && *expected_duration_s != 0.0) {
        const double drift = std::abs(info.duration_s - *expected_duration_s);
        metrics["duration_drift_s"] = round_to(drift, 3);
        if (drift > std::max(0.5, *expected_duration_s * 0.2)) {
            return fail(shot.shot_id, Verdict::Regress, gate_name,
                        {"时长 " + fmt("%.2f", info.duration_s) + " 秒，期望 " +
                         fmt("%.2f", *expected_duration_s) + " 秒，差 " +
                         fmt("%.2f", drift) + " 秒，多半是帧数算错了"},
                        metrics);
        }
    }

    // 分辨率不符说明档位参数没生效，同样重跑无用。
    if (expected_size.has_value() &&
        (info.width != expected_size->first || info.height != expected_size->second)) {
        return fail(shot.shot_id, Verdict::Regress, gate_name,
                    {"分辨率 " + std::to_string(info.width) + "x" +
                     std::to_string(info.height) + "，期望 " +
                     std::to_string(expected_size->first) + "x" +
                     std::to_string(expected_size->second) + "，档位参数没生效"},
                    metrics);
    }

    // 画面内容。**多点取样**，只看一帧会漏掉中途崩坏。
    std::vector<media::PixelStats> samples;
    try {
        samples = ff.sample_pixel_stats(video_path, 3);
    } catch (const media::FFmpegError& e) {
        return fail(shot.shot_id, Verdict::Retry, gate_name,
                    {std::string("读不出画面统计：") + e.what()}, metrics);
    }
    if (samples.empty()) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, {"取不到任何画面"},
                    metrics);
    }

    std::vector<double> spreads, means;
    for (const auto& s : samples) {
        spreads.push_back(s.spread);
        means.push_back(s.mean);
    }
    const double spread_min = *std::min_element(spreads.begin(), spreads.end());
    double mean_sum = 0.0;
    for (const double m : means) mean_sum += m;
    metrics["spread_min"] = round_to(spread_min, 2);
    metrics["mean_avg"] = round_to(mean_sum / static_cast<double>(means.size()), 2);

    std::vector<std::string> blank_where;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].looks_blank()) {
            blank_where.push_back(
                position_name(static_cast<int>(i), static_cast<int>(samples.size())));
        }
    }
    if (!blank_where.empty()) {
        reasons.push_back(join(blank_where, "、") + "的画面近乎纯色，展布只有 " +
                          fmt("%.1f", spread_min));
    }

    std::vector<std::string> clipped_where;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].looks_clipped()) {
            clipped_where.push_back(
                position_name(static_cast<int>(i), static_cast<int>(samples.size())));
        }
    }
    if (!clipped_where.empty()) {
        reasons.push_back(join(clipped_where, "、") + "的画面整体过暗或过曝");
    }

    // 已经报过"近乎纯色"就不再报"细节偏少"——同一件事说两遍，
    // 而用户要从两句里判断是不是两个问题。
    if (spread_min < cfg.min_pixel_std && blank_where.empty()) {
        reasons.push_back("画面细节偏少，展布 " + fmt("%.1f", spread_min) +
                          " 低于阈值 " + fmt("%.0f", cfg.min_pixel_std));
    }

    // 相邻取样点之间画面差异过大，说明中途崩坏。
    if (means.size() >= 2) {
        const double swing = *std::max_element(means.begin(), means.end()) -
                             *std::min_element(means.begin(), means.end());
        metrics["mean_swing"] = round_to(swing, 2);
        if (swing > 60.0) {
            reasons.push_back("片中亮度剧烈跳变 " + fmt("%.0f", swing) +
                              "，可能中途崩坏");
        }
    }

    if (!reasons.empty()) {
        return fail(shot.shot_id, Verdict::Retry, gate_name, reasons, metrics);
    }
    GateResult r;
    r.shot_id = shot.shot_id;
    r.verdict = Verdict::Pass;
    r.gate = gate_name;
    r.metrics = metrics;
    return r;
}

GateResult gate_audio_sync(const models::Shot& shot, const fs::path& video_path,
                           const media::FFmpeg& ff,
                           const config::GateConfig& cfg) {
    const std::string gate = "音画闸门";
    const auto speech = shot.total_dialogue_duration_s();
    if (!speech.has_value()) {
        // 有台词但没有配音时长，说明配音阶段没跑完。重跑视频没用。
        return fail(shot.shot_id, Verdict::Regress, gate,
                    {"有台词但没有配音时长，配音阶段没跑完"});
    }
    if (*speech == 0.0) {
        GateResult r;
        r.shot_id = shot.shot_id;
        r.verdict = Verdict::Pass;
        r.gate = gate;
        return r;
    }

    media::MediaInfo info;
    try {
        info = ff.probe(video_path);
    } catch (const media::FFmpegError& e) {
        return fail(shot.shot_id, Verdict::Retry, gate,
                    {std::string("读不出视频时长：") + e.what()});
    }

    const double slack = info.duration_s - *speech;
    std::map<std::string, double> metrics{
        {"speech_s", round_to(*speech, 3)},
        {"video_s", round_to(info.duration_s, 3)},
        {"slack_s", round_to(slack, 3)},
    };

    if (slack < -cfg.max_audio_drift_s) {
        return fail(shot.shot_id, Verdict::Regress, gate,
                    {"配音 " + fmt("%.2f", *speech) + " 秒装不进 " +
                     fmt("%.2f", info.duration_s) + " 秒的镜头，超出 " +
                     fmt("%.2f", -slack) + " 秒。需要重新锁定时长后重出这一镜"},
                    metrics);
    }
    GateResult r;
    r.shot_id = shot.shot_id;
    r.verdict = Verdict::Pass;
    r.gate = gate;
    r.metrics = metrics;
    return r;
}

EpisodeGateResult gate_episode(const fs::path& video_path,
                               const media::FFmpeg& ff,
                               const config::GateConfig& cfg,
                               std::optional<double> expected_duration_s) {
    EpisodeGateResult out;
    std::error_code ec;
    if (!fs::is_regular_file(video_path, ec)) {
        return {Verdict::Regress, {"成片文件不存在"}, {}};
    }

    media::MediaInfo info;
    try {
        info = ff.probe(video_path);
    } catch (const media::FFmpegError& e) {
        return {Verdict::Regress, {std::string("成片读不出来：") + e.what()}, {}};
    }

    out.metrics["duration_s"] = round_to(info.duration_s, 2);
    if (!info.has_video) out.reasons.push_back("成片里没有视频轨");
    if (!info.has_audio) out.reasons.push_back("成片里没有音轨");

    if (expected_duration_s.has_value() && *expected_duration_s != 0.0) {
        const double drift = std::abs(info.duration_s - *expected_duration_s);
        out.metrics["duration_drift_s"] = round_to(drift, 2);
        // 整集的容差比单镜宽（5% 对 20%，下限 2 秒对 0.5 秒）：
        // 几十镜拼起来，每镜零点几秒的误差累积是正常的。
        if (drift > std::max(2.0, *expected_duration_s * 0.05)) {
            out.reasons.push_back("成片时长 " + fmt("%.1f", info.duration_s) +
                                  " 秒，期望 " + fmt("%.1f", *expected_duration_s) +
                                  " 秒");
        }
    }

    if (info.has_audio) {
        try {
            const auto loud = ff.measure_loudness(video_path);
            const auto get = [&loud](const char* k) {
                const auto it = loud.find(k);
                return it == loud.end() ? 0.0 : it->second;
            };
            const double measured = get("input_i");
            const double peak = get("input_tp");
            out.metrics["lufs"] = round_to(measured, 2);
            out.metrics["true_peak_db"] = round_to(peak, 2);
            if (std::abs(measured - cfg.target_lufs) > 2.0) {
                out.reasons.push_back("响度 " + fmt("%.1f", measured) +
                                      " LUFS，目标 " + fmt("%.0f", cfg.target_lufs) +
                                      "，平台可能会自动调整");
            }
            if (peak > cfg.max_true_peak_db + 0.5) {
                out.reasons.push_back("真峰值 " + fmt("%.1f", peak) +
                                      " dBTP 偏高，可能削波");
            }
        } catch (const media::FFmpegError&) {
            out.reasons.push_back("测不出响度");
        }
    }

    out.verdict = out.reasons.empty() ? Verdict::Pass : Verdict::Retry;
    return out;
}

Verdict decide_next(const GateResult& result, const models::Shot& shot,
                    const config::GateConfig& cfg) {
    if (result.ok()) return Verdict::Pass;
    // REGRESS 不看重试次数：重跑也没用，问题在上一阶段。
    if (result.verdict == Verdict::Regress) return Verdict::Regress;
    if (shot.attempts + 1 >= cfg.max_attempts_per_shot) {
        // **降级而不是停下来。** 无人值守跑一晚上，为一镜停住等于
        // 整晚白熬；降级成静帧加运镜至少整集能出片，问题记录下来事后查。
        return cfg.fallback_on_exhausted ? Verdict::Fallback : Verdict::Regress;
    }
    return Verdict::Retry;
}

std::string summarize(const std::vector<GateResult>& results) {
    if (results.empty()) return "没有需要检查的镜头";
    int passed = 0;
    for (const auto& r : results) {
        if (r.ok()) ++passed;
    }
    std::string out = "闸门检查 " + std::to_string(results.size()) +
                      " 个镜头，通过 " + std::to_string(passed) + " 个";
    // 只列没过的。全过时人不需要读四十行"通过"。
    for (const auto& r : results) {
        if (!r.ok()) out += "\n  " + r.describe();
    }
    return out;
}

}  // namespace changji::gates
