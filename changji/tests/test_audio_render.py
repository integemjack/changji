# -*- coding: utf-8 -*-
"""配音与渲染。配音先行决定音画能否对齐，提示词拼接决定角色是否一致。"""
import pytest

from changji.config import TTSConfig
from changji.models.character import (
    AppearanceBlock, AssetLibrary, Character, Location, StyleLine,
    StyleProfile, WardrobeVariant,
)
from changji.models.project import ProjectStore
from changji.models.shot import (
    CameraMove, CharacterInShot, DialogueLine, FacePose, Shot, ShotSize,
)
from changji.stages.audio import (
    AudioStage, EstimateBackend, estimate_speech_duration, probe_wav_duration,
    summarize,
)
from changji.stages.render import PromptComposer, RenderError, frames_for

IDENT = "二十八岁女性，身形清瘦，黑色长发挽起，灰色西装套裙"


def _assets(style_line=StyleLine.REALISTIC):
    return AssetLibrary(
        characters={
            "c_lin": Character(
                char_id="c_lin", name="林晚", voice_id="v_lin",
                appearance=AppearanceBlock(
                    identity="二十八岁女性", body="身形清瘦",
                    face="黑色长发挽起", attire="灰色西装套裙"),
                wardrobe=[WardrobeVariant(wardrobe_id="torn", description="西装外套破损")],
                ref_front="refs/lin_front.png",
            ),
        },
        locations={"loc_office": Location(
            location_id="loc_office", name="办公室",
            space="高层落地窗办公室", lighting="夜间冷调顶光")},
        style=StyleProfile(style_line=style_line, global_style="电影感，浅景深",
                           negative_prompt="低质量"),
    )


class TestFrames:
    def test_帧数满足4n加1(self):
        for d in (2.0, 3.0, 5.0, 8.0, 10.0):
            assert (frames_for(d) - 1) % 4 == 0

    def test_五秒是121帧(self):
        assert frames_for(5.0) == 121

    def test_不超过121帧(self):
        """超过 120 帧会在约 100 帧处到达末帧后往回跑，出现乒乓现象。"""
        assert frames_for(30.0) == 121
        assert frames_for(10.0) <= 121

    def test_短镜头帧数合理(self):
        assert 40 <= frames_for(2.0) <= 56


class TestSpeechEstimate:
    def test_越长的台词越久(self):
        short = estimate_speech_duration("你说什么")
        long = estimate_speech_duration("你说什么我一个字都没有听懂请你再讲一遍")
        assert short < long

    def test_空文本为零(self):
        assert estimate_speech_duration("") == 0.0
        assert estimate_speech_duration("   ") == 0.0

    def test_标点带来停顿(self):
        assert estimate_speech_duration("你说，什么啊") > estimate_speech_duration("你说什么啊")

    def test_量级合理(self):
        """九个字的台词应该在两到四秒半之间。"""
        assert 2.0 < estimate_speech_duration("你到底想让我怎么做") < 4.5


class TestAudioStage:
    def _shot(self, texts, **kw):
        opts = dict(
            shot_id="ep01_sh001", scene_id="ep01_s01", order=0, duration_s=3.0,
            characters=[CharacterInShot(char_id="c_lin")],
            dialogue=[DialogueLine(char_id="c_lin", text=t) for t in texts],
        )
        opts.update(kw)
        return Shot(**opts)

    async def test_配音后回填时长并锁定(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        shot = self._shot(["你到底想让我怎么做"])
        plans = await AudioStage(EstimateBackend(), TTSConfig(), store.paths).run(
            [shot], _assets())
        assert shot.dialogue[0].actual_duration_s > 0
        assert shot.duration_locked is True
        assert plans[0].locked_duration_s >= plans[0].speech_duration_s

    async def test_时长向上吸附不会截断台词(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        shot = self._shot(["需要说一会儿的台词"])
        await AudioStage(EstimateBackend(), TTSConfig(), store.paths).run(
            [shot], _assets())
        assert shot.duration_s >= shot.dialogue[0].actual_duration_s

    async def test_无台词镜头时长不动(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        shot = self._shot([], characters=[], dialogue=[])
        await AudioStage(EstimateBackend(), TTSConfig(), store.paths).run(
            [shot], _assets())
        assert shot.duration_s == 3.0
        assert shot.duration_locked is False

    async def test_音色来自角色资产不来自分镜(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        shot = self._shot(["一句话"])
        await AudioStage(EstimateBackend(), TTSConfig(), store.paths).run(
            [shot], _assets())
        assert shot.dialogue[0].voice_id == "v_lin"

    async def test_音频存成项目内相对路径(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        shot = self._shot(["一句话"])
        await AudioStage(EstimateBackend(), TTSConfig(), store.paths).run(
            [shot], _assets())
        rel = shot.dialogue[0].audio_path
        assert rel.startswith("audio/")
        assert "\\" not in rel
        assert store.paths.abs(rel).is_file()

    async def test_产出静音时长与回填一致(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        shot = self._shot(["你到底想让我怎么做"])
        await AudioStage(EstimateBackend(), TTSConfig(), store.paths).run(
            [shot], _assets())
        wav = store.paths.abs(shot.dialogue[0].audio_path)
        assert probe_wav_duration(wav) == pytest.approx(
            shot.dialogue[0].actual_duration_s, abs=0.05)

    async def test_多句台词累加(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        shot = self._shot(["第一句话", "第二句话"])
        plans = await AudioStage(EstimateBackend(), TTSConfig(), store.paths).run(
            [shot], _assets())
        assert plans[0].lines == 2
        assert plans[0].speech_duration_s == pytest.approx(
            sum(d.actual_duration_s for d in shot.dialogue), abs=0.01)

    async def test_概览可读(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        plans = await AudioStage(EstimateBackend(), TTSConfig(), store.paths).run(
            [self._shot(["短"])], _assets())
        assert "配音完成" in summarize(plans)


class TestPromptComposer:
    def _shot(self, **kw):
        opts = dict(
            shot_id="ep01_sh001", scene_id="ep01_s01", order=0,
            shot_size=ShotSize.MCU, location_id="loc_office",
            first_frame_prompt="她站在窗前背对镜头",
            characters=[CharacterInShot(char_id="c_lin", expression="隐忍",
                                        action="缓缓转身")],
        )
        opts.update(kw)
        return Shot(**opts)

    def test_身份层来自资产库而非分镜(self):
        """一致性的核心。分镜里没有外观，外观只能从资产库来。"""
        p = PromptComposer(_assets()).compose(self._shot())
        assert "黑色长发挽起" in p.positive
        assert "灰色西装套裙" in p.positive

    def test_同一角色跨镜头逐字节一致(self):
        c = PromptComposer(_assets())
        a = c.compose(self._shot(shot_size=ShotSize.CU))
        b = c.compose(self._shot(shot_size=ShotSize.LS, first_frame_prompt="远景"))
        assert IDENT in a.positive
        assert IDENT in b.positive

    def test_换装只替换服装段其余不变(self):
        p = PromptComposer(_assets()).compose(self._shot(
            characters=[CharacterInShot(char_id="c_lin", wardrobe_state="torn")]))
        assert "西装外套破损" in p.positive
        assert "灰色西装套裙" not in p.positive
        assert "黑色长发挽起" in p.positive

    def test_包含场景与风格层(self):
        p = PromptComposer(_assets()).compose(self._shot())
        assert "高层落地窗办公室" in p.positive
        assert "夜间冷调顶光" in p.positive
        assert "电影感" in p.positive

    def test_景别被翻译成中文(self):
        p = PromptComposer(_assets()).compose(self._shot(shot_size=ShotSize.ECU))
        assert "大特写" in p.positive

    def test_参考图被收集(self):
        p = PromptComposer(_assets()).compose(self._shot(
            characters=[CharacterInShot(char_id="c_lin", face_pose=FacePose.FRONT)]))
        assert "refs/lin_front.png" in p.reference_images

    def test_动漫线用逗号分隔(self):
        p = PromptComposer(_assets(StyleLine.ANIME)).compose(self._shot())
        assert ", " in p.positive

    def test_未注册角色报错点名(self):
        with pytest.raises(RenderError, match="c_ghost"):
            PromptComposer(_assets()).compose(self._shot(
                characters=[CharacterInShot(char_id="c_ghost")]))

    def test_未注册场景报错点名(self):
        with pytest.raises(RenderError, match="loc_void"):
            PromptComposer(_assets()).compose(self._shot(location_id="loc_void"))

    def test_运动提示词只讲动作不重复外观(self):
        m = PromptComposer(_assets()).motion_prompt(
            self._shot(camera_move=CameraMove.PUSH_IN))
        assert "镜头缓慢推近" in m
        assert "缓缓转身" in m
        assert "灰色西装套裙" not in m

    def test_负向提示词合并分镜与全局(self):
        p = PromptComposer(_assets()).compose(self._shot(negative_prompt="模糊"))
        assert "模糊" in p.negative
        assert "低质量" in p.negative


class TestAudioProfile:
    """成片的音频规格要固定，不能由滤镜链随手决定。"""

    def _assembler(self, tmp_path, **kw):
        from changji.assembly.assemble import Assembler
        from changji.assembly.ffmpeg import FFmpeg
        from changji.config import AssemblyConfig
        from changji.models.project import ProjectStore

        store = ProjectStore.create(tmp_path / "p", "drama")
        return Assembler(FFmpeg(AssemblyConfig()), AssemblyConfig(),
                         store.paths, **kw)

    def test_默认采样率是四万八(self):
        from changji.config import AssemblyConfig
        c = AssemblyConfig()
        assert c.audio_sample_rate == 48000
        assert c.audio_channels == 2

    def test_响度目标来自配置不是写死的(self, tmp_path):
        a = self._assembler(tmp_path, target_lufs=-14.0, max_true_peak_db=-2.0)
        assert a.target_lufs == -14.0
        assert a.max_true_peak_db == -2.0

    @pytest.mark.asyncio
    async def test_loudnorm后面要收回采样率(self, tmp_path, monkeypatch):
        """loudnorm 内部按 192k 工作。不收回来编码器会挑个 96k，
        文件白白变大，实测成片就是 96000Hz。"""
        a = self._assembler(tmp_path)
        seen = []

        async def fake_run(args):
            seen.append(args)

        async def fake_dur(path):
            return 10.0

        monkeypatch.setattr(a.ff, "run_ffmpeg", fake_run)
        monkeypatch.setattr(a, "_video_duration", fake_dur)

        work = tmp_path / "w"
        work.mkdir()
        wav = work / "a.wav"
        wav.write_bytes(b"\0")
        from changji.assembly.assemble import Timeline, TimelineEntry
        from changji.models.shot import Transition

        tl = Timeline(entries=[TimelineEntry(
            shot_id="sh001", video_path=tmp_path / "v.mp4", start_s=0.0,
            duration_s=5.0, transition_in=Transition.CUT,
            transition_dur_s=0.0, audio_paths=[wav])])
        await a._mix_audio(tmp_path / "v.mp4", tl, work)

        args = seen[-1]
        chain = args[args.index("-filter_complex") + 1]
        assert "loudnorm" in chain
        after = chain.split("loudnorm")[1]
        assert "aresample=48000" in after, "loudnorm 之后必须再收一次采样率"
        assert args[args.index("-ar") + 1] == "48000"
        assert args[args.index("-ac") + 1] == "2"
