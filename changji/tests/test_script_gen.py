# -*- coding: utf-8 -*-
"""写剧本。

这是流水线的第一步，以前是空的：用户得自己写好剧本粘进来。
但整套东西要解决的问题就是「用 AI 写剧本、编排、出片」。

两件事必须由代码保证，不能指望模型自觉：
一是输出格式。让模型直接写剧本，它一会儿用冒号一会儿用括号，
一会儿把旁白也写成对白，后面识别角色就开始出错。所以模型只出
结构化的场次表，「名字：台词」这个形式由我们渲染。
二是时长。不给字数预算的话，模型写出来的东西按语速算能拍十分钟。
"""
import pytest
from fastapi.testclient import TestClient

from changji.config import LLMConfig, Settings
from changji.models.character import StyleLine
from changji.models.project import ProjectStore
from changji.stages.script import (
    ScriptDraft, ScriptError, ScriptGenerator, budget_chars, build_prompt,
)
from changji.web import server as srv


class TestBudget:
    def test_时长越长字数越多(self):
        assert budget_chars(30) < budget_chars(60) < budget_chars(120)

    def test_一分钟大约两百字(self):
        n = budget_chars(60)
        assert 150 <= n <= 220, f"按中文语速算一分钟装不下 {n} 字"

    def test_再短也给个下限(self):
        assert budget_chars(1) >= 20


class TestRender:
    """形式由我们定，不看模型心情。"""

    def _draft(self, beats):
        return ScriptDraft(title="t", logline="l", beats=beats)

    def test_对白渲染成名字加冒号(self):
        d = self._draft([{"kind": "dialogue", "speaker": "林晚", "text": "我走了。"}])
        assert d.render() == "林晚：我走了。"

    def test_动作单独成行不带名字(self):
        d = self._draft([
            {"kind": "action", "speaker": "", "text": "雪落在门廊上。"},
            {"kind": "dialogue", "speaker": "苏禾", "text": "我回来了。"},
        ])
        assert d.render() == "雪落在门廊上。\n苏禾：我回来了。"

    def test_空行不进渲染结果(self):
        d = self._draft([
            {"kind": "action", "speaker": "", "text": "  "},
            {"kind": "dialogue", "speaker": "甲", "text": "在。"},
        ])
        assert d.render() == "甲：在。"

    def test_说话人按出场顺序去重(self):
        d = self._draft([
            {"kind": "dialogue", "speaker": "甲", "text": "一"},
            {"kind": "dialogue", "speaker": "乙", "text": "二"},
            {"kind": "dialogue", "speaker": "甲", "text": "三"},
        ])
        assert d.speakers == ["甲", "乙"]

    def test_对白字数不算动作(self):
        d = self._draft([
            {"kind": "action", "speaker": "", "text": "一段很长的环境描写文字"},
            {"kind": "dialogue", "speaker": "甲", "text": "四个字啊"},
        ])
        assert d.dialogue_chars == 4


class TestParse:
    def test_正常解析(self):
        d = ScriptGenerator._parse('''{"title":"雪夜","logline":"回家",
          "beats":[{"kind":"action","speaker":"","text":"雪夜。"},
                   {"kind":"dialogue","speaker":"苏禾","text":"我回来了。"}]}''')
        assert d.title == "雪夜"
        assert d.speakers == ["苏禾"]

    def test_有话没说是谁说的就当旁白(self):
        """丢掉的话这句台词就从成片里消失了，比配错声音还糟。"""
        d = ScriptGenerator._parse('''{"title":"t","logline":"l",
          "beats":[{"kind":"dialogue","speaker":"","text":"没人应答。"},
                   {"kind":"dialogue","speaker":"甲","text":"在吗。"}]}''')
        assert d.render() == "没人应答。\n甲：在吗。"
        assert d.speakers == ["甲"]

    def test_一句台词都没有要报错(self):
        with pytest.raises(ScriptError, match="默片"):
            ScriptGenerator._parse('''{"title":"t","logline":"l",
              "beats":[{"kind":"action","speaker":"","text":"风吹过。"}]}''')

    def test_什么都没写要报错(self):
        with pytest.raises(ScriptError, match="没写出任何内容"):
            ScriptGenerator._parse('{"title":"t","logline":"l","beats":[]}')

    def test_内容全是空白要报错(self):
        with pytest.raises(ScriptError, match="全是空的"):
            ScriptGenerator._parse('''{"title":"t","logline":"l",
              "beats":[{"kind":"action","speaker":"","text":"   "}]}''')


class TestPrompt:
    def test_把字数预算写进去(self):
        p = build_prompt("随便", 60, StyleLine.REALISTIC)
        assert str(budget_chars(60)) in p

    def test_沿用角色时点名(self):
        p = build_prompt("x", 60, StyleLine.REALISTIC,
                         characters=["林晚", "陈默"])
        assert "林晚、陈默" in p
        assert "一字不改" in p

    def test_接着写时带上前文(self):
        p = build_prompt("x", 60, StyleLine.REALISTIC, previous="第一集的内容")
        assert "第一集的内容" in p

    def test_前文太长要截断(self):
        p = build_prompt("x", 60, StyleLine.REALISTIC, previous="啊" * 9000)
        assert len(p) < 6000, "整本剧塞进上下文会把小模型撑爆"

    def test_两条风格线提示不同(self):
        a = build_prompt("x", 60, StyleLine.REALISTIC)
        b = build_prompt("x", 60, StyleLine.ANIME)
        assert a != b

    @pytest.mark.asyncio
    async def test_梗概为空直接拒(self):
        with pytest.raises(ScriptError, match="要写什么"):
            await ScriptGenerator(LLMConfig()).generate("   ")


class TestWriteEndpoint:
    @pytest.fixture
    def project(self, tmp_path):
        from changji.models.character import (
            AppearanceBlock, AssetLibrary, Character,
        )
        from changji.models.project import Episode

        store = ProjectStore.create(tmp_path / "剧", "d", "剧")
        store.save_assets(AssetLibrary(characters={"c_lin": Character(
            char_id="c_lin", name="林晚",
            appearance=AppearanceBlock(identity="女性", face="x", attire="y"))}))
        p = store.load_project()
        p.episodes.append(Episode(episode_id="ep01", script="林晚：第一集。"))
        p.episodes.append(Episode(episode_id="ep02"))
        store.save_project(p)
        return store

    @pytest.fixture
    def fake_llm(self, monkeypatch):
        seen = {}

        async def fake_generate(self, premise, duration_s=60.0,
                                style_line=StyleLine.REALISTIC,
                                previous="", characters=None):
            seen["premise"] = premise
            seen["previous"] = previous
            seen["characters"] = characters
            seen["duration_s"] = duration_s
            return ScriptDraft(title="标题", logline="一句话", beats=[
                {"kind": "action", "speaker": "", "text": "夜。"},
                {"kind": "dialogue", "speaker": "林晚", "text": "我在。"},
            ])

        monkeypatch.setattr(ScriptGenerator, "generate", fake_generate)
        return seen

    @pytest.fixture
    def client(self):
        return TestClient(srv.create_app(Settings()))

    def test_写完回给界面不落库(self, client, project, fake_llm):
        d = client.post("/api/script/write", json={
            "project": str(project.root), "episode_id": "ep02",
            "premise": "写点什么"}).json()
        assert d["script"] == "夜。\n林晚：我在。"
        assert d["title"] == "标题"
        ep = project.load_project().episode_by_id("ep02")
        assert ep.script == "", "没点保存就不该写进项目"

    def test_接着前几集写(self, client, project, fake_llm):
        client.post("/api/script/write", json={
            "project": str(project.root), "episode_id": "ep02",
            "premise": "x"})
        assert "第一集。" in fake_llm["previous"]

    def test_只带这一集之前的(self, client, project, fake_llm):
        """把后面的也塞进去，模型会把还没发生的事当成已经发生的写。"""
        client.post("/api/script/write", json={
            "project": str(project.root), "episode_id": "ep01",
            "premise": "x"})
        assert fake_llm["previous"] == ""

    def test_不勾接着写就不带前文(self, client, project, fake_llm):
        client.post("/api/script/write", json={
            "project": str(project.root), "episode_id": "ep02",
            "premise": "x", "continue_from_previous": False})
        assert fake_llm["previous"] == ""

    def test_沿用已有角色(self, client, project, fake_llm):
        client.post("/api/script/write", json={
            "project": str(project.root), "episode_id": "ep02",
            "premise": "x"})
        assert fake_llm["characters"] == ["林晚"]

    def test_不勾沿用就不带角色(self, client, project, fake_llm):
        client.post("/api/script/write", json={
            "project": str(project.root), "episode_id": "ep02",
            "premise": "x", "reuse_characters": False})
        assert fake_llm["characters"] is None

    def test_报告长短是否合适(self, client, project, fake_llm):
        d = client.post("/api/script/write", json={
            "project": str(project.root), "episode_id": "ep02",
            "premise": "x", "duration_s": 600}).json()
        assert d["fit"] == "偏短", "六百秒只写三个字，得说出来"

    def test_多余字段不认(self, client, project):
        r = client.post("/api/script/write", json={
            "project": str(project.root), "premise": "x", "model": "gpt"})
        assert r.status_code == 422

    def test_时长越界要挡住(self, client, project):
        assert client.post("/api/script/write", json={
            "project": str(project.root), "premise": "x",
            "duration_s": 0}).status_code == 422


class TestStripWrapper:
    """模型爱给动作行加括号、给台词加引号，提示词里怎么说都拦不住。

    这些符号会一路流到分镜提示词和字幕里。形式是我们定的。
    """

    def test_削掉动作行的括号(self):
        from changji.stages.script import strip_wrapper
        assert strip_wrapper("（夜深了。）") == "夜深了。"
        assert strip_wrapper("(He waits.)") == "He waits."

    def test_削掉台词的引号(self):
        from changji.stages.script import strip_wrapper
        assert strip_wrapper("“我回来了。”") == "我回来了。"
        assert strip_wrapper("「在吗」") == "在吗"

    def test_不动中间的括号(self):
        from changji.stages.script import strip_wrapper
        assert strip_wrapper("（他笑了（很轻）然后走了）") == "他笑了（很轻）然后走了"

    def test_首尾不成对时不动(self):
        """光数左右个数会数错。这句左右各两个，但首尾那两个不是一对。"""
        from changji.stages.script import strip_wrapper
        s = "（甲说）乙答（丙笑）"
        assert strip_wrapper(s) == s

    def test_没闭合的不动(self):
        from changji.stages.script import strip_wrapper
        assert strip_wrapper("（未闭合") == "（未闭合"
        assert strip_wrapper("闭合）") == "闭合）"

    def test_空括号不动(self):
        from changji.stages.script import strip_wrapper
        assert strip_wrapper("（）") == "（）"

    def test_普通文字原样(self):
        from changji.stages.script import strip_wrapper
        assert strip_wrapper("没什么。我只是想站一会儿。") == "没什么。我只是想站一会儿。"

    def test_解析时就削掉(self):
        d = ScriptGenerator._parse('''{"title":"t","logline":"l","beats":[
          {"kind":"action","speaker":"","text":"（夜深了。）"},
          {"kind":"dialogue","speaker":"林薇","text":"“我在。”"}]}''')
        assert d.render() == "夜深了。\n林薇：我在。"


class TestWriteSeries:
    """一口气写好几集。量产的前半段。"""

    @pytest.fixture
    def project(self, tmp_path):
        return ProjectStore.create(tmp_path / "季", "s", "季")

    @pytest.fixture
    def app_client(self):
        app = srv.create_app(Settings())
        return app, TestClient(app)

    @pytest.fixture
    def fake_llm(self, monkeypatch):
        calls = []

        async def fake_generate(self, premise, duration_s=60.0,
                                style_line=StyleLine.REALISTIC,
                                previous="", characters=None):
            calls.append({"previous": previous, "characters": characters})
            n = len(calls)
            return ScriptDraft(title=f"第{n}集", logline="梗概", beats=[
                {"kind": "action", "speaker": "", "text": f"场景{n}。"},
                {"kind": "dialogue", "speaker": "甲", "text": f"第{n}句。"},
            ])

        monkeypatch.setattr(ScriptGenerator, "generate", fake_generate)
        return calls

    def _wait(self, client):
        import time
        for _ in range(200):
            s = client.get("/api/script/series").json()
            if not s["running"]:
                return s
            time.sleep(0.05)
        raise AssertionError("写了太久没结束")

    def test_写出三集并落库(self, app_client, project, fake_llm):
        _, c = app_client
        c.post("/api/script/series", json={
            "project": str(project.root), "premise": "随便", "episodes": 3})
        s = self._wait(c)
        assert s["done"] == 3
        eps = project.load_project().episodes
        assert [e.episode_id for e in eps] == ["ep01", "ep02", "ep03"]
        assert eps[0].script == "场景1。\n甲：第1句。"
        assert eps[2].title == "第3集"

    def test_后面几集拿前面的当上下文(self, app_client, project, fake_llm):
        _, c = app_client
        c.post("/api/script/series", json={
            "project": str(project.root), "premise": "x", "episodes": 3})
        self._wait(c)
        assert fake_llm[0]["previous"] == "", "第一集没有前文"
        assert "第1句" in fake_llm[1]["previous"]
        assert "第2句" in fake_llm[2]["previous"]

    def test_上下文只带最近三集(self, app_client, project, fake_llm):
        """整季塞进去小模型撑不住。"""
        _, c = app_client
        c.post("/api/script/series", json={
            "project": str(project.root), "premise": "x", "episodes": 5})
        self._wait(c)
        last = fake_llm[-1]["previous"]
        assert "第1句" not in last, "太早的几集不该还留在上下文里"
        assert "第4句" in last

    def test_写砸一集接着往下写(self, app_client, project, monkeypatch):
        calls = []

        async def flaky(self, premise, duration_s=60.0,
                        style_line=StyleLine.REALISTIC,
                        previous="", characters=None):
            calls.append(1)
            if len(calls) == 2:
                raise ScriptError("这一集模型抽风了")
            return ScriptDraft(title="t", logline="l", beats=[
                {"kind": "dialogue", "speaker": "甲", "text": "在。"}])

        monkeypatch.setattr(ScriptGenerator, "generate", flaky)
        _, c = app_client
        c.post("/api/script/series", json={
            "project": str(project.root), "premise": "x", "episodes": 3})
        s = self._wait(c)
        assert s["done"] == 3
        bad = [e for e in s["episodes"] if e.get("error")]
        assert len(bad) == 1
        assert len(project.load_project().episodes) == 2, "写砸的那集不建"

    def test_正在写的时候不许再起一个(self, app_client, project):
        app, c = app_client
        app.state.write.running = True
        r = c.post("/api/script/series", json={
            "project": str(project.root), "premise": "x", "episodes": 2})
        assert r.status_code == 409

    def test_集数越界要挡住(self, app_client, project):
        _, c = app_client
        assert c.post("/api/script/series", json={
            "project": str(project.root), "premise": "x",
            "episodes": 99}).status_code == 422
        assert c.post("/api/script/series", json={
            "project": str(project.root), "premise": "x",
            "episodes": 0}).status_code == 422

    def test_编号跳过已有的(self, app_client, project, fake_llm):
        from changji.models.project import Episode

        p = project.load_project()
        p.episodes.append(Episode(episode_id="ep01", script="已有的"))
        project.save_project(p)
        _, c = app_client
        c.post("/api/script/series", json={
            "project": str(project.root), "premise": "x", "episodes": 2})
        self._wait(c)
        ids = [e.episode_id for e in project.load_project().episodes]
        assert ids == ["ep01", "ep02", "ep03"]
        assert project.load_project().episodes[0].script == "已有的", \
            "现有的几集一个字都不能动"


class TestPlanAll:
    """连着写完几集之后，分镜也得能一次补齐。

    否则每集还要单独点一次「重出分镜」，漏掉一次那集就跑不了，
    而且「跑每一集」会直接告诉你没有任何一集有分镜表。
    """

    @pytest.fixture
    def project(self, tmp_path):
        from changji.models.project import Episode
        from changji.models.shot import Shot, ShotSize

        store = ProjectStore.create(tmp_path / "季", "s", "季")
        p = store.load_project()
        p.episodes.append(Episode(episode_id="ep01", script="甲：一。",
                                  shots=[Shot(shot_id="a", scene_id="s",
                                              order=0, duration_s=3.0,
                                              shot_size=ShotSize.MS)]))
        p.episodes.append(Episode(episode_id="ep02", script="甲：二。"))
        p.episodes.append(Episode(episode_id="ep03", script="甲：三。"))
        p.episodes.append(Episode(episode_id="ep04"))  # 没剧本
        store.save_project(p)
        return store

    @pytest.fixture
    def fake_llm(self, monkeypatch):
        from changji.models.character import (
            AppearanceBlock, AssetLibrary, Character,
        )
        from changji.models.shot import Shot, ShotSize
        from changji.stages.bible import BibleGenerator
        from changji.stages.storyboard import StoryboardGenerator

        seen = []

        async def fake_bible(self, script, style_line=None, aspect_ratio="9:16"):
            return AssetLibrary(characters={"c_a": Character(
                char_id="c_a", name="甲",
                appearance=AppearanceBlock(identity="女性", face="x",
                                           attire="y"))})

        async def fake_board(self, script, assets, episode_id, duration_s):
            seen.append(episode_id)
            return [Shot(shot_id=f"{episode_id}_sh001", scene_id="s", order=0,
                         duration_s=3.0, shot_size=ShotSize.MS)]

        monkeypatch.setattr(BibleGenerator, "generate", fake_bible)
        monkeypatch.setattr(StoryboardGenerator, "generate", fake_board)
        return seen

    @pytest.fixture
    def client(self):
        return TestClient(srv.create_app(Settings()))

    def _wait(self, client):
        import time
        for _ in range(200):
            s = client.get("/api/script/series").json()
            if not s["running"]:
                return s
            time.sleep(0.05)
        raise AssertionError("跑了太久没结束")

    def test_只补没分镜的(self, client, project, fake_llm):
        r = client.post("/api/plan/all", json={"project": str(project.root)})
        assert r.json()["episodes"] == ["ep02", "ep03"], \
            "已有分镜的不动，没剧本的也轮不上"
        self._wait(client)
        assert fake_llm == ["ep02", "ep03"]

    def test_已有分镜的不被冲掉(self, client, project, fake_llm):
        client.post("/api/plan/all", json={"project": str(project.root)})
        self._wait(client)
        ep01 = project.load_project().episode_by_id("ep01")
        assert ep01.shots[0].shot_id == "a", "人工改过的分镜不能被覆盖"

    def test_勾了覆盖才连已有的一起重出(self, client, project, fake_llm):
        r = client.post("/api/plan/all", json={
            "project": str(project.root), "overwrite": True})
        assert r.json()["episodes"] == ["ep01", "ep02", "ep03"]

    def test_没得可出时说清楚(self, client, tmp_path, fake_llm):
        store = ProjectStore.create(tmp_path / "空", "k")
        r = client.post("/api/plan/all", json={"project": str(store.root)})
        assert r.status_code == 400
        assert "没有需要出分镜" in r.json()["detail"]

    def test_角色设定只出一次(self, client, project, fake_llm):
        """全剧共用一套角色。每集都重出的话同一个人前后长得不一样。"""
        client.post("/api/plan/all", json={"project": str(project.root)})
        self._wait(client)
        assert len(project.load_assets().characters) == 1

    def test_出完了报每集几个镜头(self, client, project, fake_llm):
        client.post("/api/plan/all", json={"project": str(project.root)})
        s = self._wait(client)
        assert s["done"] == 2
        assert all(e["shots"] == 1 for e in s["episodes"])

    def test_写剧本和出分镜不许同时跑(self, project):
        app = srv.create_app(Settings())
        app.state.write.running = True
        r = TestClient(app).post("/api/plan/all",
                                 json={"project": str(project.root)})
        assert r.status_code == 409


class TestNormalizeSpeaker:
    """模型说「没人说话」的写法五花八门。

    schema 里写的是填空字符串，实测它填了 none。原样当名字用的话，
    成片字幕上会出现「none：寂静。只有地铁的轰鸣声。」，
    出场角色列表里也会多一个叫 none 的人。
    """

    def test_各种空写法都归成空(self):
        from changji.stages.script import normalize_speaker
        for raw in ("none", "None", "NULL", "n/a", "-", "无",
                    "旁白", "画外音", "narrator", "(none)", "", None):
            assert normalize_speaker(raw) == "", raw

    def test_真名字不动(self):
        from changji.stages.script import normalize_speaker
        assert normalize_speaker("琳") == "琳"
        assert normalize_speaker("陈默") == "陈默"

    def test_解析时就归一(self):
        d = ScriptGenerator._parse('''{"title":"t","logline":"l","beats":[
          {"kind":"dialogue","speaker":"琳","text":"你看窗户。"},
          {"kind":"dialogue","speaker":"none","text":"寂静。"}]}''')
        assert d.speakers == ["琳"], "none 不是一个角色"
        assert "none" not in d.render()
        assert d.render().endswith("寂静。")


class TestPremiseRemembered:
    """梗概要存下来。

    隔天想接着写第六集，得凭记忆把当初那句话重打一遍，
    打得不一样，写出来的就跑偏了。
    """

    @pytest.fixture
    def project(self, tmp_path):
        return ProjectStore.create(tmp_path / "剧", "d", "剧")

    @pytest.fixture
    def fake_llm(self, monkeypatch):
        async def fake_generate(self, premise, duration_s=60.0,
                                style_line=StyleLine.REALISTIC,
                                previous="", characters=None):
            return ScriptDraft(title="t", logline="一句梗概", beats=[
                {"kind": "dialogue", "speaker": "甲", "text": "在。"}])

        monkeypatch.setattr(ScriptGenerator, "generate", fake_generate)

    @pytest.fixture
    def client(self):
        return TestClient(srv.create_app(Settings()))

    def test_写单集时存下梗概(self, client, project, fake_llm):
        client.post("/api/script/write", json={
            "project": str(project.root), "premise": "地铁上的倒影"})
        assert project.load_project().premise == "地铁上的倒影"

    def test_项目接口带出梗概(self, client, project, fake_llm):
        client.post("/api/script/write", json={
            "project": str(project.root), "premise": "地铁上的倒影"})
        d = client.get("/api/project",
                       params={"path": str(project.root)}).json()
        assert d["premise"] == "地铁上的倒影"

    def test_连着写也存(self, client, project, fake_llm):
        import time
        client.post("/api/script/series", json={
            "project": str(project.root), "premise": "整季的梗概",
            "episodes": 1})
        for _ in range(200):
            if not client.get("/api/script/series").json()["running"]:
                break
            time.sleep(0.05)
        assert project.load_project().premise == "整季的梗概"

    def test_每集存下自己的一句话(self, client, project, fake_llm):
        import time
        client.post("/api/script/series", json={
            "project": str(project.root), "premise": "x", "episodes": 1})
        for _ in range(200):
            if not client.get("/api/script/series").json()["running"]:
                break
            time.sleep(0.05)
        ep = project.load_project().episodes[0]
        assert ep.synopsis == "一句梗概"
        d = client.get("/api/project",
                       params={"path": str(project.root)}).json()
        assert d["episodes"][0]["synopsis"] == "一句梗概"


class TestLeadingTimecode:
    """模型爱在动作行开头挂一个时间码。

    提示词里没让它写，schema 里也没有这一项，它自己加的：
    「[0-3秒] 画面特写：老陈翻找」。这段文字原样进到分镜提示词里，
    等于让画面模型去理解一个时间码。
    """

    def test_削掉时间码(self):
        from changji.stages.script import strip_leading_timecode as f
        assert f("[0-3秒] 画面特写：老陈翻找。") == "画面特写：老陈翻找。"
        assert f("【00:03】他开口。") == "他开口。"
        assert f("(3s) 镜头推近") == "镜头推近"

    def test_正常的括号开头不动(self):
        """「（他犹豫了）他开口」是正常写法，削掉就丢内容了。"""
        from changji.stages.script import strip_leading_timecode as f
        assert f("（他犹豫了）他开口。") == "（他犹豫了）他开口。"
        assert f("[特写] 他开口。") == "[特写] 他开口。"
        assert f("[第二幕] 开始") == "[第二幕] 开始"

    def test_没有标记的原样(self):
        from changji.stages.script import strip_leading_timecode as f
        assert f("没有标记的一句话。") == "没有标记的一句话。"

    def test_解析时就削掉(self):
        d = ScriptGenerator._parse('''{"title":"t","logline":"l","beats":[
          {"kind":"action","speaker":"","text":"[0-3秒] 老陈翻找退书。"},
          {"kind":"dialogue","speaker":"老陈","text":"你又来了？"}]}''')
        assert d.render() == "老陈翻找退书。\n老陈：你又来了？"
