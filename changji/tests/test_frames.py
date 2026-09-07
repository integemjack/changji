# -*- coding: utf-8 -*-
"""首帧生成。跨镜头一致性靠这一步锁死。"""
import pytest

from changji.comfy.workflow import ApiWorkflow, WorkflowError
from changji.models.character import (
    AppearanceBlock, AssetLibrary, Character, Location, StyleProfile,
)
from changji.models.project import ProjectStore
from changji.models.shot import CharacterInShot, Shot, ShotSize, ShotStatus
from changji.stages.frames import (
    FrameOutcome, FrameStage, _seed_for, _text_nodes,
)
from changji.stages.render import PromptComposer


def _assets():
    return AssetLibrary(
        characters={"c_lin": Character(
            char_id="c_lin", name="林晚",
            appearance=AppearanceBlock(
                identity="二十八岁女性", face="黑色长发挽起",
                attire="灰色西装套裙"),
            ref_front="refs/lin.png")},
        locations={"loc_office": Location(
            location_id="loc_office", name="办公室",
            space="高层落地窗办公室", lighting="夜间冷调顶光")},
        style=StyleProfile(global_style="电影感"),
    )


def _shot(sid="ep01_sh001", **kw):
    opts = dict(
        shot_id=sid, scene_id="ep01_s01", order=0, shot_size=ShotSize.MCU,
        location_id="loc_office", first_frame_prompt="她站在窗前",
        characters=[CharacterInShot(char_id="c_lin", expression="隐忍")],
    )
    opts.update(kw)
    return Shot(**opts)


class FakeBackend:
    """假后端。测编排逻辑不需要真跑模型。"""

    name = "fake"

    def __init__(self, fail_on: set[str] | None = None) -> None:
        self.fail_on = fail_on or set()
        self.seen: list[tuple[str, str]] = []

    async def generate(self, shot, prompts, spec, dest):
        self.seen.append((shot.shot_id, prompts.positive))
        if shot.shot_id in self.fail_on:
            from changji.stages.frames import FrameError
            raise FrameError("造出来的失败")
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(b"\x89PNG\r\n\x1a\n")
        return dest


class TestFrameStage:
    async def test_出首帧并回填路径(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        shot = _shot()
        stage = FrameStage(FakeBackend(), PromptComposer(_assets()), store.paths)
        from changji.hardware import tiers_for_vram, Tier
        spec = tiers_for_vram(16.0)[Tier.DRAFT]

        outcomes = await stage.run([shot], spec)
        assert outcomes[0].ok
        assert shot.frame_path == "frames/ep01_sh001.png"
        assert shot.status is ShotStatus.FRAME_DONE
        assert store.paths.abs(shot.frame_path).is_file()

    async def test_首帧提示词含身份层(self, tmp_path):
        """首帧是一致性的锚点，身份层必须在里面。"""
        store = ProjectStore.create(tmp_path / "p", "p")
        backend = FakeBackend()
        stage = FrameStage(backend, PromptComposer(_assets()), store.paths)
        from changji.hardware import tiers_for_vram, Tier
        await stage.run([_shot()], tiers_for_vram(16.0)[Tier.DRAFT])
        _, prompt = backend.seen[0]
        assert "黑色长发挽起" in prompt
        assert "灰色西装套裙" in prompt
        assert "高层落地窗办公室" in prompt

    async def test_失败的镜头状态不前进(self, tmp_path):
        """状态停在原地，渲染阶段才能识别出它要退回纯文生。"""
        store = ProjectStore.create(tmp_path / "p", "p")
        shot = _shot()
        shot.status = ShotStatus.AUDIO_DONE
        stage = FrameStage(FakeBackend(fail_on={"ep01_sh001"}),
                           PromptComposer(_assets()), store.paths)
        from changji.hardware import tiers_for_vram, Tier
        outcomes = await stage.run([shot], tiers_for_vram(16.0)[Tier.DRAFT])
        assert not outcomes[0].ok
        assert shot.status is ShotStatus.AUDIO_DONE
        assert shot.frame_path is None
        assert shot.attempts == 1

    async def test_一个失败不影响其它镜头(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        shots = [_shot("ep01_sh001"), _shot("ep01_sh002", order=1),
                 _shot("ep01_sh003", order=2)]
        stage = FrameStage(FakeBackend(fail_on={"ep01_sh002"}),
                           PromptComposer(_assets()), store.paths)
        from changji.hardware import tiers_for_vram, Tier
        outcomes = await stage.run(shots, tiers_for_vram(16.0)[Tier.DRAFT])
        assert [o.ok for o in outcomes] == [True, False, True]

    async def test_竖屏尺寸被应用(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        seen = {}

        class SizeSpy(FakeBackend):
            async def generate(self, shot, prompts, spec, dest):
                seen["size"] = (spec.width, spec.height)
                return await super().generate(shot, prompts, spec, dest)

        from changji.hardware import tiers_for_vram, Tier
        await FrameStage(SizeSpy(), PromptComposer(_assets()), store.paths).run(
            [_shot()], tiers_for_vram(16.0)[Tier.DRAFT], aspect_ratio="9:16")
        assert seen["size"][1] > seen["size"][0]


class TestSeed:
    def test_不同镜头种子不同(self):
        assert _seed_for(_shot("ep01_sh001")) != _seed_for(_shot("ep01_sh002"))

    def test_重试时种子会变(self):
        s = _shot()
        first = _seed_for(s)
        s.attempts = 1
        assert _seed_for(s) != first

    def test_首帧与视频种子不同(self):
        """两者用不同偏移，避免同时撞上同一个坏种子。"""
        from changji.stages.render import _seed_for as video_seed
        s = _shot()
        assert _seed_for(s) != video_seed(s)


class TestTextNodes:
    def test_按连线找正负提示词(self):
        wf = ApiWorkflow({
            "9": {"class_type": "KSampler", "inputs": {
                "positive": ["5", 0], "negative": ["6", 0]}},
            "5": {"class_type": "CLIPTextEncode", "inputs": {"text": "正"}},
            "6": {"class_type": "CLIPTextEncode", "inputs": {"text": "负"}},
        })
        assert _text_nodes(wf) == ("5", "6")

    def test_没接提示词时报错(self):
        wf = ApiWorkflow({"9": {"class_type": "KSampler", "inputs": {}}})
        with pytest.raises(WorkflowError, match="正负提示词"):
            _text_nodes(wf)
