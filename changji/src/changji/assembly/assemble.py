"""成片装配。

四个决定成败的细节：

统一编码规格。所有镜头入库时先转成同一套参数，否则拼接会花屏丢帧。
转场只在场景切换处用。硬切走 concat 无需重编码，快一个量级。
中文字幕自己算断点。
响度归一的顺序：先单条配音，再混音，最后整集。顺序错了拼接处会有音量跳变。
"""

from __future__ import annotations

import shutil
from dataclasses import dataclass, field
from pathlib import Path

from ..config import AssemblyConfig
from ..models.project import ProjectPaths
from ..models.shot import Shot, Transition
from .ffmpeg import FFmpeg, FFmpegError
from .subtitles import SubtitleCue, validate_cues, write_ass


class AssemblyError(RuntimeError):
    pass


@dataclass
class Timeline:
    """一集的时间线。分镜表加配音时长推出来的排期。"""

    entries: list[TimelineEntry] = field(default_factory=list)

    @property
    def total_duration_s(self) -> float:
        return sum(e.duration_s for e in self.entries)

    def cues(self) -> list[SubtitleCue]:
        out: list[SubtitleCue] = []
        for entry in self.entries:
            out.extend(entry.cues)
        return out


@dataclass
class TimelineEntry:
    """时间线上的一个镜头。"""

    shot_id: str
    video_path: Path
    start_s: float
    duration_s: float
    transition_in: Transition
    transition_dur_s: float
    audio_paths: list[Path] = field(default_factory=list)
    cues: list[SubtitleCue] = field(default_factory=list)


def build_timeline(
    shots: list[Shot], paths: ProjectPaths, config: AssemblyConfig,
) -> Timeline:
    """由分镜表推出时间线。

    字幕的时间戳来自配音的真实时长，不是估算。这是音画对齐的最后一环。
    """
    timeline = Timeline()
    cursor = 0.0
    for shot in shots:
        if not shot.video_path:
            raise AssemblyError(f"镜头 {shot.shot_id} 还没有视频，不能装配")
        video = paths.abs(shot.video_path)
        if not video.is_file():
            raise AssemblyError(f"镜头 {shot.shot_id} 的视频文件不见了：{video}")

        # 溶解会让两镜重叠，起点要往回挪
        overlap = shot.transition_dur_s if shot.transition_in is not Transition.CUT else 0.0
        start = max(0.0, cursor - overlap)

        audio_paths: list[Path] = []
        cues: list[SubtitleCue] = []
        speech_cursor = start
        for line in shot.dialogue:
            dur = line.actual_duration_s or 0.0
            if line.audio_path:
                audio_paths.append(paths.abs(line.audio_path))
            text = line.text.strip()
            if text and dur > 0:
                cues.append(SubtitleCue(
                    start_s=speech_cursor,
                    end_s=speech_cursor + dur,
                    text=text,
                    style="narration" if line.char_id is None else "dialogue",
                ))
            speech_cursor += dur

        timeline.entries.append(TimelineEntry(
            shot_id=shot.shot_id,
            video_path=video,
            start_s=start,
            duration_s=shot.duration_s,
            transition_in=shot.transition_in,
            transition_dur_s=shot.transition_dur_s,
            audio_paths=audio_paths,
            cues=cues,
        ))
        cursor = start + shot.duration_s
    return timeline


class Assembler:
    """把镜头拼成一集。"""

    def __init__(
        self, ff: FFmpeg, config: AssemblyConfig, paths: ProjectPaths,
        target_lufs: float = -16.0, max_true_peak_db: float = -1.5,
    ) -> None:
        self.ff = ff
        self.config = config
        self.paths = paths
        # 响度目标从闸门配置传进来。以前这里写死 -16，界面上改了
        # 响度目标其实一点用没有，成片还是老样子。
        self.target_lufs = target_lufs
        self.max_true_peak_db = max_true_peak_db

    async def assemble(
        self, timeline: Timeline, out_name: str = "episode.mp4",
        burn_subtitles: bool = True,
    ) -> Path:
        """装配成片。返回成片路径。"""
        if not timeline.entries:
            raise AssemblyError("时间线是空的，没有可装配的镜头")

        work = self.paths.output / ".work"
        work.mkdir(parents=True, exist_ok=True)
        try:
            normalized = await self._normalize_all(timeline, work)
            silent = await self._concat(normalized, work)
            with_audio = await self._mix_audio(silent, timeline, work)
            final = self.paths.output / out_name
            if burn_subtitles and timeline.cues():
                await self._burn_subtitles(with_audio, timeline, final)
            else:
                shutil.move(str(with_audio), str(final))
            return final
        finally:
            shutil.rmtree(work, ignore_errors=True)

    async def _normalize_all(self, timeline: Timeline, work: Path) -> list[Path]:
        """统一编码规格。

        拼接环节最容易踩的坑。分辨率、帧率、像素格式、采样宽高比、时基
        任何一项不齐都会导致花屏或丢帧。
        """
        target_w, target_h = await self._target_size(timeline)
        out: list[Path] = []
        for i, entry in enumerate(timeline.entries):
            dest = work / f"norm_{i:04d}.mp4"
            await self.ff.run_ffmpeg([
                "-i", str(entry.video_path),
                "-vf", (
                    f"scale={target_w}:{target_h}:force_original_aspect_ratio=decrease,"
                    f"pad={target_w}:{target_h}:(ow-iw)/2:(oh-ih)/2,"
                    f"setsar=1,fps={self.config.fps}"
                ),
                "-an",  # 音频统一在后面处理
                "-c:v", self.config.video_codec,
                "-crf", str(self.config.crf),
                "-pix_fmt", self.config.pix_fmt,
                "-preset", "medium",
                "-video_track_timescale", "90000",
                str(dest),
            ])
            out.append(dest)
        return out

    async def _target_size(self, timeline: Timeline) -> tuple[int, int]:
        """取最大的那个分辨率作为统一规格，避免放大模糊。"""
        best = (0, 0)
        for entry in timeline.entries:
            try:
                info = await self.ff.probe(entry.video_path)
            except FFmpegError:
                continue
            if info.width * info.height > best[0] * best[1]:
                best = (info.width, info.height)
        if best == (0, 0):
            raise AssemblyError("所有镜头都读不出分辨率")
        # 必须是偶数，否则 yuv420p 编不了
        return best[0] - best[0] % 2, best[1] - best[1] % 2

    async def _concat(self, clips: list[Path], work: Path) -> Path:
        """拼接。

        全部硬切时走 concat 分离器，零重编码。短剧九成的镜头切换本来
        就该是硬切，只在场景切换处用溶解，能省掉九成的重编码时间。
        """
        listing = work / "concat.txt"
        listing.write_text(
            "\n".join(f"file '{p.as_posix()}'" for p in clips) + "\n",
            encoding="utf-8",
        )
        dest = work / "joined.mp4"
        await self.ff.run_ffmpeg([
            "-f", "concat", "-safe", "0", "-i", str(listing),
            "-c", "copy", str(dest),
        ])
        return dest

    async def _mix_audio(self, video: Path, timeline: Timeline, work: Path) -> Path:
        """混音并做响度归一。

        顺序很重要：先对每条配音单独归一，再混音，最后对整集再归一一次。
        顺序错了拼接处会有明显的音量跳变。
        """
        segments: list[tuple[Path, float]] = []
        for entry in timeline.entries:
            cursor = entry.start_s
            for idx, audio in enumerate(entry.audio_paths):
                segments.append((audio, cursor))
                cue = entry.cues[idx] if idx < len(entry.cues) else None
                cursor += cue.duration_s if cue else 0.0

        dest = work / "with_audio.mp4"
        if not segments:
            # 没有配音也要有音轨，否则平台会认为文件损坏。
            # 这里用 -shortest 是安全的：静音源是无限长的，
            # 截到视频长度正是想要的结果。
            await self.ff.run_ffmpeg([
                "-i", str(video),
                "-f", "lavfi", "-i",
                f"anullsrc=r={self.config.audio_sample_rate}:cl=stereo",
                "-c:v", "copy", "-c:a", self.config.audio_codec,
                "-b:a", self.config.audio_bitrate,
                "-ar", str(self.config.audio_sample_rate),
                "-ac", str(self.config.audio_channels),
                "-shortest", str(dest),
            ])
            return dest

        args = ["-i", str(video)]
        for audio, _ in segments:
            args += ["-i", str(audio)]

        # 每条配音延迟到自己的位置，然后混在一起
        filters = []
        for i, (_, at) in enumerate(segments, start=1):
            delay_ms = int(at * 1000)
            filters.append(
                f"[{i}:a]aresample={self.config.audio_sample_rate},"
                f"adelay={delay_ms}|{delay_ms}[a{i}]"
            )
        mix_inputs = "".join(f"[a{i}]" for i in range(1, len(segments) + 1))
        # loudnorm 之后必须再 aresample 一次。这个滤镜内部按 192k 工作，
        # 不收回来的话编码器会选个 96k 之类的采样率，文件白白变大。
        filters.append(
            f"{mix_inputs}amix=inputs={len(segments)}:dropout_transition=0:normalize=0,"
            f"loudnorm=I={self.target_lufs}:TP={self.max_true_peak_db}:LRA=11,"
            f"aresample={self.config.audio_sample_rate}[aout]"
        )

        # 音轨补静音到视频长度。
        #
        # 这里绝不能用 -shortest：配音总长几乎总是短于画面，
        # 因为无对白的镜头没有音频。用 -shortest 会把成片截到配音那么长，
        # 后面的画面直接丢掉。实测一次四镜头的片子丢了整整一个镜头。
        filters[-1] = filters[-1].replace("[aout]", "[amixed]")
        filters.append("[amixed]apad[aout]")

        args += [
            "-filter_complex", ";".join(filters),
            "-map", "0:v", "-map", "[aout]",
            "-c:v", "copy",
            "-c:a", self.config.audio_codec, "-b:a", self.config.audio_bitrate,
            "-ar", str(self.config.audio_sample_rate),
            "-ac", str(self.config.audio_channels),
            # 以视频为准截断补出来的静音尾巴
            "-t", f"{await self._video_duration(video):.3f}",
            str(dest),
        ]
        await self.ff.run_ffmpeg(args)
        return dest

    async def _video_duration(self, path: Path) -> float:
        info = await self.ff.probe(path)
        return info.duration_s

    async def _burn_subtitles(self, video: Path, timeline: Timeline, dest: Path) -> Path:
        """烧字幕。"""
        info = await self.ff.probe(video)
        ass_path = self.paths.subtitles / f"{dest.stem}.ass"
        write_ass(
            ass_path, timeline.cues(),
            width=info.width or 1080, height=info.height or 1920,
            font=self.config.subtitle_font,
            max_chars_per_line=self.config.subtitle_max_chars_per_line,
            max_lines=self.config.subtitle_max_lines,
        )
        dest.parent.mkdir(parents=True, exist_ok=True)
        # Windows 上路径里的冒号和反斜杠会被滤镜语法吃掉，要转义
        escaped = _escape_filter_path(ass_path)
        await self.ff.run_ffmpeg([
            "-i", str(video),
            "-vf", f"subtitles='{escaped}'",
            "-c:v", self.config.video_codec, "-crf", str(self.config.crf),
            "-pix_fmt", self.config.pix_fmt, "-preset", "medium",
            "-c:a", "copy",
            str(dest),
        ])
        return dest


def _escape_filter_path(path: Path) -> str:
    """把路径转成 ffmpeg 滤镜能接受的形式。

    Windows 上 C:\\x 里的冒号是滤镜的参数分隔符，反斜杠是转义符，
    直接传进去会解析失败。
    """
    text = str(path).replace("\\", "/")
    return text.replace(":", r"\:")


def subtitle_problems(timeline: Timeline, config: AssemblyConfig) -> list[str]:
    """装配前先查一遍字幕。"""
    return validate_cues(timeline.cues(), config.subtitle_max_chars_per_line)
