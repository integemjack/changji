# -*- coding: utf-8 -*-
"""台词装不进镜头时怎么办。

单镜时长有硬上限，来自视频模型能生成的最大帧数，实测是 5.04 秒。
台词配出来七八秒，镜头只有五秒，混音时后面的声音会盖到下一镜上，
成片里两个人同时说话。

实测就是这么栽的：AI 写的一集里有两句长台词，配出来 7.00 秒和
8.36 秒，音画闸门报了警，装配照样往下走，出来的片子那两处就是
两个声音叠在一起。

两道处理。一是单句太长的先按标点切开，切完每句都装得进一个镜头。
二是一镜里台词太多的，把镜头拆成连着的几镜。
"""
import pytest

from changji.models.shot import (
    CharacterInShot, DialogueLine, Shot, ShotSize, ShotStatus,
)
from changji.stages.audio import (
    TAIL_S, estimate_speech_duration, max_line_seconds, split_long_text,
    split_overlong_shots,
)
from changji.stages.render import max_shot_duration_s

LIMIT = max_shot_duration_s()


class TestSplitText:
    def test_短句原样不动(self):
        assert split_long_text("我在。", max_line_seconds()) == ["我在。"]

    def test_长句按句末标点切开(self):
        t = ("这么晚了，是需要点什么吗？买点零食吧，"
             "吃饱肚子才有力气继续等下去，别把自己熬坏了。")
        pieces = split_long_text(t, max_line_seconds())
        assert len(pieces) > 1
        assert "".join(pieces) == t.replace(" ", ""), "一个字都不能丢"

    def test_切完每句都装得进(self):
        t = ("这么晚了，是需要点什么吗？买点零食吧，"
             "吃饱肚子才有力气继续等下去，别把自己熬坏了。")
        lim = max_line_seconds()
        for piece in split_long_text(t, lim):
            assert estimate_speech_duration(piece) <= lim, piece

    def test_没有标点也能切(self):
        lim = max_line_seconds()
        pieces = split_long_text("啊" * 80, lim)
        assert len(pieces) > 1
        for piece in pieces:
            assert estimate_speech_duration(piece) <= lim

    def test_硬切时算上头尾留白(self):
        """每一段都自带呼吸留白，不扣掉的话切出来每段都刚好超一点。"""
        lim = max_line_seconds()
        for piece in split_long_text("啊" * 200, lim):
            assert estimate_speech_duration(piece) <= lim

    def test_不丢字(self):
        t = "啊" * 100
        assert "".join(split_long_text(t, max_line_seconds())) == t


def _shot(shot_id: str, order: int, lines: list[tuple[str, float]]) -> Shot:
    return Shot(
        shot_id=shot_id, scene_id="s1", order=order, duration_s=5.0,
        shot_size=ShotSize.MS, first_frame_prompt="画面",
        characters=[CharacterInShot(char_id="c_a")],
        dialogue=[DialogueLine(char_id="c_a", text=t, actual_duration_s=d,
                               audio_path=f"audio/{shot_id}_{i}.wav")
                  for i, (t, d) in enumerate(lines)],
        status=ShotStatus.AUDIO_DONE, frame_path="frames/x.png",
    )


class TestSplitShots:
    def test_装得下就不动(self):
        shots = [_shot("sh001", 0, [("短", 2.0)])]
        out = split_overlong_shots(shots, LIMIT)
        assert len(out) == 1
        assert out[0].shot_id == "sh001"

    def test_装不下就拆(self):
        shots = [_shot("sh001", 0, [("一", 3.0), ("二", 3.0), ("三", 3.0)])]
        out = split_overlong_shots(shots, LIMIT)
        assert len(out) > 1
        assert [s.shot_id for s in out][0] == "sh001"
        for s in out:
            total = sum(x.actual_duration_s for x in s.dialogue)
            assert total + TAIL_S <= LIMIT + 1e-6, f"{s.shot_id} 还是装不下"

    def test_台词一句都不丢(self):
        shots = [_shot("sh001", 0, [("一", 3.0), ("二", 3.0), ("三", 3.0)])]
        out = split_overlong_shots(shots, LIMIT)
        texts = [d.text for s in out for d in s.dialogue]
        assert texts == ["一", "二", "三"]

    def test_原镜留着已有的音频和画面(self):
        shots = [_shot("sh001", 0, [("一", 3.0), ("二", 3.0)])]
        out = split_overlong_shots(shots, LIMIT)
        assert out[0].frame_path == "frames/x.png"
        assert out[0].dialogue[0].audio_path == "audio/sh001_0.wav"

    def test_新镜从头生成(self):
        """新镜是全新的画面，不能继承上一镜的产物。"""
        shots = [_shot("sh001", 0, [("一", 3.0), ("二", 3.0)])]
        out = split_overlong_shots(shots, LIMIT)
        extra = out[1]
        assert extra.frame_path is None
        assert extra.video_path is None
        assert extra.status is ShotStatus.PLANNED
        assert extra.attempts == 0
        # 音频是配好的，不该丢
        assert extra.dialogue[0].audio_path == "audio/sh001_1.wav"

    def test_新镜的id不重号(self):
        shots = [_shot("sh001", 0, [("一", 3.0), ("二", 3.0), ("三", 3.0)])]
        out = split_overlong_shots(shots, LIMIT)
        ids = [s.shot_id for s in out]
        assert len(ids) == len(set(ids))

    def test_拆完顺序连着排(self):
        shots = [
            _shot("sh001", 0, [("一", 3.0), ("二", 3.0)]),
            _shot("sh002", 1, [("三", 2.0)]),
        ]
        out = split_overlong_shots(shots, LIMIT)
        assert [s.order for s in out] == list(range(len(out)))
        # 后面那一镜不能被插到中间去
        assert out[-1].shot_id == "sh002"

    def test_只有一句话时不拆(self):
        """一句话拆不开，那是台词切分该管的事。"""
        shots = [_shot("sh001", 0, [("一句很长的话", 9.0)])]
        out = split_overlong_shots(shots, LIMIT)
        assert len(out) == 1

    def test_没台词的镜头不动(self):
        s = Shot(shot_id="sh001", scene_id="s", order=0, duration_s=4.0,
                 shot_size=ShotSize.MS)
        out = split_overlong_shots([s], LIMIT)
        assert len(out) == 1


class TestStageSplitsLines:
    @pytest.mark.asyncio
    async def test_配音前先切开长台词(self, tmp_path):
        from changji.config import TTSConfig
        from changji.models.character import AssetLibrary
        from changji.models.project import ProjectStore
        from changji.stages.audio import AudioStage, EstimateBackend

        store = ProjectStore.create(tmp_path / "p", "d")
        long_line = ("这么晚了，是需要点什么吗？买点零食吧，"
                     "吃饱肚子才有力气继续等下去，别把自己熬坏了。")
        shot = Shot(shot_id="sh001", scene_id="s", order=0, duration_s=5.0,
                    shot_size=ShotSize.MS,
                    dialogue=[DialogueLine(text=long_line)])
        stage = AudioStage(EstimateBackend(), TTSConfig(), store.paths)
        await stage.run([shot], AssetLibrary())
        assert len(shot.dialogue) > 1, "长台词该在配音前就切开"
        for line in shot.dialogue:
            assert line.actual_duration_s <= max_line_seconds() + 0.3


class TestResplitAfterSynthesis:
    """估算不准是常态，合成之后还要再验一次。

    实测踩过：估 4.8 秒的一句，CosyVoice 出来 5.8 秒，
    多出来那一秒就盖到下一镜上去了。光靠字数估算挡不住。
    """

    class SlowBackend:
        """一个说话比估算慢四成的引擎。"""

        name = "slow"

        def __init__(self):
            self.calls = []

        async def available(self):
            return True

        async def synthesize(self, text, out_path, voice_id=None,
                             emotion="neutral", intensity=0.5):
            from changji.stages.audio import SynthesisResult, _write_silence
            self.calls.append(text)
            d = estimate_speech_duration(text) * 1.4
            out_path.parent.mkdir(parents=True, exist_ok=True)
            _write_silence(out_path, d, 24000)
            return SynthesisResult(duration_s=d, audio_path=out_path)

    @pytest.mark.asyncio
    async def test_合成偏长时重切再合成(self, tmp_path):
        from changji.config import TTSConfig
        from changji.models.character import AssetLibrary
        from changji.models.project import ProjectStore
        from changji.stages.audio import AudioStage

        store = ProjectStore.create(tmp_path / "p", "d")
        # 估算刚好压线，实际会超四成
        text = "这么晚了是需要点什么吗买点零食吧吃饱肚子才有力气"
        shot = Shot(shot_id="sh001", scene_id="s", order=0, duration_s=5.0,
                    shot_size=ShotSize.MS,
                    dialogue=[DialogueLine(text=text)])
        backend = self.SlowBackend()
        await AudioStage(backend, TTSConfig(), store.paths).run(
            [shot], AssetLibrary())

        limit = max_line_seconds()
        for line in shot.dialogue:
            assert line.actual_duration_s <= limit + 1e-6, \
                f"合成完还是装不下：{line.actual_duration_s} > {limit}"
        assert len(backend.calls) > 1, "该重切重合成一次"

    @pytest.mark.asyncio
    async def test_重切不丢字(self, tmp_path):
        from changji.config import TTSConfig
        from changji.models.character import AssetLibrary
        from changji.models.project import ProjectStore
        from changji.stages.audio import AudioStage

        store = ProjectStore.create(tmp_path / "p", "d")
        text = "这么晚了是需要点什么吗买点零食吧吃饱肚子才有力气继续等下去"
        shot = Shot(shot_id="sh001", scene_id="s", order=0, duration_s=5.0,
                    shot_size=ShotSize.MS,
                    dialogue=[DialogueLine(text=text)])
        await AudioStage(self.SlowBackend(), TTSConfig(), store.paths).run(
            [shot], AssetLibrary())
        assert "".join(d.text for d in shot.dialogue) == text

    @pytest.mark.asyncio
    async def test_每句都有自己的音频文件(self, tmp_path):
        from changji.config import TTSConfig
        from changji.models.character import AssetLibrary
        from changji.models.project import ProjectStore
        from changji.stages.audio import AudioStage

        store = ProjectStore.create(tmp_path / "p", "d")
        shot = Shot(shot_id="sh001", scene_id="s", order=0, duration_s=5.0,
                    shot_size=ShotSize.MS, dialogue=[DialogueLine(
                        text="这么晚了是需要点什么吗买点零食吧吃饱肚子才有力气")])
        await AudioStage(self.SlowBackend(), TTSConfig(), store.paths).run(
            [shot], AssetLibrary())
        paths = [d.audio_path for d in shot.dialogue]
        assert all(paths), "每句都得有音频"
        assert len(set(paths)) == len(paths), "文件名不能重号，重了会互相覆盖"


class TestNoDuplicateIds:
    """同一集重跑配音会再拆一次，编号不能撞车。

    实测踩过：第一次拆出 sh001_b，重跑一次又拆出一个 sh001_b。
    一集里两个同名镜头，按 id 找只找得到头一个，
    音频和首帧的文件名还会互相覆盖。
    """

    def test_重复拆不出重名(self):
        shots = [_shot("sh001", 0, [("一", 3.0), ("二", 3.0), ("三", 3.0)])]
        once = split_overlong_shots(shots, LIMIT)
        # 把台词塞回第一镜，模拟重跑配音后又需要拆
        once[0].dialogue = [
            DialogueLine(char_id="c_a", text=t, actual_duration_s=3.0,
                         audio_path=f"audio/x{i}.wav")
            for i, t in enumerate(["甲", "乙", "丙"])]
        twice = split_overlong_shots(once, LIMIT)
        ids = [s.shot_id for s in twice]
        assert len(ids) == len(set(ids)), f"编号撞车了：{ids}"

    def test_跳过已被占用的后缀(self):
        from changji.stages.audio import _free_shot_id
        assert _free_shot_id("sh001", {"sh001"}) == "sh001_b"
        assert _free_shot_id("sh001", {"sh001", "sh001_b"}) == "sh001_c"
        used = {"sh001"} | {f"sh001_{c}" for c in "bcdefghijklmnopqrstuvwxyz"}
        assert _free_shot_id("sh001", used) == "sh001_2"
