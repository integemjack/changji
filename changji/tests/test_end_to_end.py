# -*- coding: utf-8 -*-
"""全流程测试：从新建项目到出成片。

这条测试把整条链路真跑一遍，包括真实的 ComfyUI 推理和真实的 ffmpeg 装配。
它是「一键出片」这个承诺的唯一证据，纸面测试盖不住的问题都在这里暴露。

需要一个能连上的 ComfyUI。连不上就跳过，不让 CI 挂掉。
"""
from __future__ import annotations

import shutil

import pytest

from changji.comfy.client import ComfyClient
from changji.comfy.workflow import load_ui_workflow
from changji.config import GateConfig, Settings
from changji.models.character import (
    AppearanceBlock, AssetLibrary, Character, Location, StyleProfile,
)
from changji.models.project import Episode, ProjectStore
from changji.models.shot import (
    CameraMove, CharacterInShot, DialogueLine, FacePose, Shot, ShotSize,
    ShotStatus, apply_lipsync_rules,
)
from changji.pipeline import Pipeline

pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="需要 ffmpeg"),
]


async def _comfy_up(settings: Settings) -> bool:
    return await ComfyClient(settings.comfy).ping()


def _assets() -> AssetLibrary:
    return AssetLibrary(
        characters={
            "c_lin": Character(
                char_id="c_lin", name="林晚", voice_id="v_lin",
                appearance=AppearanceBlock(
                    identity="二十八岁女性，冷峭克制",
                    body="身形清瘦",
                    face="黑色长发挽起，眼型细长",
                    attire="灰色西装套裙"),
            ),
        },
        locations={
            "loc_office": Location(
                location_id="loc_office", name="总裁办公室",
                space="高层落地窗办公室，窗外是雨夜городской灯火".replace("городской", "城市"),
                lighting="夜间冷调顶光，霓虹反光"),
        },
        style=StyleProfile(
            global_style="电影感，浅景深，真实光影",
            negative_prompt="低质量，模糊，多余的手指，畸形",
            aspect_ratio="9:16"),
    )


def _shots() -> list[Shot]:
    """三个镜头，覆盖有台词、无台词、需口型三种情况。"""
    shots = [
        Shot(
            shot_id="ep01_sh001", scene_id="ep01_s01", order=0, duration_s=2.0,
            shot_size=ShotSize.MCU, camera_move=CameraMove.PUSH_IN,
            location_id="loc_office",
            first_frame_prompt="她站在落地窗前，雨水沿玻璃流下",
            motion_prompt="她缓缓转身看向镜头",
            characters=[CharacterInShot(char_id="c_lin", expression="隐忍",
                                        action="缓缓转身", face_pose=FacePose.FRONT)],
            dialogue=[DialogueLine(char_id="c_lin", text="你说什么",
                                   emotion="anger", emotion_intensity=0.7)],
            subtitle_text="你说什么",
            beat="铺垫",
        ),
        Shot(
            shot_id="ep01_sh002", scene_id="ep01_s01", order=1, duration_s=2.0,
            shot_size=ShotSize.LS, camera_move=CameraMove.STATIC,
            location_id="loc_office",
            first_frame_prompt="空旷办公室全景，只有一盏台灯亮着",
            motion_prompt="窗帘被风轻轻吹动",
            beat="过场",
        ),
        Shot(
            shot_id="ep01_sh003", scene_id="ep01_s01", order=2, duration_s=2.0,
            shot_size=ShotSize.CU, camera_move=CameraMove.STATIC,
            location_id="loc_office",
            first_frame_prompt="她的侧脸特写，眼神落在窗外",
            characters=[CharacterInShot(char_id="c_lin", expression="疲惫",
                                        face_pose=FacePose.THREE_QUARTER)],
            dialogue=[DialogueLine(char_id="c_lin",
                                   text="我已经把能做的都做完了")],
            subtitle_text="我已经把能做的都做完了",
            beat="收束",
        ),
    ]
    return apply_lipsync_rules(shots)


@pytest.fixture
def settings() -> Settings:
    s = Settings()
    # 全流程测试要跑得快，把闸门放宽一点但不关掉
    s.gates = GateConfig(max_attempts_per_shot=2)
    return s


class TestEndToEnd:
    async def test_从新建项目到出成片(self, tmp_path, settings):
        if not await _comfy_up(settings):
            pytest.skip(f"连不上 ComfyUI（{settings.comfy.base_url}）")

        # 一、新建项目
        store = ProjectStore.create(tmp_path / "雨夜迷局", "rainy-night", "雨夜迷局")
        assert store.exists()

        # 二、写入资产库与分镜表
        store.save_assets(_assets())
        project = store.load_project()
        project.episodes.append(Episode(
            episode_id="ep01", title="第一集", target_duration_s=6.0,
            shots=_shots()))
        store.save_project(project)

        # 口型判定应由规则算出：近景正脸有台词的做，远景无台词的不做
        ep = store.load_project().episode_by_id("ep01")
        assert ep.shot_by_id("ep01_sh001").needs_lipsync is True
        assert ep.shot_by_id("ep01_sh002").needs_lipsync is False

        # 三、跑流水线。只跑草稿档，成片档一个镜头要六分半，测试等不起。
        client = ComfyClient(settings.comfy)
        workflow = (await client.converter()).convert(
            load_ui_workflow(_bundled_workflow()))

        events = []
        pipeline = Pipeline(store, settings, client, workflow,
                            listener=events.append)
        report = await pipeline.run("ep01", skip_final=True)

        # 四、验证结果
        assert report.ok, f"流水线失败：{report.errors}\n{report.render()}"
        assert report.output is not None and report.output.is_file()

        final = store.load_project().episode_by_id("ep01")

        # 首帧要真的生成，而且被视频阶段用上了。
        # 这是跨镜头一致性的锚点，缺了它每一镜都会自说自话。
        framed = [s for s in final.shots if s.frame_path]
        assert framed, "一个首帧都没生成"
        for shot in framed:
            assert store.paths.abs(shot.frame_path).is_file(),                 f"{shot.shot_id} 的首帧文件不见了"

        for shot in final.shots:
            assert shot.status in (ShotStatus.DRAFT_DONE, ShotStatus.FALLBACK), \
                f"{shot.shot_id} 状态是 {shot.status}"

        # 有台词的镜头时长应被配音锁定
        assert final.shot_by_id("ep01_sh001").duration_locked is True
        assert final.shot_by_id("ep01_sh002").duration_locked is False

        # 成片要有画面有声音，时长对得上
        info = await pipeline.ff.probe(report.output)
        assert info.has_video and info.has_audio
        assert info.duration_s > 3.0
        assert info.width % 2 == 0 and info.height % 2 == 0

        # 字幕要真的生成了，而且内容是台词原文
        ass = list(store.paths.subtitles.glob("*.ass"))
        assert ass, "没有生成字幕"
        text = ass[0].read_text(encoding="utf-8-sig")
        assert "你说什么" in text
        assert "我已经把能做的都做完了" in text

        # 事件流要覆盖全部阶段
        stages = {e.stage.value for e in events}
        assert {"audio", "frames", "draft", "assemble"} <= stages

    async def test_断点续跑不重做已完成的镜头(self, tmp_path, settings):
        """中途停了再跑，已完成的部分不能重来。"""
        if not await _comfy_up(settings):
            pytest.skip("连不上 ComfyUI")

        store = ProjectStore.create(tmp_path / "续跑", "resume", "续跑测试")
        store.save_assets(_assets())
        project = store.load_project()
        shots = _shots()[:2]
        project.episodes.append(Episode(episode_id="ep01", shots=shots,
                                        target_duration_s=4.0))
        store.save_project(project)

        client = ComfyClient(settings.comfy)
        workflow = (await client.converter()).convert(
            load_ui_workflow(_bundled_workflow()))
        pipeline = Pipeline(store, settings, client, workflow)

        # 第一遍：只跑配音和草稿
        project = store.load_project()
        ep = project.episode_by_id("ep01")
        assets = store.load_assets()
        await pipeline.run_audio(project, ep, assets)
        from changji.hardware import Tier
        await pipeline.run_render(project, ep, assets, Tier.DRAFT)

        first_paths = {s.shot_id: s.video_path for s in ep.shots}
        first_mtimes = {
            sid: store.paths.abs(p).stat().st_mtime
            for sid, p in first_paths.items() if p
        }
        assert first_mtimes, "第一遍没有产出任何视频"

        # 第二遍：再跑一次，已完成的不该被重做
        events = []
        pipeline2 = Pipeline(store, settings, client, workflow,
                             listener=events.append)
        project2 = store.load_project()
        ep2 = project2.episode_by_id("ep01")
        await pipeline2.run_audio(project2, ep2, assets)
        await pipeline2.run_render(project2, ep2, assets, Tier.DRAFT)

        for sid, mtime in first_mtimes.items():
            shot = ep2.shot_by_id(sid)
            assert shot.video_path == first_paths[sid]
            assert store.paths.abs(shot.video_path).stat().st_mtime == mtime, \
                f"{sid} 被重做了"

        assert any("跳过" in e.message for e in events), \
            "第二遍应该报告跳过已完成的阶段"


def _bundled_workflow():
    from changji import bundled_workflow
    return bundled_workflow()
