# -*- coding: utf-8 -*-
"""工作流格式转换。ComfyUI 的界面版和接口版格式不同，转换是最容易出错的一环。"""
import pytest

from changji.comfy.workflow import (
    ApiWorkflow, WorkflowConverter, WorkflowError, _is_widget_type,
)

# 仿真的 object_info。类型名的形状照抄真实服务端。
OBJECT_INFO = {
    "KSampler": {"input": {"required": {
        "model": ["MODEL"], "seed": ["INT"], "steps": ["INT"], "cfg": ["FLOAT"],
        "sampler_name": [["euler", "uni_pc"]], "scheduler": [["simple", "normal"]],
        "positive": ["CONDITIONING"], "negative": ["CONDITIONING"],
        "latent_image": ["LATENT"], "denoise": ["FLOAT"],
    }}},
    "UNETLoader": {"input": {"required": {
        "unet_name": [["a.safetensors"]], "weight_dtype": [["default"]],
    }}},
    "SaveVideo": {"input": {
        "required": {"video": ["VIDEO"], "filename_prefix": ["STRING"],
                     "format": ["COMFY_DYNAMICCOMBO_V3"]},
        "optional": {"codec": ["COMFY_DYNAMICCOMBO_V3"]},
    }},
    "Wan22ImageToVideoLatent": {"input": {
        "required": {"vae": ["VAE"], "width": ["INT"], "height": ["INT"],
                     "length": ["INT"], "batch_size": ["INT"]},
        "optional": {"start_image": ["IMAGE"]},
    }},
}


class TestWidgetTypeDetection:
    def test_标量类型是控件(self):
        for t in ("INT", "FLOAT", "STRING", "BOOLEAN"):
            assert _is_widget_type(t) is True

    def test_连线类型不是控件(self):
        for t in ("MODEL", "CLIP", "VAE", "IMAGE", "LATENT", "CONDITIONING",
                  "AUDIO", "VIDEO", "MASK"):
            assert _is_widget_type(t) is False

    def test_下拉框是控件(self):
        assert _is_widget_type([["euler", "uni_pc"]][0]) is True
        assert _is_widget_type("COMBO") is True
        assert _is_widget_type("COMFY_DYNAMICCOMBO_V3") is True

    def test_int和float虽然全大写但是控件(self):
        """回归测试。

        早期版本用「全大写即连线型」判断，把 INT 和 FLOAT 也排除了，
        结果 KSampler 只认出 sampler_name 和 scheduler 两个控件，
        seed / steps / cfg / denoise 全部丢失。
        """
        conv = WorkflowConverter(OBJECT_INFO)
        assert conv.widget_names("KSampler") == [
            "seed", "steps", "cfg", "sampler_name", "scheduler", "denoise"]


class TestConversion:
    def _ui(self):
        return {
            "nodes": [
                {"id": 1, "type": "UNETLoader", "mode": 0, "inputs": [],
                 "outputs": [{"name": "MODEL", "type": "MODEL", "links": [1]}],
                 "widgets_values": ["a.safetensors", "default"]},
                {"id": 9, "type": "KSampler", "mode": 0,
                 "inputs": [{"name": "model", "type": "MODEL", "link": 1},
                            {"name": "positive", "type": "CONDITIONING", "link": None},
                            {"name": "negative", "type": "CONDITIONING", "link": None},
                            {"name": "latent_image", "type": "LATENT", "link": None}],
                 "outputs": [],
                 # 界面上 seed 后面跟着一个伪控件
                 "widgets_values": [12345, "randomize", 30, 5.0, "uni_pc", "simple", 1.0]},
            ],
            "links": [[1, 1, 0, 9, 0, "MODEL"]],
        }

    def test_伪控件不会导致参数错位(self):
        """最隐蔽的坑。

        界面在 seed 后面插了个 control_after_generate 下拉框，它占 widgets_values
        一格但接口不认。不跳过它，steps 会拿到 "randomize"，后面全部错位。
        """
        api = WorkflowConverter(OBJECT_INFO).convert(self._ui())
        ks = api.prompt["9"]["inputs"]
        assert ks["seed"] == 12345
        assert ks["steps"] == 30
        assert ks["cfg"] == 5.0
        assert ks["sampler_name"] == "uni_pc"
        assert ks["scheduler"] == "simple"
        assert ks["denoise"] == 1.0
        assert "randomize" not in ks.values()

    def test_连线被正确还原(self):
        api = WorkflowConverter(OBJECT_INFO).convert(self._ui())
        assert api.prompt["9"]["inputs"]["model"] == ["1", 0]

    def test_未连线的输入不出现(self):
        api = WorkflowConverter(OBJECT_INFO).convert(self._ui())
        assert "positive" not in api.prompt["9"]["inputs"]

    def test_静音和旁路节点不提交(self):
        ui = self._ui()
        ui["nodes"][0]["mode"] = 2  # 静音
        api = WorkflowConverter(OBJECT_INFO).convert(ui)
        assert "1" not in api.prompt

    def test_接口版直接传入会报错提示(self):
        with pytest.raises(WorkflowError, match="不是界面版"):
            WorkflowConverter(OBJECT_INFO).convert({"9": {"class_type": "KSampler"}})

    def test_未知节点类型报错说明原因(self):
        with pytest.raises(WorkflowError, match="自定义节点"):
            WorkflowConverter(OBJECT_INFO).widget_names("SomeCustomNode")


class TestApiWorkflow:
    def _api(self):
        return ApiWorkflow({
            "9": {"class_type": "KSampler", "inputs": {"steps": 30}},
            "8": {"class_type": "Wan22ImageToVideoLatent",
                  "inputs": {"width": 1280, "height": 704, "length": 121}},
            "5": {"class_type": "CLIPTextEncode", "inputs": {"text": "a"}},
            "6": {"class_type": "CLIPTextEncode", "inputs": {"text": "b"}},
        })

    def test_按类型改参数(self):
        api = self._api()
        api.set_by_class("Wan22ImageToVideoLatent", width=640, height=352, length=25)
        assert api.prompt["8"]["inputs"]["width"] == 640
        assert api.prompt["8"]["inputs"]["length"] == 25

    def test_类型不唯一时拒绝猜测(self):
        """两个 CLIPTextEncode 分别是正负提示词，猜错就全反了。"""
        with pytest.raises(WorkflowError, match="无法确定改哪个"):
            self._api().one_by_class("CLIPTextEncode")

    def test_类型不存在时报错(self):
        with pytest.raises(WorkflowError, match="没有"):
            self._api().one_by_class("LoadImage")

    def test_copy_是深拷贝(self):
        a = self._api()
        b = a.copy()
        b.set_by_class("KSampler", steps=4)
        assert a.prompt["9"]["inputs"]["steps"] == 30
