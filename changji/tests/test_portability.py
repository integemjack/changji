# -*- coding: utf-8 -*-
"""可移植性相关的测试。这些是「能在别的电脑上用」的保证。"""
import pytest

from changji.config import Settings, load_settings, write_default_config
from changji.hardware import HardwareProfile, Tier, tiers_for_vram


class TestConfigPortability:
    def test_默认配置不含任何绝对路径(self):
        s = Settings()
        assert s.workspace is None
        assert "127.0.0.1" in s.comfy.base_url

    def test_环境变量可覆盖(self, monkeypatch, tmp_path):
        monkeypatch.setenv("CHANGJI_COMFY_BASE_URL", "http://192.168.1.50:8188")
        monkeypatch.setenv("CHANGJI_LLM_MODEL", "qwen3:32b")
        monkeypatch.setenv("CHANGJI_VRAM_GB", "24")
        s = load_settings(project_dir=tmp_path)
        assert s.comfy.base_url == "http://192.168.1.50:8188"
        assert s.llm.model == "qwen3:32b"
        assert s.vram_gb_override == 24

    def test_项目配置覆盖全局(self, tmp_path, monkeypatch):
        monkeypatch.delenv("CHANGJI_COMFY_BASE_URL", raising=False)
        (tmp_path / "changji.toml").write_text(
            '[comfy]\nbase_url = "http://10.0.0.7:8188"\n', encoding="utf-8")
        s = load_settings(project_dir=tmp_path)
        assert s.comfy.base_url == "http://10.0.0.7:8188"

    def test_comfy_地址会去掉尾斜杠(self):
        s = Settings.model_validate({"comfy": {"base_url": "http://host:8188/"}})
        assert s.comfy.base_url == "http://host:8188"

    def test_comfy_地址必须带协议(self):
        with pytest.raises(ValueError, match="http"):
            Settings.model_validate({"comfy": {"base_url": "192.168.1.5:8188"}})

    def test_websocket_地址由_http_推导(self):
        s = Settings.model_validate({"comfy": {"base_url": "https://gpu.local:8188"}})
        assert s.comfy.ws_url == "wss://gpu.local:8188/ws"

    def test_workspace_可指向任意位置(self, tmp_path):
        s = Settings(workspace=str(tmp_path / "我的项目"))
        assert s.workspace_path().name == "我的项目"

    def test_未设_workspace_时用系统数据目录(self):
        assert Settings().workspace_path().is_absolute()

    def test_配置模板可生成且能被读回(self, tmp_path):
        p = write_default_config(tmp_path / "config.toml")
        assert p.is_file()
        s = load_settings(project_dir=tmp_path.parent)  # 不含项目配置
        assert isinstance(s, Settings)

    def test_错拼的配置项会报错而不是被忽略(self):
        with pytest.raises(ValueError):
            Settings.model_validate({"comfy": {"base_urls": "http://x:1"}})


class TestHardwareTiers:
    def test_档位随显存变化(self):
        small = tiers_for_vram(8.0)
        big = tiers_for_vram(24.0)
        assert small[Tier.FINAL].width < big[Tier.FINAL].width

    def test_草稿档一定比成片档便宜(self):
        for vram in (8.0, 12.0, 16.0, 24.0):
            t = tiers_for_vram(vram)
            d, f = t[Tier.DRAFT], t[Tier.FINAL]
            assert d.width * d.height * d.steps < f.width * f.height * f.steps
            assert d.measured_seconds < f.measured_seconds

    def test_16g_档位与本机实测一致(self):
        """基准点来自 RTX 5080 实测，不能被无意改掉。"""
        t = tiers_for_vram(16.0)
        assert (t[Tier.DRAFT].width, t[Tier.DRAFT].height, t[Tier.DRAFT].steps) == (640, 352, 10)
        assert (t[Tier.FINAL].width, t[Tier.FINAL].height, t[Tier.FINAL].steps) == (1280, 704, 30)
        assert t[Tier.FINAL].measured_seconds == pytest.approx(392.0, abs=1)

    def test_超小显存不会崩(self):
        t = tiers_for_vram(4.0)
        assert t[Tier.DRAFT].width >= 32

    def test_分辨率都是32的倍数(self):
        """不是 32 的倍数会导致潜空间对不齐。"""
        for vram in (4.0, 8.0, 12.0, 16.0, 24.0, 48.0):
            for spec in tiers_for_vram(vram).values():
                assert spec.width % 32 == 0 and spec.height % 32 == 0

    def test_竖屏转换保持32倍数且长边在下(self):
        spec = tiers_for_vram(16.0)[Tier.FINAL].scaled_to("9:16")
        assert spec.width % 32 == 0 and spec.height % 32 == 0
        assert spec.height > spec.width

    def test_探测不到显卡时给保守假设且标记未探测(self):
        p = HardwareProfile.detect(override_vram_gb=None)
        assert p.vram_gb > 0
        assert isinstance(p.detected, bool)

    def test_可手动覆盖显存(self):
        """ComfyUI 在别的机器上时本机探测不到，必须能手动指定。"""
        p = HardwareProfile.detect(override_vram_gb=24.0)
        assert p.vram_gb == 24.0
        assert p.detected is False
        assert p.tiers[Tier.FINAL].width == 1920

    def test_排产估算(self):
        p = HardwareProfile.detect(override_vram_gb=16.0)
        total = p.estimate_episode(shot_count=36, tier=Tier.FINAL)
        assert total == pytest.approx(392.0 * 36, rel=0.01)

    def test_标称显存被驱动占用后仍落对档位(self):
        """回归测试。

        nvidia-smi 报的永远小于标称值，一张 16GB 卡通常报 15.9。
        早期版本按 >=16.0 卡阈值，把这台机器错判成了 12GB 档，
        分辨率和步数全被降级。所有整数标称容量都要留余量。
        """
        cases = {8.0: 7.9, 12.0: 11.8, 16.0: 15.9, 24.0: 23.6, 32.0: 31.5}
        for nominal, reported in cases.items():
            assert tiers_for_vram(reported) == tiers_for_vram(nominal), (
                f"标称 {nominal}GB 的卡报 {reported}GB 时掉档了"
            )

    def test_16g_卡拿到16g档而不是12g档(self):
        t = tiers_for_vram(15.9)
        assert (t[Tier.FINAL].width, t[Tier.FINAL].height) == (1280, 704)
