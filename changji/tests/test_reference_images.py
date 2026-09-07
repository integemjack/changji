# -*- coding: utf-8 -*-
"""角色参考图。

一致性最硬的手段。文字描述再细，模型每次也会重新想象一遍这张脸；
给一张图，它就照着画。

整条链路早就通了：Character 上有正面、四分之三侧、背面三个位置，
出首帧时按机位挑用哪一张，上传给 ComfyUI 接到 LoadImage 上。
缺的只是界面上没地方设置——功能存在但够不着。
"""
import io
import struct
import zlib

import pytest
from fastapi.testclient import TestClient

from changji.config import Settings
from changji.models.character import (
    AppearanceBlock, AssetLibrary, Character,
)
from changji.models.project import Episode, ProjectStore
from changji.models.shot import Shot, ShotSize, ShotStatus
from changji.web.server import create_app


def _png(width: int = 8, height: int = 8) -> bytes:
    """造一张最小的合法 png，不引第三方库。"""
    raw = b"".join(b"\x00" + b"\xff\x00\x00" * width for _ in range(height))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


@pytest.fixture
def project(tmp_path):
    store = ProjectStore.create(tmp_path / "剧", "d", "剧")
    store.save_assets(AssetLibrary(characters={"c_lin": Character(
        char_id="c_lin", name="林晚",
        appearance=AppearanceBlock(identity="女性", face="长发", attire="西装"))}))
    p = store.load_project()
    p.episodes.append(Episode(episode_id="ep01", shots=[
        Shot(shot_id="a", scene_id="s", order=0, duration_s=3.0,
             shot_size=ShotSize.MS, status=ShotStatus.FINAL_DONE),
        Shot(shot_id="b", scene_id="s", order=1, duration_s=3.0,
             shot_size=ShotSize.MS, status=ShotStatus.LOCKED),
    ]))
    store.save_project(p)
    return store


@pytest.fixture
def client():
    return TestClient(create_app(Settings()))


def _upload(client, store, slot="front", name="a.png",
            content=None, ctype="image/png"):
    return client.post("/api/character/reference", data={
        "project": str(store.root), "char_id": "c_lin", "slot": slot,
    }, files={"file": (name, io.BytesIO(_png() if content is None else content), ctype)})


class TestUpload:
    def test_传上去就落到项目里(self, client, project):
        r = _upload(client, project)
        assert r.status_code == 200
        rel = r.json()["saved"]
        assert (project.root / rel).is_file()
        assert project.load_assets().characters["c_lin"].ref_front == rel

    def test_三个位置各存各的(self, client, project):
        for slot in ("front", "three_quarter", "back"):
            assert _upload(client, project, slot=slot).status_code == 200
        c = project.load_assets().characters["c_lin"]
        assert c.ref_front and c.ref_three_quarter and c.ref_back
        assert len({c.ref_front, c.ref_three_quarter, c.ref_back}) == 3

    def test_传完要把镜头退回重跑(self, client, project):
        """参考图直接决定画面长什么样，跟改外观是一回事。"""
        d = _upload(client, project).json()
        assert d["reset_shots"] == 1
        ep = project.load_project().episode_by_id("ep01")
        assert ep.shot_by_id("a").status.value == "planned"
        assert ep.shot_by_id("b").status.value == "locked", "锁定的不能动"

    def test_换格式重传会删掉旧的那张(self, client, project):
        _upload(client, project, name="a.png", ctype="image/png")
        old = project.paths.refs / "c_lin_front.png"
        assert old.is_file()
        _upload(client, project, name="a.jpg", ctype="image/jpeg")
        assert not old.is_file(), "留着的话 refs 里会有一张永远用不上的"
        assert (project.paths.refs / "c_lin_front.jpg").is_file()

    def test_位置名乱填要挡住(self, client, project):
        assert _upload(client, project, slot="侧面").status_code == 400

    def test_没这个角色要报四零四(self, client, project):
        r = client.post("/api/character/reference", data={
            "project": str(project.root), "char_id": "c_nobody",
            "slot": "front",
        }, files={"file": ("a.png", io.BytesIO(_png()), "image/png")})
        assert r.status_code == 404

    def test_不收别的格式(self, client, project):
        r = _upload(client, project, name="a.txt", content=b"hello",
                    ctype="text/plain")
        assert r.status_code == 400
        assert "png" in r.json()["detail"]

    def test_空文件不收(self, client, project):
        r = _upload(client, project, content=b"", ctype="image/png")
        assert r.status_code == 400

    def test_太大的不收(self, client, project):
        """参考图给模型看，几千像素够了，别把项目目录撑爆。"""
        r = _upload(client, project, content=b"\x89PNG" + b"\x00" * (21 << 20))
        assert r.status_code == 400
        assert "太大" in r.json()["detail"]


class TestClear:
    def test_撤掉之后退回纯文字(self, client, project):
        _upload(client, project)
        d = client.post("/api/character/reference/clear", json={
            "project": str(project.root), "char_id": "c_lin",
            "slot": "front"}).json()
        assert d["cleared"] is True
        assert project.load_assets().characters["c_lin"].ref_front is None

    def test_撤掉不删文件(self, client, project):
        """用户可能只是想试试没有参考图的效果，删了再想用回来还得重新找。"""
        rel = _upload(client, project).json()["saved"]
        client.post("/api/character/reference/clear", json={
            "project": str(project.root), "char_id": "c_lin", "slot": "front"})
        assert (project.root / rel).is_file()

    def test_本来就没有时不算改动(self, client, project):
        d = client.post("/api/character/reference/clear", json={
            "project": str(project.root), "char_id": "c_lin",
            "slot": "back"}).json()
        assert d["cleared"] is False
        assert d["reset_shots"] == 0

    def test_多余字段不认(self, client, project):
        r = client.post("/api/character/reference/clear", json={
            "project": str(project.root), "char_id": "c_lin",
            "slot": "front", "delete_file": True})
        assert r.status_code == 422


class TestExposed:
    def test_资产接口带出三张图(self, client, project):
        _upload(client, project, slot="front")
        d = client.get("/api/assets",
                       params={"path": str(project.root)}).json()
        c = d["characters"][0]
        assert c["ref_front"]
        assert c["ref_three_quarter"] is None
        assert c["ref_back"] is None

    def test_能通过媒体接口取回来(self, client, project):
        rel = _upload(client, project).json()["saved"]
        r = client.get("/api/media", params={
            "path": str(project.root), "rel": rel})
        assert r.status_code == 200
        assert r.content[:8] == b"\x89PNG\r\n\x1a\n"


class TestComposerUsesThem:
    """存下来的图要真的进到出首帧那一步。"""

    def test_按机位挑用哪一张(self, client, project):
        from changji.models.shot import CharacterInShot
        from changji.stages.render import PromptComposer

        for slot in ("front", "three_quarter"):
            _upload(client, project, slot=slot)
        assets = project.load_assets()
        shot = Shot(shot_id="x", scene_id="s", order=0, duration_s=3.0,
                    shot_size=ShotSize.MCU, first_frame_prompt="她站着",
                    characters=[CharacterInShot(char_id="c_lin",
                                                face_pose="front")])
        bundle = PromptComposer(assets).compose(shot)
        assert any("front" in r for r in bundle.reference_images)

    def test_没传图时不带参考图(self, project):
        from changji.models.shot import CharacterInShot
        from changji.stages.render import PromptComposer

        shot = Shot(shot_id="x", scene_id="s", order=0, duration_s=3.0,
                    shot_size=ShotSize.MCU, first_frame_prompt="她站着",
                    characters=[CharacterInShot(char_id="c_lin")])
        bundle = PromptComposer(project.load_assets()).compose(shot)
        assert bundle.reference_images == []
        assert bundle.positive, "没有图也得有文字提示词"


class TestTellsWhenIgnored:
    """参考图只有图像工作流那条路会用。

    默认装机没有 image.json，首帧走视频模型，那条路直接忽略参考图。
    传了图、镜头也退回重跑了，画面却一点没变，用户只会以为是模型
    不听话。这种「按了没反应」的控件比没有还糟，必须当场说清楚。
    """

    def test_没有图像工作流时资产接口说清楚(self, client, project):
        d = client.get("/api/assets",
                       params={"path": str(project.root)}).json()
        assert d["reference_images_used"] is False
        assert "image.json" in d["reference_hint"]

    def test_有图像工作流就不啰嗦(self, client, project):
        wf = project.root / "workflows"
        wf.mkdir(parents=True, exist_ok=True)
        (wf / "image.json").write_text("{}", encoding="utf-8")
        d = client.get("/api/assets",
                       params={"path": str(project.root)}).json()
        assert d["reference_images_used"] is True
        assert d["reference_hint"] == ""

    @pytest.mark.asyncio
    async def test_跑首帧时也要提醒(self, project, monkeypatch):
        """不开角色场景页的人同样该看到这句话。"""
        from changji.config import Settings
        from changji.pipeline import Pipeline, Stage
        from changji.stages import frames as frames_mod

        _upload_direct(project, "front")

        events = []
        pipe = Pipeline(project, Settings(), None, None,
                        listener=events.append)

        class FakeBackend:
            name = "video_model"

            async def generate(self, *a, **kw):
                raise AssertionError("不该真去跑")

        monkeypatch.setattr(frames_mod, "build_backend",
                            lambda *a, **kw: FakeBackend())
        monkeypatch.setattr(
            "changji.pipeline.build_frame_backend",
            lambda *a, **kw: FakeBackend())

        p = project.load_project()
        ep = p.episode_by_id("ep01")
        for shot in ep.shots:
            shot.status = ShotStatus.AUDIO_DONE
        assets = project.load_assets()
        try:
            await pipe.run_frames(p, ep, assets)
        except Exception:
            pass  # 后端是假的，跑到一半炸掉无所谓，只看有没有提醒

        warns = [e.message for e in events
                 if e.stage is Stage.FRAMES and e.kind == "warn"]
        assert any("不看参考图" in m for m in warns), warns


def _upload_direct(store, slot):
    """不走接口直接写进资产库，测试里省一次 http。"""
    store.paths.refs.mkdir(parents=True, exist_ok=True)
    dest = store.paths.refs / f"c_lin_{slot}.png"
    dest.write_bytes(_png())
    assets = store.load_assets()
    setattr(assets.characters["c_lin"], f"ref_{slot}", store.paths.rel(dest))
    store.save_assets(assets)
