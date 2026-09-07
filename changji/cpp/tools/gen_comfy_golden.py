"""生成 comfy 工作流转换的对拍语料。

期望值**全部由 Python 那边的真函数算出来**，不是手写的——手写的话对拍
只能证明"C++ 和我理解的一致"，而这里要证明的是"C++ 和现有实现一致"。

`/object_info` 是**合成的**，因为它要从一台跑着的 ComfyUI 上取，而对拍
不该依赖外部服务。这不削弱对拍的价值：object_info 在这里是**输入**，
和工作流文件一样。要证明的是同一份输入下两边转出同一份结果。

合成时按 ComfyUI 里这些节点真实的控件顺序写，尤其是 KSampler——
seed 后面那个 control_after_generate 是整个转换里最容易错的地方。

用法（在 changji/ 下）：
    .venv/Scripts/python.exe cpp/tools/gen_comfy_golden.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
CPP = HERE.parent.parent
ROOT = CPP.parent
sys.path.insert(0, str(ROOT / "src"))

from changji.comfy.workflow import WorkflowConverter, load_ui_workflow  # noqa: E402

OUT = CPP / "tests" / "golden" / "comfy"


def combo(*options):
    """下拉框输入。选项直接列在类型位置上。"""
    return [list(options)]


def scalar(kind, **opts):
    return [kind, opts] if opts else [kind]


def link(kind):
    """连线型输入。不占 widgets_values 的位置。"""
    return [kind]


# 合成的 /object_info。键顺序 = 界面上控件的顺序，这一点是转换的全部依据。
OBJECT_INFO = {
    "UNETLoader": {"input": {"required": {
        "unet_name": combo("wan2.2_ti2v_5B_fp16.safetensors"),
        "weight_dtype": combo("default", "fp8_e4m3fn"),
    }}},
    "CLIPLoader": {"input": {"required": {
        "clip_name": combo("umt5_xxl_fp8_e4m3fn_scaled.safetensors"),
        "type": combo("wan", "sd3"),
        "device": combo("default", "cpu"),
    }}},
    "VAELoader": {"input": {"required": {
        "vae_name": combo("wan2.2_vae.safetensors"),
    }}},
    "ModelSamplingSD3": {"input": {"required": {
        "model": link("MODEL"),
        "shift": scalar("FLOAT", default=3.0),
    }}},
    "CLIPTextEncode": {"input": {"required": {
        "text": scalar("STRING", multiline=True),
        "clip": link("CLIP"),
    }}},
    "LoadImage": {"input": {"required": {
        "image": combo("example.png"),
        "upload": ["IMAGEUPLOAD"],
    }}},
    "Wan22ImageToVideoLatent": {"input": {"required": {
        "vae": link("VAE"),
        "width": scalar("INT"),
        "height": scalar("INT"),
        "length": scalar("INT"),
        "batch_size": scalar("INT"),
    }, "optional": {
        "start_image": link("IMAGE"),
    }}},
    # 整个转换里最难的一个：seed 之后界面插了 control_after_generate，
    # 它占 widgets_values 一格但接口不认。不跳过的话后面全部错位一格。
    "KSampler": {"input": {"required": {
        "model": link("MODEL"),
        "seed": scalar("INT"),
        "steps": scalar("INT"),
        "cfg": scalar("FLOAT"),
        "sampler_name": combo("uni_pc", "euler"),
        "scheduler": combo("simple", "normal"),
        "positive": link("CONDITIONING"),
        "negative": link("CONDITIONING"),
        "latent_image": link("LATENT"),
        "denoise": scalar("FLOAT"),
    }}},
    "VAEDecode": {"input": {"required": {
        "samples": link("LATENT"),
        "vae": link("VAE"),
    }}},
    "CreateVideo": {"input": {"required": {
        "images": link("IMAGE"),
        "fps": scalar("FLOAT"),
        "pix_fmt": combo("auto", "yuv420p"),
        "color_space": combo("sRGB", "auto"),
    }}},
    "SaveVideo": {"input": {"required": {
        "video": link("VIDEO"),
        "filename_prefix": scalar("STRING"),
        "format": combo("auto", "mp4"),
        "codec": combo("auto", "h264"),
    }}},
}


def synthetic_cases():
    """几个专门盯着一个坑的小工作流。"""
    return {
        # 连线覆盖控件：text 由连线提供时不该再从 widgets_values 取，
        # **而且不消耗一格**。消耗掉的话后面参数全错位。
        "linked_overrides_widget": {
            "nodes": [
                {"id": 1, "type": "CLIPLoader", "mode": 0,
                 "widgets_values": ["umt5.safetensors", "wan", "default"]},
                {"id": 2, "type": "CLIPTextEncode", "mode": 0,
                 "inputs": [{"name": "text", "link": 10},
                            {"name": "clip", "link": 11}],
                 "widgets_values": ["这句不该被用上"]},
            ],
            "links": [[10, 99, 0, 2, 0, "STRING"], [11, 1, 0, 2, 1, "CLIP"]],
        },
        # 静音（2）和旁路（4）的节点不提交
        "muted_and_bypassed": {
            "nodes": [
                {"id": 1, "type": "VAELoader", "mode": 0,
                 "widgets_values": ["a.safetensors"]},
                {"id": 2, "type": "VAELoader", "mode": 2,
                 "widgets_values": ["静音的.safetensors"]},
                {"id": 3, "type": "VAELoader", "mode": 4,
                 "widgets_values": ["旁路的.safetensors"]},
            ],
            "links": [],
        },
        # 控件值比控件名多/少
        "widget_count_mismatch": {
            "nodes": [
                {"id": 1, "type": "VAELoader", "mode": 0,
                 "widgets_values": ["a.safetensors", "多出来的", "还有一个"]},
                {"id": 2, "type": "CLIPLoader", "mode": 0,
                 "widgets_values": ["只给了一个.safetensors"]},
            ],
            "links": [],
        },
        # seed 后面没有伪控件时不能乱跳一格
        "seed_without_pseudo_widget": {
            "nodes": [
                {"id": 9, "type": "KSampler", "mode": 0,
                 "widgets_values": [42, 30, 5, "uni_pc", "simple", 1]},
            ],
            "links": [],
        },
        # 节点 id 是字符串的老格式
        "string_node_ids": {
            "nodes": [
                {"id": "7", "type": "LoadImage", "mode": 0,
                 "widgets_values": ["a.png", "image"]},
            ],
            "links": [],
        },
        # 断掉的连线（link id 在 links 里找不到）要当成没连
        "dangling_link": {
            "nodes": [
                {"id": 1, "type": "VAEDecode", "mode": 0,
                 "inputs": [{"name": "samples", "link": 404},
                            {"name": "vae", "link": None}]},
            ],
            "links": [],
        },
    }


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    conv = WorkflowConverter(OBJECT_INFO)

    cases = {}
    bundled = ROOT / "src" / "changji" / "workflows" / "video.json"
    cases["bundled_video"] = load_ui_workflow(bundled)
    cases.update(synthetic_cases())

    payload = {
        "object_info": OBJECT_INFO,
        "cases": [],
    }
    for name, ui in cases.items():
        # 每个 case 用一个新的 converter，免得控件名缓存把顺序问题掩盖掉
        api = WorkflowConverter(OBJECT_INFO).convert(ui)
        payload["cases"].append({
            "name": name,
            "ui": ui,
            "expected": api.to_dict(),
        })
        print(f"{name}: {len(api.to_dict())} 个节点")

    # 控件名表也存一份：转换出错时先看它，能立刻分清是取名错了还是对位错了
    payload["widget_names"] = {
        cls: conv.widget_names(cls) for cls in sorted(OBJECT_INFO)
    }

    dest = OUT / "workflow_convert.json"
    dest.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"写到 {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
