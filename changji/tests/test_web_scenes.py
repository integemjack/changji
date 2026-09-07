# -*- coding: utf-8 -*-
"""场景这一步：资产库的合并、空景图、以及镜头和场景的连接。

界面把八步分成了「全剧」和「分集」两段，场景是分集那段的第一步。
这带来三件必须成立的事：

一，出第五集的场景不能把前四集的场景冲掉。资产库是全剧共用的一份，
整个替换会让老分镜指向不存在的 id，跑起来直接断在首帧那一步。

二，场景要有空景图。这是场景一致性最硬的手段，和角色三视图同理。
模型上早就有 ref_empty 这个字段，缺的是界面够不着。

三，location_id 空着的镜头要能接回场景。schema 里 scene_id 和
location_id 是两个字段，模型十次有八次只填前者——而渲染时只认后者，
于是空间和光线那一段整个丢掉，不报错，只是画面里的房间每镜都不一样。
这类静默失败最难查，所以既要在生成时接上，也要能修老数据。
"""
import struct
import zlib

import pytest
from fastapi.testclient import TestClient

from changji.config import Settings
from changji.models.character import (
    AppearanceBlock, AssetLibrary, Character, Location, StyleProfile,
)
from changji.models.project import Episode, ProjectStore
from changji.models.shot import CharacterInShot, Shot, ShotStatus
from changji.stages.storyboard import link_location
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


def _character(char_id: str, name: str) -> Character:
    return Character(
        char_id=char_id, name=name,
        appearance=AppearanceBlock(identity="女性", face="长发", attire="西装"))


def _location(loc_id: str, name: str) -> Location:
    return Location(location_id=loc_id, name=name, space="房间", lighting="冷光")


@pytest.fixture
def project(tmp_path):
    """两集：ep01 的镜头只填了 scene_id，ep02 老老实实填了 location_id。"""
    store = ProjectStore.create(tmp_path / "剧", "drama", "测试剧")
    store.save_assets(AssetLibrary(
        characters={"c_lin": _character("c_lin", "林晚")},
        locations={"loc_office": _location("loc_office", "办公室")},
        style=StyleProfile(global_style="冷调"),
    ))
    p = store.load_project()
    p.premise = "一句梗概"
    p.episodes.append(Episode(
        episode_id="ep01", script="林晚：我走了。", target_duration_s=10.0,
        shots=[
            # location_id 留空，场景 id 填在了 scene_id 上——模型最常见的填法
            Shot(shot_id="ep01_sh001", scene_id="loc_office", order=0,
                 duration_s=3.0, status=ShotStatus.DRAFT_DONE,
                 characters=[CharacterInShot(char_id="c_lin")]),
            Shot(shot_id="ep01_sh002", scene_id="loc_office", order=1,
                 duration_s=2.0, status=ShotStatus.PLANNED),
        ]))
    p.episodes.append(Episode(
        episode_id="ep02", script="林晚：又见面了。", target_duration_s=10.0,
        shots=[
            Shot(shot_id="ep02_sh001", scene_id="s1", order=0, duration_s=3.0,
                 location_id="loc_office", status=ShotStatus.PLANNED),
        ]))
    store.save_project(p)
    return store


@pytest.fixture
def client(project):
    return TestClient(create_app(Settings()))


class TestLinkLocation:
    """生成时就把 scene_id 接到 location_id 上。"""

    def test_空着就按_scene_id_接上(self):
        item = {"scene_id": "loc_office"}
        assert link_location(item, {"loc_office"}) is True
        assert item["location_id"] == "loc_office"

    def test_已经填了就不动(self):
        item = {"scene_id": "loc_office", "location_id": "loc_bar"}
        assert link_location(item, {"loc_office", "loc_bar"}) is False
        assert item["location_id"] == "loc_bar"

    def test_scene_id_不是场景就别乱接(self):
        # 分场号这类写法接上去就是编了一个不存在的引用
        item = {"scene_id": "s1"}
        assert link_location(item, {"loc_office"}) is False
        assert item.get("location_id") is None


class TestLinkLocationsEndpoint:
    """修老数据。新分镜生成时已经接上了，这个接口给之前存下来的补。"""

    def test_按集补(self, client, project):
        r = client.post("/api/shots/link_locations", json={
            "project": str(project.root), "episode_id": "ep01"})
        assert r.status_code == 200
        assert r.json()["linked"] == 2

        ep = project.load_project().episode_by_id("ep01")
        assert all(s.location_id == "loc_office" for s in ep.shots)

    def test_补完要把已渲染的退回重跑(self, client, project):
        """接上之后提示词才完整，之前那些是按缺场景的提示词跑出来的。"""
        r = client.post("/api/shots/link_locations", json={
            "project": str(project.root), "episode_id": "ep01"})
        assert r.json()["reset_shots"] >= 1

        ep = project.load_project().episode_by_id("ep01")
        assert ep.shot_by_id("ep01_sh001").status is ShotStatus.PLANNED

    def test_不填集号就整个项目补(self, client, project):
        r = client.post("/api/shots/link_locations",
                        json={"project": str(project.root)})
        assert r.json()["episodes"] == {"ep01": 2}

    def test_没得补时不写盘也不重跑(self, client, project):
        client.post("/api/shots/link_locations", json={"project": str(project.root)})
        r = client.post("/api/shots/link_locations", json={"project": str(project.root)})
        assert r.json() == {"linked": 0, "episodes": {}, "reset_shots": 0}


class TestBibleMerge:
    """出新一集的角色场景时，老的必须留着。"""

    @pytest.fixture(autouse=True)
    def _stub_llm(self, monkeypatch):
        """大模型返回一个新角色加一个新场景，外加一个同名的老场景。"""
        async def fake(self, script, style_line=None, aspect_ratio="9:16"):
            return AssetLibrary(
                characters={"c_wang": _character("c_wang", "老王")},
                locations={
                    "loc_bar": _location("loc_bar", "酒吧"),
                    # 同名场景，名字换了——不勾覆盖的话不该顶掉旧的
                    "loc_office": _location("loc_office", "崭新的办公室"),
                },
            )
        monkeypatch.setattr("changji.stages.bible.BibleGenerator.generate", fake)

    def test_只补新的_老的原样留着(self, client, project):
        r = client.post("/api/bible", json={
            "project": str(project.root), "episode_id": "ep02"})
        assert r.status_code == 200
        body = r.json()
        assert body["added_characters"] == ["c_wang"]
        assert body["added_locations"] == ["loc_bar"]

        assets = project.load_assets()
        assert set(assets.characters) == {"c_lin", "c_wang"}
        # 同名的保留旧的：手改过的设定和传过的空景图都挂在旧的那一份上
        assert assets.locations["loc_office"].name == "办公室"

    def test_不覆盖就不重跑已渲染的镜头(self, client, project):
        r = client.post("/api/bible", json={
            "project": str(project.root), "episode_id": "ep02"})
        assert r.json()["reset_shots"] == 0

        ep = project.load_project().episode_by_id("ep01")
        assert ep.shot_by_id("ep01_sh001").status is ShotStatus.DRAFT_DONE

    def test_勾了覆盖才顶掉_并且要重跑(self, client, project):
        r = client.post("/api/bible", json={
            "project": str(project.root), "episode_id": "ep02",
            "overwrite": True})
        assert r.json()["reset_shots"] >= 1
        assert project.load_assets().locations["loc_office"].name == "崭新的办公室"

    def test_风格层不被顺手覆盖(self, client, project):
        """风格是用户在项目页调的，不该被每次重出角色带走。"""
        client.post("/api/bible", json={"project": str(project.root)})
        assert project.load_assets().style.global_style == "冷调"

    def test_没有剧本就说清楚(self, client, tmp_path):
        empty = ProjectStore.create(tmp_path / "空", "empty", "空剧")
        r = client.post("/api/bible", json={"project": str(empty.root)})
        assert r.status_code == 400
        assert "剧本" in r.json()["detail"]


class TestLocationReference:
    """空景图。场景一致性的锚点，和角色三视图同理。"""

    def test_传一张然后能读回来(self, client, project):
        r = client.post(
            "/api/location/reference",
            data={"project": str(project.root), "location_id": "loc_office"},
            files={"file": ("empty.png", _png(), "image/png")})
        assert r.status_code == 200
        rel = r.json()["saved"]
        assert (project.root / rel).is_file()

        got = client.get("/api/assets", params={"path": str(project.root)})
        loc = got.json()["locations"][0]
        assert loc["ref_empty"] == rel

    def test_传图要把已渲染的退回重跑(self, client, project):
        """空景图直接决定画面长什么样，跟改场景描述是一回事。"""
        r = client.post(
            "/api/location/reference",
            data={"project": str(project.root), "location_id": "loc_office"},
            files={"file": ("empty.png", _png(), "image/png")})
        assert r.json()["reset_shots"] >= 1

    def test_不认的格式要拦下来(self, client, project):
        r = client.post(
            "/api/location/reference",
            data={"project": str(project.root), "location_id": "loc_office"},
            files={"file": ("a.txt", b"not an image", "text/plain")})
        assert r.status_code == 400

    def test_没有这个场景(self, client, project):
        r = client.post(
            "/api/location/reference",
            data={"project": str(project.root), "location_id": "loc_nope"},
            files={"file": ("empty.png", _png(), "image/png")})
        assert r.status_code == 404

    def test_撤掉之后退回纯文字(self, client, project):
        client.post(
            "/api/location/reference",
            data={"project": str(project.root), "location_id": "loc_office"},
            files={"file": ("empty.png", _png(), "image/png")})
        r = client.post("/api/location/reference/clear", json={
            "project": str(project.root), "location_id": "loc_office"})
        assert r.json()["cleared"] is True
        assert project.load_assets().locations["loc_office"].ref_empty is None

    def test_本来就没有就当没这回事(self, client, project):
        r = client.post("/api/location/reference/clear", json={
            "project": str(project.root), "location_id": "loc_office"})
        assert r.json() == {"cleared": False, "reset_shots": 0}


class TestPremise:
    """存一句梗概不该惊动大模型。"""

    def test_存下来并且读得回(self, client, project):
        r = client.post("/api/project/premise", json={
            "project": str(project.root), "premise": "  深夜便利店  "})
        assert r.status_code == 200
        assert r.json()["premise"] == "深夜便利店"
        assert project.load_project().premise == "深夜便利店"

    def test_不碰剧集和分镜(self, client, project):
        client.post("/api/project/premise", json={
            "project": str(project.root), "premise": "换一句"})
        p = project.load_project()
        assert len(p.episodes) == 2
        assert len(p.episode_by_id("ep01").shots) == 2

    def test_清空也行(self, client, project):
        client.post("/api/project/premise", json={
            "project": str(project.root), "premise": "先写一句"})
        r = client.post("/api/project/premise", json={
            "project": str(project.root), "premise": ""})
        assert r.json()["premise"] == ""


class TestShotsPayload:
    """界面靠这些字段把 id 翻回人话，缺一个就退化成一张看不懂的表。"""

    def test_带上场景和角色(self, client, project):
        r = client.get("/api/shots", params={
            "path": str(project.root), "episode_id": "ep02"})
        shot = r.json()["shots"][0]
        assert shot["location_id"] == "loc_office"
        assert "char_ids" in shot

    def test_角色列表跟着分镜走(self, client, project):
        r = client.get("/api/shots", params={
            "path": str(project.root), "episode_id": "ep01"})
        assert r.json()["shots"][0]["char_ids"] == ["c_lin"]
