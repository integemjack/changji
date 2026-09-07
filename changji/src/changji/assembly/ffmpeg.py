"""FFmpeg 封装。

统一在这里调用，好处是外部依赖的检查只做一次，错误信息也能统一翻译成人话。

不用 MoviePy。它最新版停在 2025 年 5 月，一年多没有新版本，
项目说明里挂着招募维护者的告示，不适合压产线。
"""

from __future__ import annotations

import asyncio
import json
import shutil
from dataclasses import dataclass
from pathlib import Path


class FFmpegError(RuntimeError):
    pass


class FFmpegMissing(FFmpegError):
    """找不到 ffmpeg。装配环节的硬依赖。"""


@dataclass(frozen=True)
class MediaInfo:
    """一个媒体文件的关键信息。"""

    path: Path
    duration_s: float
    width: int = 0
    height: int = 0
    fps: float = 0.0
    frames: int = 0
    has_video: bool = False
    has_audio: bool = False
    pix_fmt: str = ""
    sar: str = ""

    @property
    def resolution(self) -> tuple[int, int]:
        return self.width, self.height


@dataclass(frozen=True)
class PixelStats:
    """一帧的亮度统计。用来判断画面是不是纯色或全黑。

    展布用 YHIGH 减 YLOW，也就是第 90 和第 10 百分位之差，而不是极差。
    极差会被单个亮点或暗点带偏：一张几乎全黑但有一个高光点的废图，
    极差能到 250，看起来很正常。百分位之差不吃这一套。
    """

    mean: float
    spread: float
    minimum: float
    maximum: float
    low: float = 0.0
    high: float = 0.0

    @property
    def looks_blank(self) -> bool:
        """近乎纯色。生成失败最常见的表现。

        阈值取 8。实测正常画面的展布在 40 以上，
        纯色或近乎纯色的画面在个位数。
        """
        return self.spread < 8.0

    @property
    def looks_clipped(self) -> bool:
        """整体过曝或全黑。"""
        return self.mean < 6.0 or self.mean > 249.0


class FFmpeg:
    """ffmpeg 与 ffprobe 的异步封装。"""

    def __init__(self, ffmpeg: str = "ffmpeg", ffprobe: str = "ffprobe") -> None:
        self.ffmpeg = ffmpeg
        self.ffprobe = ffprobe

    # ---- 可用性 ----

    def check(self) -> None:
        """启动时体检。缺了要在跑之前就说清楚，而不是跑到装配才炸。"""
        missing = [
            name for name, exe in (("ffmpeg", self.ffmpeg), ("ffprobe", self.ffprobe))
            if shutil.which(exe) is None
        ]
        if missing:
            raise FFmpegMissing(
                f"找不到 {'和'.join(missing)}。\n"
                f"Windows 可以用 winget install Gyan.FFmpeg，\n"
                f"macOS 用 brew install ffmpeg，\n"
                f"Linux 用包管理器安装。\n"
                f"装好后如果仍然找不到，在配置里填 assembly.ffmpeg_path 指定完整路径。"
            )

    def available(self) -> bool:
        try:
            self.check()
            return True
        except FFmpegMissing:
            return False

    # ---- 执行 ----

    async def _run(self, exe: str, args: list[str], timeout_s: float = 600.0) -> str:
        try:
            proc = await asyncio.create_subprocess_exec(
                exe, *args,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
        except FileNotFoundError as exc:
            raise FFmpegMissing(f"找不到 {exe}") from exc
        try:
            out, err = await asyncio.wait_for(proc.communicate(), timeout=timeout_s)
        except asyncio.TimeoutError:
            proc.kill()
            raise FFmpegError(f"{exe} 执行超时（{timeout_s:.0f} 秒）") from None
        if proc.returncode != 0:
            tail = err.decode("utf-8", "replace").strip().splitlines()
            raise FFmpegError(
                f"{exe} 执行失败（退出码 {proc.returncode}）：\n"
                + "\n".join(tail[-8:])
            )
        return out.decode("utf-8", "replace")

    async def run_ffmpeg(self, args: list[str], timeout_s: float = 1800.0) -> str:
        return await self._run(self.ffmpeg, ["-hide_banner", "-nostdin", "-y", *args],
                               timeout_s)

    async def run_ffprobe(self, args: list[str], timeout_s: float = 120.0) -> str:
        return await self._run(self.ffprobe, ["-hide_banner", *args], timeout_s)

    # ---- 探测 ----

    async def probe(self, path: str | Path) -> MediaInfo:
        p = Path(path)
        if not p.is_file():
            raise FFmpegError(f"文件不存在：{p}")
        raw = await self.run_ffprobe([
            "-v", "error", "-print_format", "json",
            "-show_format", "-show_streams", str(p),
        ])
        try:
            data = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise FFmpegError(f"ffprobe 输出无法解析：{p}") from exc

        streams = data.get("streams", [])
        video = next((s for s in streams if s.get("codec_type") == "video"), None)
        audio = next((s for s in streams if s.get("codec_type") == "audio"), None)

        duration = _to_float(data.get("format", {}).get("duration"))
        if duration == 0.0 and video:
            duration = _to_float(video.get("duration"))

        info = MediaInfo(
            path=p,
            duration_s=duration,
            has_video=video is not None,
            has_audio=audio is not None,
        )
        if video:
            info = MediaInfo(
                path=p,
                duration_s=duration,
                width=int(video.get("width") or 0),
                height=int(video.get("height") or 0),
                fps=_parse_fps(video.get("r_frame_rate", "0/1")),
                frames=int(video.get("nb_frames") or 0),
                has_video=True,
                has_audio=audio is not None,
                pix_fmt=video.get("pix_fmt", ""),
                sar=video.get("sample_aspect_ratio", ""),
            )
        return info

    async def pixel_stats(self, path: str | Path, at_second: float | None = None) -> PixelStats:
        """取一帧的亮度统计。

        用 ffmpeg 的 signalstats 滤镜算，不在 Python 里解码，
        省掉 numpy 和 opencv 依赖。
        """
        p = Path(path)
        args = []
        if at_second is not None:
            args += ["-ss", f"{at_second:.3f}"]
        args += [
            "-i", str(p), "-frames:v", "1",
            "-vf", "signalstats,metadata=print:file=-",
            "-f", "null", "-",
        ]
        # signalstats 把结果写到 stdout，失败时退回读 stderr
        out = await self.run_ffmpeg(args, timeout_s=120.0)
        return _parse_signalstats(out)

    async def sample_pixel_stats(
        self, path: str | Path, samples: int = 3,
    ) -> list[PixelStats]:
        """在片段的几个位置取样。只看一帧会漏掉中途崩坏的镜头。"""
        info = await self.probe(path)
        if info.duration_s <= 0:
            return []
        points = [
            info.duration_s * frac
            for frac in _sample_points(samples)
        ]
        out = []
        for t in points:
            try:
                out.append(await self.pixel_stats(path, at_second=t))
            except FFmpegError:
                continue
        return out

    async def measure_loudness(self, path: str | Path) -> dict[str, float]:
        """测响度。两遍归一法的第一遍。"""
        p = Path(path)
        args = [
            "-i", str(p), "-af",
            "loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json",
            "-f", "null", "-",
        ]
        try:
            proc = await asyncio.create_subprocess_exec(
                self.ffmpeg, "-hide_banner", "-nostdin", *args,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
            )
            _, err = await asyncio.wait_for(proc.communicate(), timeout=600)
        except FileNotFoundError as exc:
            raise FFmpegMissing(f"找不到 {self.ffmpeg}") from exc
        except asyncio.TimeoutError:
            raise FFmpegError("测响度超时") from None

        text = err.decode("utf-8", "replace")
        start = text.rfind("{")
        end = text.rfind("}")
        if start == -1 or end == -1:
            raise FFmpegError("loudnorm 没有返回测量结果，可能是文件里没有音轨")
        try:
            data = json.loads(text[start:end + 1])
        except json.JSONDecodeError as exc:
            raise FFmpegError(f"loudnorm 输出无法解析：{exc}") from exc
        return {k: _to_float(v) for k, v in data.items()
                if k.startswith(("input_", "output_", "target_"))}

    async def extract_frame(
        self, path: str | Path, dest: str | Path, at_second: float = 0.0,
    ) -> Path:
        """抽一帧存成图片。首尾帧比对和接龙都要用。"""
        target = Path(dest)
        target.parent.mkdir(parents=True, exist_ok=True)
        await self.run_ffmpeg([
            "-ss", f"{at_second:.3f}", "-i", str(path),
            "-frames:v", "1", "-q:v", "2", str(target),
        ], timeout_s=120.0)
        return target


def _to_float(value) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def _parse_fps(rate: str) -> float:
    try:
        num, _, den = rate.partition("/")
        d = float(den or 1)
        return float(num) / d if d else 0.0
    except ValueError:
        return 0.0


def _sample_points(samples: int) -> list[float]:
    """取样位置。避开首尾各 10%，那里常有编码伪影。"""
    if samples <= 1:
        return [0.5]
    lo, hi = 0.1, 0.9
    step = (hi - lo) / (samples - 1)
    return [lo + step * i for i in range(samples)]


_STAT_KEYS = {
    "lavfi.signalstats.YAVG": "mean",
    "lavfi.signalstats.YMIN": "minimum",
    "lavfi.signalstats.YMAX": "maximum",
    "lavfi.signalstats.YLOW": "low",
    "lavfi.signalstats.YHIGH": "high",
}


def _parse_signalstats(text: str) -> PixelStats:
    """解析 signalstats 的输出。

    signalstats 不输出标准差。YDIF 是相邻帧差异，单帧模式下根本不产生，
    拿它当标准差会让所有画面都被判成纯色。用 YLOW 和 YHIGH 这两个
    百分位数算展布才是对的。
    """
    found: dict[str, float] = {}
    for line in text.splitlines():
        key, sep, value = line.strip().partition("=")
        if not sep:
            continue
        name = _STAT_KEYS.get(key.strip())
        if name:
            found[name] = _to_float(value)

    if "mean" not in found:
        raise FFmpegError("signalstats 没有返回亮度统计，可能是文件损坏或没有视频轨")

    low = found.get("low", found.get("minimum", 0.0))
    high = found.get("high", found.get("maximum", 0.0))
    return PixelStats(
        mean=found.get("mean", 0.0),
        spread=max(0.0, high - low),
        minimum=found.get("minimum", 0.0),
        maximum=found.get("maximum", 0.0),
        low=low,
        high=high,
    )
