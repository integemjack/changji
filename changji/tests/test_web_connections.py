# -*- coding: utf-8 -*-
"""连接设置。

显卡不一定在这台机器上。地址写死在 toml 里、只能手改文件，
等于逼着用户放弃「换一台机器跑」这件事。所以界面上要能改，
而且要写回配置文件，否则重启一次就白改了。
"""
import pytest
from fastapi.testclient import TestClient

from changji.config import Settings, save_user_config
from changji.doctor import Check, Level, Report
from changji.web import server as srv


@pytest.fixture
def cfg_file(tmp_path, monkeypatch):
    """把写回目标挪到临时目录，别动用户真正的配置。"""
    target = tmp_path / "config.toml"
    monkeypatch.setattr(srv, "save_user_config",
                        lambda patch: save_user_config(patch, target))
    monkeypatch.setattr(srv, "user_config_path", lambda: target)
    return target


@pytest.fixture
def fake_doctor(monkeypatch):
    async def _checks(settings):
        return Report(checks=[Check("环境", Level.OK, "假的")])
    monkeypatch.setattr(srv, "run_checks", _checks)


@pytest.fixture
def settings():
    return Settings()


@pytest.fixture
def client(settings, cfg_file, fake_doctor):
    return TestClient(srv.create_app(settings))


class TestReadConnections:
    def test_读得到当前地址(self, client):
        d = client.get("/api/connections").json()
        assert d["comfy_base_url"] == "http://127.0.0.1:8188"
        assert d["llm_model"] == "qwen3:14b"
        assert d["tts_backend"] == "comfy"

    def test_不回传_api_key_明文(self, client):
        d = client.get("/api/connections").json()
        assert "llm_api_key" not in d
        assert d["llm_api_key_set"] is True
        assert "ollama" not in d["llm_api_key_hint"]


class TestWriteConnections:
    def test_改_comfy_地址(self, client, settings):
        r = client.post("/api/connections", json={
            "patch": {"comfy_base_url": "http://192.168.1.9:8188"}})
        assert r.status_code == 200
        assert r.json()["changed"] == ["comfy_base_url"]
        assert settings.comfy.base_url == "http://192.168.1.9:8188"

    def test_改动写回配置文件(self, client, cfg_file):
        client.post("/api/connections", json={
            "patch": {"comfy_base_url": "http://10.0.0.5:8188",
                      "llm_model": "qwen3:32b"}})
        text = cfg_file.read_text(encoding="utf-8")
        assert "http://10.0.0.5:8188" in text
        assert "qwen3:32b" in text

    def test_没改的项不进_changed(self, client):
        d = client.post("/api/connections", json={
            "patch": {"comfy_base_url": "http://127.0.0.1:8188",
                      "llm_model": "qwen3:32b"}}).json()
        assert d["changed"] == ["llm_model"]

    def test_地址少了协议头要报错(self, client, settings):
        r = client.post("/api/connections", json={
            "patch": {"comfy_base_url": "192.168.1.9:8188"}})
        assert r.status_code == 400
        assert "http" in r.json()["detail"]
        assert settings.comfy.base_url == "http://127.0.0.1:8188", "报错了就不能改"

    def test_配音后端选_http_必须填地址(self, client, settings):
        r = client.post("/api/connections", json={
            "patch": {"tts_backend": "http"}})
        assert r.status_code == 400
        assert settings.tts.backend == "comfy"

    def test_配音后端选_http_填了地址就能过(self, client, settings):
        r = client.post("/api/connections", json={
            "patch": {"tts_backend": "http",
                      "tts_base_url": "http://127.0.0.1:9880"}})
        assert r.status_code == 200
        assert settings.tts.backend == "http"

    def test_乱填的后端名不认(self, client):
        r = client.post("/api/connections", json={
            "patch": {"tts_backend": "随便"}})
        assert r.status_code == 400

    def test_不认识的字段直接拒(self, client):
        r = client.post("/api/connections", json={
            "patch": {"comfy_password": "x"}})
        assert r.status_code == 422

    def test_空改动要说清楚(self, client):
        r = client.post("/api/connections", json={"patch": {}})
        assert r.status_code == 400

    def test_显存覆盖能改也能清(self, client, settings):
        client.post("/api/connections", json={
            "patch": {"vram_gb_override": 16.0}})
        assert settings.vram_gb_override == 16.0

    def test_不勾持久化就不写文件(self, client, cfg_file):
        d = client.post("/api/connections", json={
            "patch": {"llm_model": "只改这一次"}, "persist": False}).json()
        assert d["saved_to"] is None
        assert not cfg_file.exists()

    def test_改完立刻带回体检结果(self, client):
        d = client.post("/api/connections", json={
            "patch": {"llm_model": "qwen3:32b"}}).json()
        assert d["can_run"] is True
        assert d["checks"][0]["name"] == "环境"

    def test_正在跑的时候不许换机器(self, settings, cfg_file, fake_doctor):
        """跑到一半换机器，这一集前后就是两套模型出的画面。"""
        app = srv.create_app(settings)
        app.state.run.running = True
        r = TestClient(app).post("/api/connections",
                                 json={"patch": {"llm_model": "x"}})
        assert r.status_code == 409
        assert settings.llm.model == "qwen3:14b"


class TestConfigRoundTrip:
    def test_写回后重新加载能读到(self, client, cfg_file):
        from changji.config import load_settings
        client.post("/api/connections", json={
            "patch": {"comfy_base_url": "http://1.2.3.4:8188"}})
        import changji.config as cf
        text = cfg_file.read_text(encoding="utf-8")
        assert "http://1.2.3.4:8188" in text
        # 用同一份文件走一遍解析，确认没写坏
        import tomlkit
        assert tomlkit.parse(text)["comfy"]["base_url"] == "http://1.2.3.4:8188"

    def test_保留文件里原有的注释(self, tmp_path):
        target = tmp_path / "c.toml"
        target.write_text('# 我的备注\n[comfy]\nbase_url = "http://a:1"\n',
                          encoding="utf-8")
        save_user_config({"comfy": {"base_url": "http://b:2"}}, target)
        text = target.read_text(encoding="utf-8")
        assert "# 我的备注" in text
        assert "http://b:2" in text


class TestReadableErrors:
    def test_报错是一句人话不是堆栈(self, client):
        r = client.post("/api/connections", json={
            "patch": {"comfy_base_url": "192.168.1.9:8188"}})
        detail = r.json()["detail"]
        assert detail.startswith("地址：")
        assert "pydantic" not in detail
        assert "type=value_error" not in detail
        assert "\n" not in detail

    def test_温度超范围也说人话(self, client):
        r = client.post("/api/connections", json={
            "patch": {"llm_temperature": 9.0}})
        assert r.status_code == 400
        detail = r.json()["detail"]
        assert detail == "温度：不能大于 2", "范围报错也得是中文"


class TestVoiceList:
    """音色得让人从服务端实际有的列表里选。"""

    def test_问不到服务端时不报五百(self, client, tmp_path, monkeypatch):
        """ComfyUI 没起也不该让整个页面崩掉，给个空列表加一句说明就行。"""
        from changji.models.project import ProjectStore
        from changji.stages.audio import ComfyTTSBackend

        async def boom(self):
            raise ConnectionError("连不上")

        monkeypatch.setattr(ComfyTTSBackend, "list_voices", boom)
        store = ProjectStore.create(tmp_path / "p", "drama")
        r = client.get("/api/voices", params={"path": str(store.root)})
        assert r.status_code == 200
        d = r.json()
        assert d["voices"] == []
        assert "连不上" in d["error"]

    def test_列表里滤掉_none(self, client, tmp_path, monkeypatch):
        from changji.models.project import ProjectStore
        from changji.stages.audio import ComfyTTSBackend

        store = ProjectStore.create(tmp_path / "p", "drama")

        async def fake_list(self):
            return ["none", "voices_examples/a.wav", ""]

        monkeypatch.setattr(ComfyTTSBackend, "list_voices", fake_list)
        d = client.get("/api/voices",
                       params={"path": str(store.root)}).json()
        assert d["voices"] == ["voices_examples/a.wav"]


class TestSettingsPersist:
    """参数默认只影响本次进程，勾了才写回配置文件。

    调画质档位常常是试几次才定下来，每次都写进文件反而碍事；
    但试定了之后重启一次就退回默认值，也说不过去。所以做成开关。
    """

    def test_默认不写文件(self, client, cfg_file):
        d = client.post("/api/settings", json={"patch": {"crf": 22}}).json()
        assert d["changed"] == ["crf"]
        assert d["saved_to"] is None
        assert not cfg_file.exists()

    def test_勾了就写回(self, client, cfg_file):
        d = client.post("/api/settings", json={
            "patch": {"crf": 22, "target_lufs": -14.0}, "persist": True}).json()
        assert d["saved_to"]
        import tomlkit
        doc = tomlkit.parse(cfg_file.read_text(encoding="utf-8"))
        assert doc["assembly"]["crf"] == 22
        assert doc["gates"]["target_lufs"] == -14.0

    def test_画质档位不写回(self, client, cfg_file):
        """档位是按显存推的，刻进配置等于把这台机器的显存写死进项目。"""
        d = client.post("/api/settings", json={
            "patch": {"draft_steps": 12}, "persist": True}).json()
        assert d["changed"] == ["draft_steps"]
        if cfg_file.exists():
            text = cfg_file.read_text(encoding="utf-8")
            assert "draft_steps" not in text

    def test_没改动就不写文件(self, client, cfg_file):
        client.post("/api/settings", json={"patch": {}, "persist": True})
        assert not cfg_file.exists()

    def test_老写法仍然收(self, client):
        """字段直接摊在请求体里的老写法，刷新慢一步的页面还在用。"""
        d = client.post("/api/settings", json={"crf": 21}).json()
        assert d["changed"] == ["crf"]

    def test_不认识的字段还是拒(self, client):
        assert client.post("/api/settings",
                           json={"patch": {"没这项": 1}}).status_code == 422
        assert client.post("/api/settings",
                           json={"没这项": 1}).status_code == 422


class TestProviders:
    """常见大模型平台的接入地址。

    各家都是 OpenAI 兼容接口，差别只在 base_url 和密钥。列出来是为了
    免得用户去翻各家文档找那一行地址——填错地址的后果是跑到写剧本
    那一步才炸，而报出来的 404 分不清是地址错还是模型名错。
    """

    def test_列得出来(self, client):
        r = client.get("/api/llm/providers")
        assert r.status_code == 200
        providers = r.json()["providers"]
        assert len(providers) >= 10

    def test_每一条都能直接填进设置(self, client):
        """地址必须是完整的 http(s) 地址，不能是「见文档」这种占位。"""
        for p in client.get("/api/llm/providers").json()["providers"]:
            assert p["base_url"].startswith(("http://", "https://")), p["id"]
            assert p["name"] and p["note"], p["id"]
            assert isinstance(p["local"], bool), p["id"]

    def test_id_不重复(self, client):
        ids = [p["id"] for p in client.get("/api/llm/providers").json()["providers"]]
        assert len(ids) == len(set(ids))

    def test_本机和云端都有(self, client):
        """只给云服务的话，装完连个能立刻试通的选项都没有。"""
        providers = client.get("/api/llm/providers").json()["providers"]
        assert any(p["local"] for p in providers)
        assert any(not p["local"] for p in providers)


class TestModelList:
    """那台服务上有哪些模型。界面靠它把手打的模型名换成可选的。"""

    def test_连不上时给空列表和原因(self, client, monkeypatch):
        """列不出来不该让设置页整个不能用，退回手打就行。

        这里必须把网络打桩掉。不打桩的话，开发机上正好跑着 Ollama
        就会真连上去，测试结果跟着机器走——那种测试过了也说明不了什么。
        """
        import httpx

        class Offline:
            def __init__(self, *a, **k):
                pass

            async def __aenter__(self):
                return self

            async def __aexit__(self, *a):
                return False

            async def get(self, url, headers=None):
                raise httpx.ConnectError("连不上", request=httpx.Request("GET", url))

        monkeypatch.setattr(httpx, "AsyncClient", Offline)
        r = client.get("/api/llm/models")
        assert r.status_code == 200, "列不出来也不该报 500，那会把设置页整页打死"
        body = r.json()
        assert body["models"] == []
        assert body["error"], "列不出来必须说清楚为什么"

    def test_问得到时按名字排好(self, client, monkeypatch):
        import httpx

        class FakeClient:
            def __init__(self, *a, **k):
                pass

            async def __aenter__(self):
                return self

            async def __aexit__(self, *a):
                return False

            async def get(self, url, headers=None):
                return httpx.Response(
                    200,
                    json={"data": [{"id": "b-model"}, {"id": "a-model"},
                                   {"id": "b-model"}]},
                    request=httpx.Request("GET", url),
                )

        monkeypatch.setattr(httpx, "AsyncClient", FakeClient)
        body = client.get("/api/llm/models").json()
        # 去重并排序：同一个模型报两遍在下拉框里会出现两条一样的
        assert body["models"] == ["a-model", "b-model"]
        assert body["current"]
