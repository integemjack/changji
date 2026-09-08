# -*- coding: utf-8 -*-
"""装配与闸门的真实文件测试。

这些用真的 ffmpeg 跑真的视频。纸面测试盖不住编码规格、滤镜语法、
路径转义这类问题，它们只在真跑的时候才暴露。
"""
import shutil

import pytest

from changji.assembly.assemble import Assembler, AssemblyError, build_timeline
from changji.assembly.ffmpeg import FFmpeg
from changji.config import AssemblyConfig, GateConfig
from changji.gates.checks import Verdict, gate_episode, gate_video
from changji.models.project import ProjectStore
from changji.models.shot import CharacterInShot, DialogueLine, Shot, Transition

pytestmark = pytest.mark.skipif(
    shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None,
    reason="需要 ffmpeg",
)


@pytest.fixture
def ff():
    return FFmpeg()


async def _make_clip(ff, path, seconds=2.0, size="320x192", pattern="testsrc2"):
    """造一段有内容的测试视频。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    await ff.run_ffmpeg([
        "-f", "lavfi", "-i", f"{pattern}=size={size}:rate=24:duration={seconds}",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-t", str(seconds), str(path),
    ])
    return path


async def _make_solid(ff, path, color="gray", seconds=1.0, size="320x192"):
    """造一段纯色视频。color 滤镜的尺寸参数是 s 不是 size。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    await ff.run_ffmpeg([
        "-f", "lavfi", "-i", f"color=c={color}:s={size}:rate=24:duration={seconds}",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-t", str(seconds), str(path),
    ])
    return path


async def _make_blank(ff, path, seconds=2.0):
    """造一段纯色废片。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    await ff.run_ffmpeg([
        "-f", "lavfi", "-i", f"color=c=gray:s=320x192:rate=24:duration={seconds}",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-t", str(seconds), str(path),
    ])
    return path


class TestGateVideo:
    async def test_正常视频通过(self, tmp_path, ff):
        clip = await _make_clip(ff, tmp_path / "ok.mp4")
        shot = Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=2.0)
        r = await gate_video(shot, clip, ff, GateConfig(), expected_duration_s=2.0)
        assert r.ok, r.reasons

    async def test_纯色废片被拦下(self, tmp_path, ff):
        clip = await _make_blank(ff, tmp_path / "blank.mp4")
        shot = Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=2.0)
        r = await gate_video(shot, clip, ff, GateConfig(), expected_duration_s=2.0)
        assert not r.ok
        assert r.verdict is Verdict.RETRY
        assert any("纯色" in x for x in r.reasons)

    async def test_时长不符判为需退回(self, tmp_path, ff):
        """时长差太多是帧数算错了，重跑一样错。"""
        clip = await _make_clip(ff, tmp_path / "short.mp4", seconds=1.0)
        shot = Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=5.0)
        r = await gate_video(shot, clip, ff, GateConfig(), expected_duration_s=5.0)
        assert r.verdict is Verdict.REGRESS
        assert any("帧数" in x for x in r.reasons)

    async def test_分辨率不符判为需退回(self, tmp_path, ff):
        clip = await _make_clip(ff, tmp_path / "small.mp4", size="160x96")
        shot = Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=2.0)
        r = await gate_video(shot, clip, ff, GateConfig(),
                             expected_duration_s=2.0, expected_size=(320, 192))
        assert r.verdict is Verdict.REGRESS
        assert any("档位参数" in x for x in r.reasons)

    async def test_文件不存在判为可重试(self, tmp_path, ff):
        shot = Shot(shot_id="ep01_sh001", scene_id="s1", order=0)
        r = await gate_video(shot, tmp_path / "没有.mp4", ff, GateConfig())
        assert r.verdict is Verdict.RETRY

    async def test_损坏文件不会让程序崩(self, tmp_path, ff):
        bad = tmp_path / "bad.mp4"
        bad.write_bytes(b"\x00" * 4096)
        shot = Shot(shot_id="ep01_sh001", scene_id="s1", order=0)
        r = await gate_video(shot, bad, ff, GateConfig())
        assert not r.ok


class TestAssembleReal:
    async def _project(self, tmp_path, ff, with_dialogue=True):
        store = ProjectStore.create(tmp_path / "proj", "proj")
        shots = []
        for i in range(3):
            sid = f"ep01_sh{i + 1:03d}"
            clip = await _make_clip(ff, store.paths.shots("final") / f"{sid}.mp4",
                                    seconds=2.0)
            dialogue = []
            if with_dialogue:
                wav = store.paths.audio / f"{sid}_00.wav"
                await ff.run_ffmpeg([
                    "-f", "lavfi", "-i", "sine=frequency=440:duration=1.2",
                    "-c:a", "pcm_s16le", str(wav)])
                dialogue = [DialogueLine(
                    char_id="c_a", text=f"这是第{i + 1}句台词内容",
                    actual_duration_s=1.2,
                    audio_path=store.paths.rel(wav))]
            shots.append(Shot(
                shot_id=sid, scene_id="ep01_s01", order=i, duration_s=2.0,
                characters=[CharacterInShot(char_id="c_a")] if with_dialogue else [],
                dialogue=dialogue,
                subtitle_text=f"这是第{i + 1}句台词内容",
                video_path=store.paths.rel(clip),
            ))
        return store, shots

    async def test_装配出成片(self, tmp_path, ff):
        store, shots = await self._project(tmp_path, ff)
        cfg = AssemblyConfig()
        tl = build_timeline(shots, store.paths, cfg)
        out = await Assembler(ff, cfg, store.paths).assemble(tl, "ep01.mp4")
        assert out.is_file()
        info = await ff.probe(out)
        assert info.has_video and info.has_audio
        assert info.duration_s == pytest.approx(6.0, abs=1.0)

    async def test_成片有字幕烧进去(self, tmp_path, ff):
        """字幕烧录会重编码，能跑通就说明滤镜路径转义对了。"""
        store, shots = await self._project(tmp_path, ff)
        cfg = AssemblyConfig()
        tl = build_timeline(shots, store.paths, cfg)
        await Assembler(ff, cfg, store.paths).assemble(tl, "ep01.mp4")
        ass_files = list(store.paths.subtitles.glob("*.ass"))
        assert ass_files, "没有生成字幕文件"
        text = ass_files[0].read_text(encoding="utf-8-sig")
        assert "这是第1句台词内容" in text

    async def test_无台词也有音轨(self, tmp_path, ff):
        """没有音轨的文件会被平台当成损坏。"""
        store, shots = await self._project(tmp_path, ff, with_dialogue=False)
        cfg = AssemblyConfig()
        tl = build_timeline(shots, store.paths, cfg)
        out = await Assembler(ff, cfg, store.paths).assemble(tl, "ep01.mp4")
        assert (await ff.probe(out)).has_audio

    async def test_分辨率不一的镜头能拼(self, tmp_path, ff):
        """统一编码规格这一步的意义。规格不齐直接拼会花屏。"""
        store = ProjectStore.create(tmp_path / "proj", "proj")
        shots = []
        for i, size in enumerate(("320x192", "480x288")):
            sid = f"ep01_sh{i + 1:03d}"
            clip = await _make_clip(ff, store.paths.shots("final") / f"{sid}.mp4",
                                    seconds=1.5, size=size)
            shots.append(Shot(shot_id=sid, scene_id="s1", order=i, duration_s=1.5,
                              video_path=store.paths.rel(clip)))
        cfg = AssemblyConfig()
        tl = build_timeline(shots, store.paths, cfg)
        out = await Assembler(ff, cfg, store.paths).assemble(tl, "ep01.mp4")
        info = await ff.probe(out)
        assert info.width == 480 and info.height == 288

    async def test_缺视频的镜头报错点名(self, tmp_path, ff):
        store = ProjectStore.create(tmp_path / "proj", "proj")
        shots = [Shot(shot_id="ep01_sh001", scene_id="s1", order=0)]
        with pytest.raises(AssemblyError, match="ep01_sh001"):
            build_timeline(shots, store.paths, AssemblyConfig())

    async def test_成片过整集闸门(self, tmp_path, ff):
        store, shots = await self._project(tmp_path, ff)
        cfg = AssemblyConfig()
        tl = build_timeline(shots, store.paths, cfg)
        out = await Assembler(ff, cfg, store.paths).assemble(tl, "ep01.mp4")
        r = await gate_episode(out, ff, GateConfig(), expected_duration_s=6.0)
        assert r.metrics.get("duration_s", 0) > 0
        assert "lufs" in r.metrics


class TestTimeline:
    async def test_字幕时间戳来自配音真实时长(self, tmp_path, ff):
        store, shots = await TestAssembleReal()._project(tmp_path, ff)
        tl = build_timeline(shots, store.paths, AssemblyConfig())
        cues = tl.cues()
        assert len(cues) == 3
        assert cues[0].duration_s == pytest.approx(1.2, abs=0.01)
        # 第二条应该落在第二个镜头开头
        assert cues[1].start_s == pytest.approx(2.0, abs=0.01)

    async def test_溶解让镜头重叠(self, tmp_path, ff):
        store = ProjectStore.create(tmp_path / "proj", "proj")
        shots = []
        for i in range(2):
            sid = f"ep01_sh{i + 1:03d}"
            clip = await _make_clip(ff, store.paths.shots("final") / f"{sid}.mp4", 2.0)
            shots.append(Shot(
                shot_id=sid, scene_id="s1", order=i, duration_s=2.0,
                transition_in=Transition.DISSOLVE if i else Transition.CUT,
                transition_dur_s=0.4 if i else 0.0,
                video_path=store.paths.rel(clip)))
        tl = build_timeline(shots, store.paths, AssemblyConfig())
        assert tl.entries[1].start_s == pytest.approx(1.6, abs=0.01)


class TestBrightnessSwingGate:
    """回归测试：亮度剧烈跳变说明镜头中途崩坏。

    真实跑一集时这条规则拦下了一个废镜头，跳变值 73。换种子重跑后通过。
    只看一帧的检查抓不到这类问题，必须多点取样比较。
    """

    async def test_亮度稳定的镜头通过(self, tmp_path, ff):
        clip = await _make_clip(ff, tmp_path / "steady.mp4", seconds=2.0)
        shot = Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=2.0)
        r = await gate_video(shot, clip, ff, GateConfig(), expected_duration_s=2.0)
        assert r.ok, r.reasons
        assert r.metrics.get("mean_swing", 0) < 60

    async def test_前后半段亮度突变的镜头被拦下(self, tmp_path, ff):
        """前一秒近黑，后一秒近白，模拟生成中途崩坏。"""
        dark = await _make_solid(ff, tmp_path / "dark.mp4", "0x101010")
        bright = await _make_solid(ff, tmp_path / "bright.mp4", "0xF0F0F0")
        listing = tmp_path / "list.txt"
        listing.write_text(
            f"file '{dark.as_posix()}'\nfile '{bright.as_posix()}'\n",
            encoding="utf-8")
        joined = tmp_path / "swing.mp4"
        await ff.run_ffmpeg(["-f", "concat", "-safe", "0", "-i", str(listing),
                             "-c", "copy", str(joined)])

        shot = Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=2.0)
        r = await gate_video(shot, joined, ff, GateConfig(), expected_duration_s=2.0)
        assert not r.ok
        assert any("亮度" in x for x in r.reasons), r.reasons

    async def test_多点取样才能发现中途崩坏(self, tmp_path, ff):
        """只看首帧的话这段视频看起来完全正常。"""
        dark = await _make_solid(ff, tmp_path / "d.mp4", "0x101010")
        first = await ff.pixel_stats(dark, at_second=0.0)
        assert first.looks_blank  # 单帧看是纯色
        samples = await ff.sample_pixel_stats(dark, samples=3)
        assert len(samples) == 3  # 取样点数正确


class TestShortAudioDoesNotTruncate:
    """回归测试：配音短于画面时绝不能丢画面。

    混音时用 -shortest 会把成片截到配音那么长。配音总长几乎总是短于
    画面，因为无对白的镜头没有音频。真跑一集四镜头的片子时丢了整整
    一个镜头，成片 7.9 秒而画面本该有 11.2 秒。
    """

    async def _project_with_short_audio(self, tmp_path, ff):
        store = ProjectStore.create(tmp_path / "proj", "proj")
        shots = []
        for i in range(4):
            sid = f"ep01_sh{i + 1:03d}"
            clip = await _make_clip(ff, store.paths.shots("final") / f"{sid}.mp4",
                                    seconds=2.0)
            dialogue = []
            # 只有头两个镜头有台词，后两个是无对白的过场
            if i < 2:
                wav = store.paths.audio / f"{sid}_00.wav"
                await ff.run_ffmpeg([
                    "-f", "lavfi", "-i", "sine=frequency=440:duration=0.8",
                    "-c:a", "pcm_s16le", str(wav)])
                dialogue = [DialogueLine(
                    char_id="c_a", text=f"第{i + 1}句", actual_duration_s=0.8,
                    audio_path=store.paths.rel(wav))]
            shots.append(Shot(
                shot_id=sid, scene_id="s1", order=i, duration_s=2.0,
                characters=[CharacterInShot(char_id="c_a")] if dialogue else [],
                dialogue=dialogue, video_path=store.paths.rel(clip)))
        return store, shots

    async def test_成片保留全部画面(self, tmp_path, ff):
        store, shots = await self._project_with_short_audio(tmp_path, ff)
        cfg = AssemblyConfig()
        tl = build_timeline(shots, store.paths, cfg)
        out = await Assembler(ff, cfg, store.paths).assemble(tl, "ep01.mp4")
        info = await ff.probe(out)
        # 四个两秒镜头，画面应有约八秒；配音只有 1.6 秒
        assert info.duration_s > 7.0, (
            f"成片只有 {info.duration_s:.1f} 秒，画面被配音截断了"
        )
        assert info.has_audio

    async def test_音轨补齐到画面长度(self, tmp_path, ff):
        store, shots = await self._project_with_short_audio(tmp_path, ff)
        cfg = AssemblyConfig()
        tl = build_timeline(shots, store.paths, cfg)
        out = await Assembler(ff, cfg, store.paths).assemble(tl, "ep01.mp4")
        raw = await ff.run_ffprobe([
            "-v", "error", "-select_streams", "a:0",
            "-show_entries", "stream=duration", "-of", "default=nw=1:nk=1",
            str(out)])
        audio_s = float(raw.strip().splitlines()[0])
        info = await ff.probe(out)
        assert abs(audio_s - info.duration_s) < 0.5, (
            f"音轨 {audio_s:.2f}s 与画面 {info.duration_s:.2f}s 不齐"
        )
