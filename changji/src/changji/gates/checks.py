"""质量闸门。

无人值守模式下，闸门是唯一阻止废片流入成片的机制。

三条纪律：
判定必须程序可算，不能依赖人看。
失败必须给出可操作的下一步，而不只是说不合格。
绝不静默放行，重试超限就明确降级并留下记录。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from ..assembly.ffmpeg import FFmpeg, FFmpegError, MediaInfo, PixelStats
from ..config import GateConfig
from ..models.shot import Shot


class Verdict(str, Enum):
    PASS = "pass"
    RETRY = "retry"      # 换种子重跑可能就好了
    REGRESS = "regress"  # 重跑也没用，得退回上一阶段
    FALLBACK = "fallback"  # 重试超限，降级处理


@dataclass
class GateResult:
    """一次闸门判定的结果。"""

    shot_id: str
    verdict: Verdict
    gate: str
    reasons: list[str] = field(default_factory=list)
    metrics: dict[str, float] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return self.verdict is Verdict.PASS

    def describe(self) -> str:
        if self.ok:
            return f"{self.shot_id} 通过{self.gate}"
        return f"{self.shot_id} 未过{self.gate}：" + "；".join(self.reasons)


async def gate_video(
    shot: Shot,
    video_path: Path,
    ff: FFmpeg,
    config: GateConfig,
    expected_duration_s: float | None = None,
    expected_size: tuple[int, int] | None = None,
    gate_name: str = "画面闸门",
) -> GateResult:
    """检查一个镜头的视频。

    草稿档和成片档用同一套检查，只是期望值不同。
    """
    reasons: list[str] = []
    metrics: dict[str, float] = {}

    if not video_path.is_file():
        return GateResult(shot.shot_id, Verdict.RETRY, gate_name, ["视频文件不存在"])
    if video_path.stat().st_size < 1024:
        return GateResult(shot.shot_id, Verdict.RETRY, gate_name, ["视频文件几乎是空的"])

    try:
        info: MediaInfo = await ff.probe(video_path)
    except FFmpegError as exc:
        return GateResult(shot.shot_id, Verdict.RETRY, gate_name,
                          [f"视频文件读不出来：{exc}"])

    if not info.has_video:
        return GateResult(shot.shot_id, Verdict.RETRY, gate_name, ["文件里没有视频轨"])

    metrics["duration_s"] = info.duration_s
    metrics["width"] = float(info.width)
    metrics["height"] = float(info.height)

    # 时长。差太多说明帧数算错了，重跑也是一样，得退回上一阶段。
    if expected_duration_s:
        drift = abs(info.duration_s - expected_duration_s)
        metrics["duration_drift_s"] = round(drift, 3)
        if drift > max(0.5, expected_duration_s * 0.2):
            return GateResult(
                shot.shot_id, Verdict.REGRESS, gate_name,
                [f"时长 {info.duration_s:.2f} 秒，期望 {expected_duration_s:.2f} 秒，"
                 f"差 {drift:.2f} 秒，多半是帧数算错了"],
                metrics,
            )

    # 分辨率。不符说明档位参数没生效。
    if expected_size and (info.width, info.height) != expected_size:
        return GateResult(
            shot.shot_id, Verdict.REGRESS, gate_name,
            [f"分辨率 {info.width}x{info.height}，期望 "
             f"{expected_size[0]}x{expected_size[1]}，档位参数没生效"],
            metrics,
        )

    # 画面内容。多点取样，只看一帧会漏掉中途崩坏。
    try:
        samples: list[PixelStats] = await ff.sample_pixel_stats(video_path, samples=3)
    except FFmpegError as exc:
        return GateResult(shot.shot_id, Verdict.RETRY, gate_name,
                          [f"读不出画面统计：{exc}"], metrics)

    if not samples:
        return GateResult(shot.shot_id, Verdict.RETRY, gate_name,
                          ["取不到任何画面"], metrics)

    spreads = [s.spread for s in samples]
    means = [s.mean for s in samples]
    metrics["spread_min"] = round(min(spreads), 2)
    metrics["mean_avg"] = round(sum(means) / len(means), 2)

    blank = [i for i, s in enumerate(samples) if s.looks_blank]
    if blank:
        where = "、".join(_position_name(i, len(samples)) for i in blank)
        reasons.append(f"{where}的画面近乎纯色，展布只有 {min(spreads):.1f}")

    clipped = [i for i, s in enumerate(samples) if s.looks_clipped]
    if clipped:
        where = "、".join(_position_name(i, len(samples)) for i in clipped)
        reasons.append(f"{where}的画面整体过暗或过曝")

    if min(spreads) < config.min_pixel_std:
        if not blank:
            reasons.append(f"画面细节偏少，展布 {min(spreads):.1f} "
                           f"低于阈值 {config.min_pixel_std:.0f}")

    # 相邻取样点之间画面差异过大，说明中途崩坏
    if len(means) >= 2:
        swing = max(means) - min(means)
        metrics["mean_swing"] = round(swing, 2)
        if swing > 60:
            reasons.append(f"片中亮度剧烈跳变 {swing:.0f}，可能中途崩坏")

    if reasons:
        return GateResult(shot.shot_id, Verdict.RETRY, gate_name, reasons, metrics)
    return GateResult(shot.shot_id, Verdict.PASS, gate_name, [], metrics)


def _position_name(index: int, total: int) -> str:
    if total <= 1:
        return "画面"
    if index == 0:
        return "片头"
    if index == total - 1:
        return "片尾"
    return "片中"


async def gate_audio_sync(
    shot: Shot,
    video_path: Path,
    ff: FFmpeg,
    config: GateConfig,
) -> GateResult:
    """检查镜头时长能不能装下配音。

    这个检查放在装配之前。装完再发现装不下就得重做整集。
    """
    speech = shot.total_dialogue_duration_s
    if speech is None:
        return GateResult(shot.shot_id, Verdict.REGRESS, "音画闸门",
                          ["有台词但没有配音时长，配音阶段没跑完"])
    if speech == 0:
        return GateResult(shot.shot_id, Verdict.PASS, "音画闸门")

    try:
        info = await ff.probe(video_path)
    except FFmpegError as exc:
        return GateResult(shot.shot_id, Verdict.RETRY, "音画闸门",
                          [f"读不出视频时长：{exc}"])

    slack = info.duration_s - speech
    metrics = {"speech_s": round(speech, 3), "video_s": round(info.duration_s, 3),
               "slack_s": round(slack, 3)}

    if slack < -config.max_audio_drift_s:
        return GateResult(
            shot.shot_id, Verdict.REGRESS, "音画闸门",
            [f"配音 {speech:.2f} 秒装不进 {info.duration_s:.2f} 秒的镜头，"
             f"超出 {-slack:.2f} 秒。需要重新锁定时长后重出这一镜"],
            metrics,
        )
    return GateResult(shot.shot_id, Verdict.PASS, "音画闸门", [], metrics)


@dataclass
class EpisodeGateResult:
    """整集装配后的检查。"""

    verdict: Verdict
    reasons: list[str] = field(default_factory=list)
    metrics: dict[str, float] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return self.verdict is Verdict.PASS


async def gate_episode(
    video_path: Path,
    ff: FFmpeg,
    config: GateConfig,
    expected_duration_s: float | None = None,
) -> EpisodeGateResult:
    """成片检查：时长、响度、音轨。"""
    reasons: list[str] = []
    metrics: dict[str, float] = {}

    if not video_path.is_file():
        return EpisodeGateResult(Verdict.REGRESS, ["成片文件不存在"])

    try:
        info = await ff.probe(video_path)
    except FFmpegError as exc:
        return EpisodeGateResult(Verdict.REGRESS, [f"成片读不出来：{exc}"])

    metrics["duration_s"] = round(info.duration_s, 2)
    if not info.has_video:
        reasons.append("成片里没有视频轨")
    if not info.has_audio:
        reasons.append("成片里没有音轨")

    if expected_duration_s:
        drift = abs(info.duration_s - expected_duration_s)
        metrics["duration_drift_s"] = round(drift, 2)
        if drift > max(2.0, expected_duration_s * 0.05):
            reasons.append(
                f"成片时长 {info.duration_s:.1f} 秒，期望 {expected_duration_s:.1f} 秒"
            )

    if info.has_audio:
        try:
            loud = await ff.measure_loudness(video_path)
            measured = loud.get("input_i", 0.0)
            peak = loud.get("input_tp", 0.0)
            metrics["lufs"] = round(measured, 2)
            metrics["true_peak_db"] = round(peak, 2)
            if abs(measured - config.target_lufs) > 2.0:
                reasons.append(
                    f"响度 {measured:.1f} LUFS，目标 {config.target_lufs:.0f}，"
                    f"平台可能会自动调整"
                )
            if peak > config.max_true_peak_db + 0.5:
                reasons.append(f"真峰值 {peak:.1f} dBTP 偏高，可能削波")
        except FFmpegError:
            reasons.append("测不出响度")

    if reasons:
        return EpisodeGateResult(Verdict.RETRY, reasons, metrics)
    return EpisodeGateResult(Verdict.PASS, [], metrics)


def decide_next(result: GateResult, shot: Shot, config: GateConfig) -> Verdict:
    """闸门失败后决定怎么办。

    这是无人值守能不能不卡死的关键。重试超限时降级而不是停下来，
    保证整集能出片，同时把问题记录下来供人事后查。
    """
    if result.ok:
        return Verdict.PASS
    if result.verdict is Verdict.REGRESS:
        return Verdict.REGRESS
    if shot.attempts + 1 >= config.max_attempts_per_shot:
        return Verdict.FALLBACK if config.fallback_on_exhausted else Verdict.REGRESS
    return Verdict.RETRY


def summarize(results: list[GateResult]) -> str:
    """闸门结果概览。无人值守时这是人唯一要看的东西。"""
    if not results:
        return "没有需要检查的镜头"
    passed = [r for r in results if r.ok]
    lines = [f"闸门检查 {len(results)} 个镜头，通过 {len(passed)} 个"]
    for r in results:
        if not r.ok:
            lines.append(f"  {r.describe()}")
    return "\n".join(lines)
