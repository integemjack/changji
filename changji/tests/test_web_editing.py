# -*- coding: utf-8 -*-
"""界面的编辑与分阶段执行。

界面的价值在于审片和改分镜，这两件事命令行干不了。改完之后状态要
正确回退，否则下次运行会跳过被改的镜头，用户会以为改动没生效。
"""
import json

import pytest
from fastapi.testclient import TestClient

from changji.config import Settings
from changji.models.character import (
    AppearanceBlock, AssetLibrary, Character, Location,
)
from changji.models.project import Episode, ProjectStore
from changji.models.shot import (
    CharacterInShot, DialogueLine, Shot, ShotSize, ShotStatus, Transition,
)
from changji.web.server import create_app


@pytest.fixture
def project(tmp_path):
    store = ProjectStore.create(tmp_path / "剧", "drama", "测试剧")
    store.save_assets(AssetLibrary(
        characters={"c_lin": Character(
            char_id="c_lin", name="林晚",
            appearance=AppearanceBlock(identity="女性", face="长发", attire="西装"))},
        locations={"loc_a": Location(location_id="loc_a", name="办公室",
                                     space="房间", lighting="冷光")},
    ))
    p = store.load_project()
    p.episodes.append(Episode(
        episode_id="ep01", script="林晚：我走了。", target_duration_s=10.0,
        shots=[
            Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=3.0,
                 shot_size=ShotSize.MS, first_frame_prompt="她站在窗前",
                 status=ShotStatus.DRAFT_DONE, attempts=2,
                 characters=[CharacterInShot(char_id="c_lin")],
                 dialogue=[DialogueLine(char_id="c_lin", text="我走了。",
                                        actual_duration_s=1.5,
                                        audio_path="audio/a.wav")]),
            Shot(shot_id="ep01_sh002", scene_id="s1", order=1, duration_s=2.0,
                 status=ShotStatus.FINAL_DONE),
        ]))
    store.save_project(p)
    return store


@pytest.fixture
def client(project):
    return TestClient(create_app(Settings()))


def _reload(store, shot_id="ep01_sh001"):
    return store.load_project().episode_by_id("ep01").shot_by_id(shot_id)


class TestScriptEditing:
    def test_读剧本(self, client, project):
        r = client.get("/api/script", params={
            "path": str(project.root), "episode_id": "ep01"})
        assert r.status_code == 200
        assert r.json()["script"] == "林晚：我走了。"

    def test_存剧本(self, client, project):
        r = client.post("/api/script", json={
            "project": str(project.root), "episode_id": "ep01",
            "script": "改过的剧本", "regenerate": False})
        assert r.status_code == 200
        assert r.json()["regenerated"] is False
        ep = project.load_project().episode_by_id("ep01")
        assert ep.script == "改过的剧本"
        assert len(ep.shots) == 2, "没勾选重出时不该动分镜"

    def test_改目标时长(self, client, project):
        client.post("/api/script", json={
            "project": str(project.root), "episode_id": "ep01",
            "script": "剧本", "duration_s": 30.0})
        assert project.load_project().episode_by_id("ep01").target_duration_s == 30.0

    def test_剧集不存在时报错(self, client, project):
        r = client.post("/api/script", json={
            "project": str(project.root), "episode_id": "ep99", "script": "x"})
        assert r.status_code == 404


class TestShotEditing:
    def _patch(self, client, project, patch, shot_id="ep01_sh001"):
        return client.post("/api/shot", json={
            "project": str(project.root), "episode_id": "ep01",
            "shot_id": shot_id, "patch": patch})

    def test_改提示词后状态退回未开工(self, client, project):
        """不退回的话下次运行会跳过它，用户以为改动没生效。"""
        r = self._patch(client, project, {"first_frame_prompt": "改成雨夜"})
        assert r.status_code == 200
        assert r.json()["reset_to_planned"] is True
        shot = _reload(project)
        assert shot.first_frame_prompt == "改成雨夜"
        assert shot.status is ShotStatus.PLANNED
        assert shot.attempts == 0

    def test_改字幕不退回状态(self, client, project):
        """字幕不影响画面，没必要重渲染。"""
        r = self._patch(client, project, {"subtitle_text": "新字幕"})
        assert r.json()["reset_to_planned"] is False
        assert _reload(project).status is ShotStatus.DRAFT_DONE

    def test_改台词会清掉配音(self, client, project):
        """台词变了，原来的配音和锁定的时长都作废。"""
        r = self._patch(client, project, {"dialogue_texts": ["我不走了。"]})
        assert r.status_code == 200
        shot = _reload(project)
        assert shot.dialogue[0].text == "我不走了。"
        assert shot.dialogue[0].actual_duration_s is None
        assert shot.dialogue[0].audio_path is None
        assert shot.duration_locked is False

    def test_台词条数对不上会报错(self, client, project):
        r = self._patch(client, project, {"dialogue_texts": ["一", "二"]})
        assert r.status_code == 400
        assert "条数对不上" in r.json()["detail"]

    def test_不合法的改动被拒(self, client, project):
        """硬切不能有转场时长，这条约束不因为走界面就放宽。"""
        r = self._patch(client, project, {
            "transition_in": "cut", "transition_dur_s": 0.4})
        assert r.status_code == 400

    def test_改不了角色外观(self, client, project):
        """一致性靠资产库统一管理，不因为加了编辑就松动。"""
        r = self._patch(client, project, {"face": "换个发型"})
        assert r.status_code == 422

    def test_可以手动锁定状态(self, client, project):
        """人工确认过的镜头标成 locked，后续不再重跑。"""
        r = self._patch(client, project, {"status": "locked"})
        assert r.status_code == 200
        assert _reload(project).status is ShotStatus.LOCKED

    def test_镜头不存在时报错(self, client, project):
        r = self._patch(client, project, {"beat": "x"}, shot_id="ep01_sh999")
        assert r.status_code == 404


class TestSettings:
    def test_读参数(self, client):
        d = client.get("/api/settings").json()
        assert "draft" in d["tiers"]
        assert d["assembly"]["fps"] > 0
        assert "max_attempts_per_shot" in d["gates"]

    def test_改装配参数(self, client):
        r = client.post("/api/settings", json={"fps": 30, "crf": 20})
        assert r.status_code == 200
        assert set(r.json()["changed"]) == {"fps", "crf"}
        assert client.get("/api/settings").json()["assembly"]["fps"] == 30

    def test_改闸门参数(self, client):
        client.post("/api/settings", json={"max_attempts_per_shot": 5,
                                           "gates_enabled": False})
        g = client.get("/api/settings").json()["gates"]
        assert g["max_attempts_per_shot"] == 5
        assert g["enabled"] is False

    def test_分辨率必须是32的倍数(self, client):
        """不是的话潜空间对不齐，生成会失败或出怪东西。"""
        r = client.post("/api/settings", json={"draft_width": 641})
        assert r.status_code == 400
        assert "32" in r.json()["detail"]

    def test_未知参数被拒(self, client):
        assert client.post("/api/settings", json={"不存在": 1}).status_code == 422


class TestStagedRun:
    """分阶段执行。直接测校验逻辑，不依赖异步任务的时序。"""

    async def test_阶段名非法时报错(self, tmp_path):
        from changji.models.project import ProjectStore
        from changji.pipeline import Pipeline
        store = ProjectStore.create(tmp_path / "p", "p")
        pr = store.load_project()
        pr.episodes.append(Episode(episode_id="ep01", shots=[
            Shot(shot_id="ep01_sh001", scene_id="s1", order=0)]))
        store.save_project(pr)

        pipe = Pipeline(store, Settings(), None, None)
        # 阶段名写错是调用方的问题，立刻抛出比记进报告更直接
        with pytest.raises(ValueError, match="不认识的阶段"):
            await pipe.run_stages("ep01", ["不存在的阶段"])

    async def test_合法阶段名被接受(self, tmp_path):
        """只跑装配这一段，没有视频时应报缺素材而不是报阶段名错。"""
        from changji.models.project import ProjectStore
        from changji.pipeline import Pipeline
        store = ProjectStore.create(tmp_path / "p", "p")
        pr = store.load_project()
        pr.episodes.append(Episode(episode_id="ep01", shots=[
            Shot(shot_id="ep01_sh001", scene_id="s1", order=0)]))
        store.save_project(pr)

        pipe = Pipeline(store, Settings(), None, None)
        report = await pipe.run_stages("ep01", ["assemble"])
        assert report.errors
        assert "不认识的阶段" not in report.errors[0]

    async def test_没有分镜时明确报错(self, tmp_path):
        from changji.models.project import ProjectStore
        from changji.pipeline import Pipeline
        store = ProjectStore.create(tmp_path / "p", "p")
        pr = store.load_project()
        pr.episodes.append(Episode(episode_id="ep01"))
        store.save_project(pr)

        pipe = Pipeline(store, Settings(), None, None)
        with pytest.raises(ValueError, match="还没有分镜表"):
            await pipe.run_stages("ep01", ["audio"])


class TestShotsApiCompleteness:
    """回归测试：分镜接口必须返回编辑器要回填的所有字段。

    缺了 first_frame_prompt 时，编辑器打开是空的，用户以为本来就没内容，
    一保存就把原提示词清空了。这类问题只有真在界面上点一遍才会发现。
    """

    def test_返回编辑器需要的字段(self, client, project):
        r = client.get("/api/shots", params={
            "path": str(project.root), "episode_id": "ep01"})
        assert r.status_code == 200
        shot = r.json()["shots"][0]
        required = {
            "shot_id", "shot_size", "camera_angle", "camera_move",
            "first_frame_prompt", "motion_prompt", "negative_prompt",
            "subtitle_text", "duration_s", "transition_in",
            "transition_dur_s", "needs_lipsync", "status", "dialogue",
        }
        missing = required - set(shot)
        assert not missing, f"编辑器要用但接口没返回：{sorted(missing)}"

    def test_提示词内容真的返回(self, client, project):
        r = client.get("/api/shots", params={
            "path": str(project.root), "episode_id": "ep01"})
        shot = r.json()["shots"][0]
        assert shot["first_frame_prompt"] == "她站在窗前"

    def test_编辑器可改的字段接口都能读回(self, client, project):
        """写得进去就得读得出来，否则改一次丢一次。"""
        from changji.web.server import ShotPatch
        r = client.get("/api/shots", params={
            "path": str(project.root), "episode_id": "ep01"})
        shot = r.json()["shots"][0]
        writable = set(ShotPatch.model_fields) - {"dialogue_texts", "status",
                                                  "visual_desc", "beat"}
        missing = writable - set(shot)
        assert not missing, f"能改但读不回来：{sorted(missing)}"


class TestMediaAndPreview:
    """审片相关：分镜要能看首帧和视频，成片要能在界面里播。"""

    def test_分镜返回媒体路径(self, client, project):
        """只报 has_video 的话界面拿不到文件，看不了片。"""
        r = client.get("/api/shots", params={
            "path": str(project.root), "episode_id": "ep01"})
        shot = r.json()["shots"][0]
        assert "frame_path" in shot
        assert "video_path" in shot
        assert "audio_paths" in shot
        assert shot["audio_paths"] == ["audio/a.wav"]

    def test_取项目内的文件(self, client, project):
        f = project.paths.frames / "t.png"
        f.write_bytes(b"\x89PNG\r\n\x1a\n")
        r = client.get("/api/media", params={
            "path": str(project.root), "rel": "frames/t.png"})
        assert r.status_code == 200
        assert r.content.startswith(b"\x89PNG")

    def test_不存在的文件返回404(self, client, project):
        r = client.get("/api/media", params={
            "path": str(project.root), "rel": "frames/没有.png"})
        assert r.status_code == 404

    def test_拒绝读项目外的文件(self, client, project):
        """路径穿越必须被挡住，否则能读到服务器上任意文件。"""
        for evil in ("../../../etc/passwd", r"..\..\windows\win.ini",
                     "../secret.txt"):
            r = client.get("/api/media", params={
                "path": str(project.root), "rel": evil})
            assert r.status_code in (403, 404), f"{evil} 没被挡住"

    def test_列成片(self, client, project):
        (project.paths.output / "ep01.mp4").write_bytes(b"\x00" * 2048)
        r = client.get("/api/outputs", params={"path": str(project.root)})
        assert r.status_code == 200
        files = r.json()["files"]
        assert len(files) == 1
        assert files[0]["name"] == "ep01.mp4"
        assert files[0]["rel"] == "output/ep01.mp4"

    def test_没有成片时返回空列表(self, client, project):
        r = client.get("/api/outputs", params={"path": str(project.root)})
        assert r.json()["files"] == []

    def test_成片按时间倒序(self, client, project):
        import os, time
        for i, name in enumerate(("old.mp4", "new.mp4")):
            f = project.paths.output / name
            f.write_bytes(b"\x00" * 1024)
            os.utime(f, (time.time() + i * 100, time.time() + i * 100))
        names = [f["name"] for f in
                 client.get("/api/outputs",
                            params={"path": str(project.root)}).json()["files"]]
        assert names[0] == "new.mp4", "最新的应该排在前面"


class TestBatchOperations:
    """批量操作。量产时一集几十个镜头，一个个点太慢。"""

    def _batch(self, client, project, action, ids=None):
        return client.post("/api/shots/batch", json={
            "project": str(project.root), "episode_id": "ep01",
            "shot_ids": ids or [], "action": action})

    def test_批量重置整集(self, client, project):
        r = self._batch(client, project, "reset")
        assert r.status_code == 200
        assert r.json()["changed"] == 2
        for sid in ("ep01_sh001", "ep01_sh002"):
            assert _reload(project, sid).status is ShotStatus.PLANNED

    def test_批量重置指定镜头(self, client, project):
        r = self._batch(client, project, "reset", ["ep01_sh001"])
        assert r.json()["changed"] == 1
        assert _reload(project, "ep01_sh001").status is ShotStatus.PLANNED
        assert _reload(project, "ep01_sh002").status is ShotStatus.FINAL_DONE

    def test_重置会清掉重试次数和闸门备注(self, client, project):
        self._batch(client, project, "reset", ["ep01_sh001"])
        shot = _reload(project, "ep01_sh001")
        assert shot.attempts == 0
        assert shot.gate_notes == []

    def test_批量重置不动已锁定的镜头(self, client, project):
        """锁定的是人工确认过的，一次误操作不该把审过的片全废了。"""
        self._batch(client, project, "lock", ["ep01_sh001"])
        r = self._batch(client, project, "reset")
        assert _reload(project, "ep01_sh001").status is ShotStatus.LOCKED
        assert _reload(project, "ep01_sh002").status is ShotStatus.PLANNED
        assert r.json()["changed"] == 1

    def test_批量锁定(self, client, project):
        r = self._batch(client, project, "lock")
        assert r.json()["changed"] == 2
        assert _reload(project).status is ShotStatus.LOCKED

    def test_解锁有视频的回到已完成(self, client, project):
        """解锁后不该退回未开工，那样会白白重跑一遍。"""
        p = project.load_project()
        shot = p.episodes[0].shot_by_id("ep01_sh001")
        shot.video_path = "shots/final/a.mp4"
        shot.status = ShotStatus.LOCKED
        project.save_project(p)

        self._batch(client, project, "unlock", ["ep01_sh001"])
        assert _reload(project, "ep01_sh001").status is ShotStatus.FINAL_DONE

    def test_解锁没视频的退回未开工(self, client, project):
        self._batch(client, project, "lock", ["ep01_sh002"])
        self._batch(client, project, "unlock", ["ep01_sh002"])
        assert _reload(project, "ep01_sh002").status is ShotStatus.PLANNED

    def test_清除闸门备注(self, client, project):
        p = project.load_project()
        p.episodes[0].shot_by_id("ep01_sh001").gate_notes = ["亮度跳变"]
        project.save_project(p)

        r = self._batch(client, project, "clear_notes")
        assert r.json()["changed"] == 1
        assert _reload(project).gate_notes == []

    def test_不认识的操作被拒(self, client, project):
        r = self._batch(client, project, "删库")
        assert r.status_code == 400
        assert "不认识的操作" in r.json()["detail"]

    def test_镜头不存在时报错点名(self, client, project):
        r = self._batch(client, project, "lock", ["ep01_sh999"])
        assert r.status_code == 404
        assert "ep01_sh999" in r.json()["detail"]

    def test_重复操作不重复计数(self, client, project):
        """已经是目标状态的不该算作改动，否则数字会骗人。"""
        self._batch(client, project, "lock")
        assert self._batch(client, project, "lock").json()["changed"] == 0


class TestEpisodeManagement:
    """剧集管理。量产时一个项目会有很多集，共用同一套角色和场景设定。"""

    def test_新建自动编号(self, client, project):
        """量产时不该逼用户自己想 id。"""
        r = client.post("/api/episode", json={
            "project": str(project.root), "title": "第二集"})
        assert r.status_code == 200
        assert r.json()["episode_id"] == "ep02"

    def test_连续新建不撞号(self, client, project):
        ids = [client.post("/api/episode", json={
            "project": str(project.root)}).json()["episode_id"]
            for _ in range(3)]
        assert len(set(ids)) == 3

    def test_重复id被拒(self, client, project):
        r = client.post("/api/episode", json={
            "project": str(project.root), "episode_id": "ep01"})
        assert r.status_code == 409

    def test_非法id被拒(self, client, project):
        r = client.post("/api/episode", json={
            "project": str(project.root), "episode_id": "第二集"})
        assert r.status_code == 400
        assert "小写字母" in r.json()["detail"]

    def _act(self, client, project, action, ep="ep01", title=""):
        return client.post("/api/episode/action", json={
            "project": str(project.root), "episode_id": ep,
            "action": action, "new_title": title})

    def test_复制不带产出物和状态(self, client, project):
        """复制出来的是要重跑的，带上状态会显示成已完成但没有文件。"""
        p = project.load_project()
        shot = p.episodes[0].shot_by_id("ep01_sh001")
        shot.video_path = "shots/final/a.mp4"
        shot.frame_path = "frames/a.png"
        project.save_project(p)

        r = self._act(client, project, "duplicate")
        assert r.status_code == 200
        new_id = r.json()["episode_id"]

        new_ep = project.load_project().episode_by_id(new_id)
        assert len(new_ep.shots) == 2
        for s in new_ep.shots:
            assert s.status is ShotStatus.PLANNED
            assert s.video_path is None
            assert s.frame_path is None
            assert s.attempts == 0
            assert s.duration_locked is False
            for line in s.dialogue:
                assert line.audio_path is None
                assert line.actual_duration_s is None

    def test_复制带走剧本和分镜文案(self, client, project):
        new_id = self._act(client, project, "duplicate").json()["episode_id"]
        new_ep = project.load_project().episode_by_id(new_id)
        assert new_ep.script == "林晚：我走了。"
        assert new_ep.shot_by_id(f"{new_id}_sh001").first_frame_prompt == "她站在窗前"

    def test_复制后镜号带新剧集前缀(self, client, project):
        new_id = self._act(client, project, "duplicate").json()["episode_id"]
        new_ep = project.load_project().episode_by_id(new_id)
        assert all(s.shot_id.startswith(new_id) for s in new_ep.shots)

    def test_改名(self, client, project):
        r = self._act(client, project, "rename", title="雨夜")
        assert r.status_code == 200
        assert project.load_project().episode_by_id("ep01").title == "雨夜"

    def test_删除(self, client, project):
        client.post("/api/episode", json={"project": str(project.root)})
        r = self._act(client, project, "delete")
        assert r.status_code == 200
        assert project.load_project().episode_by_id("ep01") is None

    def test_最后一集不能删(self, client, project):
        """删光了项目就没有可操作的对象，界面会变成空壳。"""
        r = self._act(client, project, "delete")
        assert r.status_code == 400
        assert "至少要留一集" in r.json()["detail"]

    def test_不认识的操作被拒(self, client, project):
        assert self._act(client, project, "清空").status_code == 400

    def test_剧集不存在时报错(self, client, project):
        assert self._act(client, project, "delete", ep="ep99").status_code == 404


class TestAssetEditing:
    """角色与场景编辑。

    外观是一致性的锚点，所有镜头共用同一段文字。改了它等于全剧的
    提示词都变了，已完成的镜头必须退回重跑，否则同一个角色前后长得不一样。
    """

    def test_读完整设定(self, client, project):
        d = client.get("/api/assets",
                       params={"path": str(project.root)}).json()
        c = d["characters"][0]
        assert c["char_id"] == "c_lin"
        assert c["face"] == "长发"
        assert "rendered" in c, "要显示拼出来的最终提示词"
        assert d["locations"][0]["location_id"] == "loc_a"
        assert "global_style" in d["style"]

    def _char(self, client, project, patch, reset=True):
        return client.post("/api/character", json={
            "project": str(project.root), "char_id": "c_lin",
            "patch": patch, "reset_shots": reset})

    def test_改外观后镜头退回重跑(self, client, project):
        r = self._char(client, project, {"face": "黑色短发，方脸"})
        assert r.status_code == 200
        assert r.json()["reset_shots"] == 2
        for sid in ("ep01_sh001", "ep01_sh002"):
            assert _reload(project, sid).status is ShotStatus.PLANNED

    def test_改外观后拼出的提示词跟着变(self, client, project):
        r = self._char(client, project, {"face": "黑色短发"})
        assert "黑色短发" in r.json()["rendered"]
        assert "长发" not in r.json()["rendered"]

    def test_改音色不重置镜头(self, client, project):
        """音色不影响画面，重置了会白跑一遍。"""
        r = self._char(client, project, {"voice_id": "v_new"})
        assert r.json()["reset_shots"] == 0
        assert _reload(project).status is ShotStatus.DRAFT_DONE

    def test_可以关掉自动重置(self, client, project):
        r = self._char(client, project, {"face": "短发"}, reset=False)
        assert r.json()["reset_shots"] == 0

    def test_重置不动锁定的镜头(self, client, project):
        client.post("/api/shots/batch", json={
            "project": str(project.root), "episode_id": "ep01",
            "shot_ids": ["ep01_sh001"], "action": "lock"})
        r = self._char(client, project, {"face": "短发"})
        assert _reload(project, "ep01_sh001").status is ShotStatus.LOCKED
        assert r.json()["reset_shots"] == 1

    def test_角色不存在时报错(self, client, project):
        r = client.post("/api/character", json={
            "project": str(project.root), "char_id": "c_ghost",
            "patch": {"face": "x"}})
        assert r.status_code == 404

    def test_未知字段被拒(self, client, project):
        assert self._char(client, project, {"发型": "短"}).status_code == 422

    def test_改场景后镜头退回重跑(self, client, project):
        r = client.post("/api/location", json={
            "project": str(project.root), "location_id": "loc_a",
            "patch": {"lighting": "白天自然光"}, "reset_shots": True})
        assert r.status_code == 200
        assert r.json()["reset_shots"] == 2
        assert "白天自然光" in r.json()["rendered"]

    def test_改场景名不重置(self, client, project):
        r = client.post("/api/location", json={
            "project": str(project.root), "location_id": "loc_a",
            "patch": {"name": "会议室"}, "reset_shots": True})
        assert r.json()["reset_shots"] == 0

    def test_改全剧风格(self, client, project):
        r = client.post("/api/style", json={
            "project": str(project.root),
            "patch": {"global_style": "冷调，高对比"}, "reset_shots": True})
        assert r.status_code == 200
        assert r.json()["reset_shots"] == 2
        d = client.get("/api/assets", params={"path": str(project.root)}).json()
        assert d["style"]["global_style"] == "冷调，高对比"

    def test_改画幅(self, client, project):
        client.post("/api/style", json={
            "project": str(project.root), "patch": {"aspect_ratio": "16:9"}})
        d = client.get("/api/assets", params={"path": str(project.root)}).json()
        assert d["style"]["aspect_ratio"] == "16:9"

    def test_外观改动落到分镜提示词上(self, client, project):
        """端到端验证：改了资产库，渲染出的提示词真的跟着变。"""
        from changji.stages.render import PromptComposer
        self._char(client, project, {"face": "银色短发，凤眼"})
        assets = project.load_assets()
        shot = _reload(project)
        p = PromptComposer(assets).compose(shot)
        assert "银色短发，凤眼" in p.positive
        assert "长发" not in p.positive


class TestSettingsRange:
    """参数有范围，界面填过界要挡住，而且不能改一半。"""

    def test_闸门相似度能改(self, client):
        r = client.post("/api/settings", json={"min_frame_similarity": 0.7})
        assert r.status_code == 200
        assert r.json()["changed"] == ["min_frame_similarity"]
        assert client.get("/api/settings").json()[
            "gates"]["min_frame_similarity"] == 0.7

    def test_配音容差能改(self, client):
        r = client.post("/api/settings", json={"tts_tolerance_s": 0.4})
        assert r.status_code == 200
        assert client.get("/api/settings").json()["tts"]["tolerance_s"] == 0.4

    def test_相似度超过一要挡住(self, client):
        before = client.get("/api/settings").json()["gates"]["min_frame_similarity"]
        r = client.post("/api/settings", json={"min_frame_similarity": 2.0})
        assert r.status_code == 400
        assert "与首帧相似度下限" in r.json()["detail"]
        after = client.get("/api/settings").json()["gates"]["min_frame_similarity"]
        assert after == before, "校验没过就得整体回滚，不能改一半"

    def test_一批里有一个越界整批都不生效(self, client):
        r = client.post("/api/settings", json={
            "crf": 20, "min_frame_similarity": 5.0})
        assert r.status_code == 400
        assert client.get("/api/settings").json()["assembly"]["crf"] == 18

    def test_降级开关能关(self, client):
        client.post("/api/settings", json={"fallback_on_exhausted": False})
        assert client.get("/api/settings").json()[
            "gates"]["fallback_on_exhausted"] is False

    def test_字幕行数上限是三(self, client):
        assert client.post("/api/settings",
                           json={"subtitle_max_lines": 4}).status_code == 400
        assert client.post("/api/settings",
                           json={"subtitle_max_lines": 2}).status_code == 200

    def test_回执用中文字段名(self, client):
        d = client.post("/api/settings",
                        json={"min_frame_similarity": 0.7}).json()
        assert d["labels"] == ["与首帧相似度下限"]


class TestNoPointlessReset:
    """提交里带着某个字段，不等于用户改了它。

    界面一次提交整张表单。光看字段在不在的话，改个音色也会把全剧
    镜头退回重跑，几十分钟渲染好的成片档白白重来一遍。
    """

    def _appearance(self, project):
        c = project.load_assets().characters["c_lin"]
        return {"name": c.name, "identity": c.appearance.identity,
                "body": c.appearance.body, "face": c.appearance.face,
                "attire": c.appearance.attire, "style": c.appearance.style}

    def test_只改音色不重置(self, client, project):
        patch = self._appearance(project)
        patch["voice_id"] = "voices_examples/female/female_01.wav"
        d = client.post("/api/character", json={
            "project": str(project.root), "char_id": "c_lin",
            "patch": patch}).json()
        assert d["reset_shots"] == 0, "外观一个字没动，不该退回重跑"
        assert _reload(project).status.value == "draft_done"

    def test_原样提交外观不重置(self, client, project):
        d = client.post("/api/character", json={
            "project": str(project.root), "char_id": "c_lin",
            "patch": self._appearance(project)}).json()
        assert d["reset_shots"] == 0

    def test_真改了外观才重置(self, client, project):
        patch = self._appearance(project)
        patch["attire"] = "米白色大衣"
        d = client.post("/api/character", json={
            "project": str(project.root), "char_id": "c_lin",
            "patch": patch}).json()
        assert d["reset_shots"] > 0
        assert _reload(project).status.value == "planned"

    def test_场景原样提交不重置(self, client, project):
        loc = project.load_assets().locations["loc_a"]
        d = client.post("/api/location", json={
            "project": str(project.root), "location_id": "loc_a",
            "patch": {"name": loc.name, "space": loc.space,
                      "lighting": loc.lighting,
                      "palette": loc.palette}}).json()
        assert d["reset_shots"] == 0

    def test_风格原样提交不重置(self, client, project):
        st = project.load_assets().style
        d = client.post("/api/style", json={
            "project": str(project.root), "reset_shots": True,
            "patch": {"global_style": st.global_style,
                      "negative_prompt": st.negative_prompt,
                      "aspect_ratio": st.aspect_ratio}}).json()
        assert d["reset_shots"] == 0

    def test_风格真改了才重置(self, client, project):
        st = project.load_assets().style
        d = client.post("/api/style", json={
            "project": str(project.root), "reset_shots": True,
            "patch": {"global_style": "换个风格",
                      "negative_prompt": st.negative_prompt,
                      "aspect_ratio": st.aspect_ratio}}).json()
        assert d["reset_shots"] > 0


class TestProjectList:
    """项目库要能列出来。

    在容器里跑的时候项目路径是 /data/projects/剧名，
    只给一个输入框让人手打，等于这个功能没法用。
    """

    def _client(self, tmp_path):
        from changji.config import Settings
        from changji.web.server import create_app
        return TestClient(create_app(Settings(workspace=str(tmp_path))))

    def test_空项目库也给出路径(self, tmp_path):
        d = self._client(tmp_path).get("/api/projects").json()
        assert d["projects"] == []
        assert d["workspace"] == str(tmp_path)

    def test_列出项目和进度(self, tmp_path):
        from changji.models.project import Episode
        from changji.models.shot import Shot, ShotSize, ShotStatus

        store = ProjectStore.create(tmp_path / "雪夜", "xy", "雪夜")
        p = store.load_project()
        p.episodes.append(Episode(episode_id="ep01", shots=[
            Shot(shot_id="a", scene_id="s", order=0, duration_s=3.0,
                 shot_size=ShotSize.MS, status=ShotStatus.FINAL_DONE),
            Shot(shot_id="b", scene_id="s", order=1, duration_s=3.0,
                 shot_size=ShotSize.MS),
        ]))
        store.save_project(p)

        d = self._client(tmp_path).get("/api/projects").json()
        assert len(d["projects"]) == 1
        item = d["projects"][0]
        assert item["name"] == "雪夜"
        assert item["episodes"] == 1
        assert item["shots"] == 2
        assert item["done_shots"] == 1

    def test_不是项目的目录不列(self, tmp_path):
        (tmp_path / "随手建的空目录").mkdir()
        ProjectStore.create(tmp_path / "真项目", "z", "真项目")
        d = self._client(tmp_path).get("/api/projects").json()
        assert [p["name"] for p in d["projects"]] == ["真项目"]

    def test_坏掉的项目也列出来并说明(self, tmp_path):
        store = ProjectStore.create(tmp_path / "坏的", "h")
        store.paths.project_file.write_text("{ 这不是 json", encoding="utf-8")
        d = self._client(tmp_path).get("/api/projects").json()
        assert d["projects"][0]["broken"], "坏了也得列出来，不然用户以为项目丢了"

    def test_新建只填名字就落在项目库下(self, tmp_path):
        c = self._client(tmp_path)
        d = c.post("/api/new", json={"path": "夏夜"}).json()
        assert d["root"] == str(tmp_path / "夏夜")
        assert (tmp_path / "夏夜" / "project.json").is_file()

    def test_新建仍然接受绝对路径(self, tmp_path):
        c = self._client(tmp_path)
        target = tmp_path / "别处" / "剧"
        d = c.post("/api/new", json={"path": str(target)}).json()
        assert d["root"] == str(target)

    def test_名字为空要报错(self, tmp_path):
        assert self._client(tmp_path).post(
            "/api/new", json={"path": "   "}).status_code == 400


class TestBatchEpisodes:
    """量产就是一次跑一串剧集，一集一集手点没有意义。"""

    @pytest.fixture
    def multi(self, tmp_path):
        from changji.models.project import Episode
        from changji.models.shot import Shot, ShotSize

        store = ProjectStore.create(tmp_path / "剧", "drama")
        p = store.load_project()
        for i in (1, 2, 3):
            p.episodes.append(Episode(
                episode_id=f"ep{i:02d}",
                shots=[Shot(shot_id=f"ep{i:02d}_a", scene_id="s", order=0,
                            duration_s=3.0, shot_size=ShotSize.MS)]))
        # 第四集只有壳没有分镜，不该进队列
        p.episodes.append(Episode(episode_id="ep04"))
        store.save_project(p)
        return store

    @pytest.fixture
    def c(self):
        """指向一个连不上的 ComfyUI。

        这些用例只看排队算得对不对，不能真去提交任务，
        否则测试会往开发机上的 ComfyUI 里塞活儿，还要等它超时。
        """
        from changji.config import ComfyConfig, Settings
        from changji.web.server import create_app

        settings = Settings(comfy=ComfyConfig(
            base_url="http://127.0.0.1:9", timeout_s=0.2, max_retries=0))
        return TestClient(create_app(settings))

    def test_排完整个项目(self, multi, c):
        r = c.post("/api/run", json={
            "project": str(multi.root), "episode_id": "ep01",
            "all_episodes": True})
        assert r.status_code == 200
        assert r.json()["queue"] == ["ep01", "ep02", "ep03"], "没分镜的那集不排"
        c.post("/api/stop", json={})

    def test_不勾就只跑指定那一集(self, multi, c):
        r = c.post("/api/run", json={
            "project": str(multi.root), "episode_id": "ep02"})
        assert r.json()["queue"] == ["ep02"]
        c.post("/api/stop", json={})

    def test_一集分镜都没有时说清楚(self, tmp_path, c):
        store = ProjectStore.create(tmp_path / "空", "k")
        r = c.post("/api/run", json={
            "project": str(store.root), "episode_id": "ep01",
            "all_episodes": True})
        assert r.status_code == 400
        assert "分镜表" in r.json()["detail"]

    def test_队列进度出现在状态里(self, multi, c):
        c.post("/api/run", json={
            "project": str(multi.root), "episode_id": "ep01",
            "all_episodes": True})
        s = c.get("/api/run").json()
        assert s["queue_total"] == 3
        assert s["outputs"] == []
        c.post("/api/stop", json={})

    def test_正在跑的时候不许再起一个(self, multi):
        """两条流水线同时写一个 project.json，写进去的就是一团乱。"""
        from changji.config import Settings
        from changji.web.server import create_app

        app = create_app(Settings())
        app.state.run.running = True
        app.state.run.episode_id = "ep01"
        r = TestClient(app).post("/api/run", json={
            "project": str(multi.root), "episode_id": "ep02"})
        assert r.status_code == 409
        assert "ep01" in r.json()["detail"]


class TestDeleteProject:
    """删项目是整个目录连素材带成片一起没，闸门要够多。"""

    def _client(self, tmp_path):
        from changji.config import Settings
        from changji.web.server import create_app
        return TestClient(create_app(Settings(workspace=str(tmp_path))))

    def test_名字对得上才删(self, tmp_path):
        store = ProjectStore.create(tmp_path / "要删的", "d")
        c = self._client(tmp_path)
        r = c.post("/api/project/delete", json={
            "path": str(store.root), "confirm_name": "要删的"})
        assert r.status_code == 200
        assert not store.root.exists()

    def test_名字打错就不删(self, tmp_path):
        store = ProjectStore.create(tmp_path / "要删的", "d")
        r = self._client(tmp_path).post("/api/project/delete", json={
            "path": str(store.root), "confirm_name": "要删得"})
        assert r.status_code == 400
        assert store.root.exists(), "名字对不上一个字节都不能动"

    def test_项目库外面的不给删(self, tmp_path):
        outside = tmp_path.parent / "库外的项目"
        store = ProjectStore.create(outside, "o")
        try:
            r = self._client(tmp_path).post("/api/project/delete", json={
                "path": str(store.root), "confirm_name": "库外的项目"})
            assert r.status_code == 403
            assert store.root.exists()
        finally:
            import shutil
            shutil.rmtree(outside, ignore_errors=True)

    def test_项目库本身不给删(self, tmp_path):
        r = self._client(tmp_path).post("/api/project/delete", json={
            "path": str(tmp_path), "confirm_name": tmp_path.name})
        assert r.status_code == 403
        assert tmp_path.exists()

    def test_不是项目的目录不给删(self, tmp_path):
        d = tmp_path / "随手建的"
        d.mkdir()
        r = self._client(tmp_path).post("/api/project/delete", json={
            "path": str(d), "confirm_name": "随手建的"})
        assert r.status_code == 404
        assert d.exists()

    def test_正在跑的时候不给删(self, tmp_path):
        from changji.config import Settings
        from changji.web.server import create_app

        store = ProjectStore.create(tmp_path / "在跑的", "r")
        app = create_app(Settings(workspace=str(tmp_path)))
        app.state.run.running = True
        r = TestClient(app).post("/api/project/delete", json={
            "path": str(store.root), "confirm_name": "在跑的"})
        assert r.status_code == 409
        assert store.root.exists()

    def test_多余字段不认(self, tmp_path):
        store = ProjectStore.create(tmp_path / "x", "x")
        r = self._client(tmp_path).post("/api/project/delete", json={
            "path": str(store.root), "confirm_name": "x", "force": True})
        assert r.status_code == 422
        assert store.root.exists()


class TestVoiceGenderVisible:
    """音色留空时按猜出来的性别挑，猜错了用户得看得见。"""

    def test_资产接口带上性别(self, client, project):
        from changji.models.character import (
            AppearanceBlock, AssetLibrary, Character,
        )
        project.save_assets(AssetLibrary(characters={"c_a": Character(
            char_id="c_a", name="甲", voice_gender="male",
            appearance=AppearanceBlock(identity="男性", face="x", attire="y"))}))
        d = client.get("/api/assets", params={"path": str(project.root)}).json()
        assert d["characters"][0]["voice_gender"] == "male"


class TestRunPreview:
    """按下开始就是几十分钟，得先说清楚这一次会做什么。"""

    @pytest.fixture
    def staged(self, tmp_path):
        from changji.models.project import Episode
        from changji.models.shot import Shot, ShotSize, ShotStatus

        store = ProjectStore.create(tmp_path / "预演", "yy")
        p = store.load_project()
        p.episodes.append(Episode(episode_id="ep01", shots=[
            Shot(shot_id="a", scene_id="s", order=0, duration_s=3.0,
                 shot_size=ShotSize.MS, status=ShotStatus.PLANNED),
            Shot(shot_id="b", scene_id="s", order=1, duration_s=3.0,
                 shot_size=ShotSize.MS, status=ShotStatus.DRAFT_DONE),
            Shot(shot_id="c", scene_id="s", order=2, duration_s=3.0,
                 shot_size=ShotSize.MS, status=ShotStatus.FINAL_DONE),
            Shot(shot_id="d", scene_id="s", order=3, duration_s=3.0,
                 shot_size=ShotSize.MS, status=ShotStatus.LOCKED),
        ]))
        store.save_project(p)
        return store

    def _get(self, client, store, **kw):
        params = {"path": str(store.root), "episode_id": "ep01"}
        params.update(kw)
        return client.get("/api/run/preview", params=params).json()

    def test_算上后面的每一步(self, client, staged):
        """一个镜头从它现在的状态开始，会一路走完后面所有阶段。

        只按当前状态归到一个阶段的话，会告诉人「配音 1 镜，粗估 8 秒」，
        而那一镜还要出首帧、跑草稿档、跑成片档，得等十几分钟。
        报小了的预演比没有预演更糟。
        """
        d = self._get(client, staged)
        by = {x["stage"]: x["shots"] for x in d["stages"]}
        assert by.get("audio") == 1, "只有未开工的那一镜要配音"
        assert by.get("frames") == 1, "那一镜配完还要出首帧"
        assert by.get("draft") == 1, "还要跑草稿档"
        assert by.get("final") == 2, "它自己加上已经过了草稿档的那一镜"
        assert d["shots"] == 4

    def test_锁定的镜头不算(self, client, staged):
        d = self._get(client, staged)
        assert all(x["shots"] <= 3 for x in d["stages"]), "锁定的那一镜不该被算进去"

    def test_已完成的镜头不算(self, client, staged):
        """final_done 的那一镜这一次一步都不用走。"""
        d = self._get(client, staged)
        by = {x["stage"]: x["shots"] for x in d["stages"]}
        assert by["audio"] == 1, "四镜里只有一镜要从头走"

    def test_只跑草稿档时不算成片档(self, client, staged):
        d = self._get(client, staged, skip_final="true")
        assert "final" not in {x["stage"] for x in d["stages"]}

    def test_全部重做时每一镜都算(self, client, staged):
        d = self._get(client, staged, force="true")
        by = {x["stage"]: x["shots"] for x in d["stages"]}
        assert by["audio"] == 4

    def test_没得做时说清楚(self, client, tmp_path):
        from changji.models.project import Episode
        from changji.models.shot import Shot, ShotSize, ShotStatus

        store = ProjectStore.create(tmp_path / "全好了", "k")
        p = store.load_project()
        p.episodes.append(Episode(episode_id="ep01", shots=[
            Shot(shot_id="a", scene_id="s", order=0, duration_s=3.0,
                 shot_size=ShotSize.MS, status=ShotStatus.FINAL_DONE)]))
        store.save_project(p)
        d = self._get(client, store)
        assert d["idle"] is True
        assert d["stages"] == []

    def test_给出时间估计(self, client, staged):
        d = self._get(client, staged)
        assert d["estimate_s"] > 0
        assert d["estimate_text"], "光给秒数没人愿意心算"

    def test_没分镜时报错而不是给个空壳(self, client, tmp_path):
        store = ProjectStore.create(tmp_path / "空", "k")
        r = client.get("/api/run/preview", params={
            "path": str(store.root), "episode_id": "ep01"})
        assert r.status_code == 400
