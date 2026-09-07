# -*- coding: utf-8 -*-
"""工作流加载。

命令行和界面共用一份逻辑。之前两处各写一遍，结果配音工作流做好了
却没有任何地方去加载它，整条链路默默走了估算后端。
"""
import json

import pytest

from changji import bundled_workflow
from changji.comfy.workflow import ApiWorkflow
from changji.models.project import ProjectStore
from changji.workflows_loader import load_all, load_workflow


class FakeClient:
    """假客户端。

    内置的视频工作流是界面版，转成接口版需要服务端的节点定义，
    所以这里给一份最小的假定义。配音工作流是接口版，不走这条路。
    """

    async def converter(self):
        from changji.comfy.workflow import WorkflowConverter
        return WorkflowConverter({
            "UNETLoader": {"input": {"required": {
                "unet_name": [["a.safetensors"]], "weight_dtype": [["default"]]}}},
            "CLIPLoader": {"input": {"required": {
                "clip_name": [["c.safetensors"]], "type": [["wan"]],
                "device": [["default"]]}}},
            "VAELoader": {"input": {"required": {"vae_name": [["v.safetensors"]]}}},
            "ModelSamplingSD3": {"input": {"required": {
                "model": ["MODEL"], "shift": ["FLOAT"]}}},
            "CLIPTextEncode": {"input": {"required": {
                "clip": ["CLIP"], "text": ["STRING"]}}},
            "LoadImage": {"input": {"required": {"image": [["x.png"]],
                                                 "upload": [["image"]]}}},
            "Wan22ImageToVideoLatent": {"input": {
                "required": {"vae": ["VAE"], "width": ["INT"], "height": ["INT"],
                             "length": ["INT"], "batch_size": ["INT"]},
                "optional": {"start_image": ["IMAGE"]}}},
            "KSampler": {"input": {"required": {
                "model": ["MODEL"], "seed": ["INT"], "steps": ["INT"],
                "cfg": ["FLOAT"], "sampler_name": [["euler"]],
                "scheduler": [["simple"]], "positive": ["CONDITIONING"],
                "negative": ["CONDITIONING"], "latent_image": ["LATENT"],
                "denoise": ["FLOAT"]}}},
            "VAEDecode": {"input": {"required": {"samples": ["LATENT"],
                                                 "vae": ["VAE"]}}},
            "CreateVideo": {"input": {
                "required": {"images": ["IMAGE"], "fps": ["FLOAT"]},
                "optional": {"audio": ["AUDIO"], "bit_depth": ["COMBO"],
                             "color_space": ["COMBO"]}}},
            "SaveVideo": {"input": {
                "required": {"video": ["VIDEO"], "filename_prefix": ["STRING"],
                             "format": ["COMBO"]},
                "optional": {"codec": ["COMBO"]}}},
        })


def _api_workflow(path, node_class="X"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({
        "1": {"class_type": node_class, "inputs": {"text": ""}}
    }), encoding="utf-8")


class TestLoadWorkflow:
    async def test_内置视频工作流存在(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        assert bundled_workflow("video.json").is_file()

    async def test_内置配音工作流存在(self, tmp_path):
        assert bundled_workflow("tts.json").is_file()

    async def test_项目里的工作流覆盖内置(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        _api_workflow(store.root / "workflows" / "tts.json", "ProjectSpecific")
        wf = await load_workflow(FakeClient(), store, "tts")
        assert wf.prompt["1"]["class_type"] == "ProjectSpecific"

    async def test_没有内置也没有项目的工作流时返回空(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        wf = await load_workflow(FakeClient(), store, "不存在的", required=False)
        assert wf is None

    async def test_必需的工作流缺失时报错指出放哪(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        with pytest.raises(FileNotFoundError, match="workflows/不存在的.json"):
            await load_workflow(FakeClient(), store, "不存在的", required=True)


class TestLoadAll:
    async def test_视频和配音都加载(self, tmp_path):
        """两者都有内置版本，所以都该拿到。"""
        store = ProjectStore.create(tmp_path / "p", "p")
        wfs = await load_all(FakeClient(), store)
        assert isinstance(wfs["video"], ApiWorkflow)
        assert isinstance(wfs["tts"], ApiWorkflow)

    async def test_没有图像工作流时返回空而不是报错(self, tmp_path):
        """缺了它有退路：用视频模型出单帧。"""
        store = ProjectStore.create(tmp_path / "p", "p")
        wfs = await load_all(FakeClient(), store)
        assert wfs["image"] is None

    async def test_配音工作流真的被接上(self, tmp_path):
        """回归测试。

        配音后端和工作流都做好了，但命令行和界面各写了一份加载逻辑，
        两份都只加载视频工作流，配音那份从来没被读过。
        """
        store = ProjectStore.create(tmp_path / "p", "p")
        wfs = await load_all(FakeClient(), store)
        assert wfs["tts"] is not None, "配音工作流没有被加载"

        from changji.config import TTSConfig
        from changji.stages.audio import ComfyTTSBackend, build_backend
        backend = build_backend(
            TTSConfig(backend="comfy"), FakeClient(), wfs["tts"], store.paths)
        assert isinstance(backend, ComfyTTSBackend), "配音后端没有被启用"


class TestBundledTTSWorkflow:
    def test_配音工作流结构正确(self):
        raw = json.loads(bundled_workflow("tts.json").read_text(encoding="utf-8"))
        classes = {n["class_type"] for n in raw.values()}
        assert "UnifiedTTSTextNode" in classes
        assert any("Engine" in c for c in classes)

    def test_有可填文本的节点(self):
        """没有的话后端会报错而不是静默产出空音频。"""
        from changji.stages.audio import _apply_text
        raw = json.loads(bundled_workflow("tts.json").read_text(encoding="utf-8"))
        assert _apply_text(ApiWorkflow(raw), "台词", None, "neutral") is True

    def test_引擎许可证可商用(self):
        """内置工作流用的引擎必须可商用。

        同一个节点包里的 IndexTTS 商用授权有争议，Fish Speech 权重是
        非商用许可，都不能作为默认。CosyVoice 3 是 Apache 2.0。
        """
        raw = json.loads(bundled_workflow("tts.json").read_text(encoding="utf-8"))
        engines = [n["class_type"] for n in raw.values() if "Engine" in n["class_type"]]
        assert engines, "工作流里没有引擎节点"
        banned = ("IndexTTS", "FishAudio", "Higgs")
        for e in engines:
            assert not any(b in e for b in banned), f"{e} 的商用授权有问题"

    def test_没有缓存以免失败结果被复用(self):
        """节点失败时会输出空音频，缓存下来后后续都拿到同一个空文件。"""
        raw = json.loads(bundled_workflow("tts.json").read_text(encoding="utf-8"))
        unified = [n for n in raw.values()
                   if n["class_type"] == "UnifiedTTSTextNode"]
        assert unified
        assert unified[0]["inputs"].get("enable_audio_cache") is False
