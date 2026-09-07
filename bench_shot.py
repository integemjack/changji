# -*- coding: utf-8 -*-
"""实测不同画质档位下单个镜头的生成耗时，用于推算全自动产能。"""
import json, time, urllib.request

HOST = "http://127.0.0.1:8188"
WF = r"E:\AI短剧\ComfyUI\user\default\workflows\Wan2.2_5B_图生视频.json"

WIDGET_NAMES = {
    "UNETLoader": ["unet_name", "weight_dtype"],
    "CLIPLoader": ["clip_name", "type", "device"],
    "VAELoader": ["vae_name"],
    "ModelSamplingSD3": ["shift"],
    "CLIPTextEncode": ["text"],
    "LoadImage": ["image", "upload"],
    "Wan22ImageToVideoLatent": ["width", "height", "length", "batch_size"],
    "KSampler": ["seed", "__skip__", "steps", "cfg", "sampler_name", "scheduler", "denoise"],
    "VAEDecode": [],
    "CreateVideo": ["fps", "bit_depth", "color_space"],
    "SaveVideo": ["filename_prefix", "format", "codec"],
}


def api(path, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(f"{HOST}{path}", data=data,
                                 headers={"Content-Type": "application/json"} if data else {})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read() or b"{}")


def to_api(wf):
    nodes = {n["id"]: n for n in wf["nodes"]}
    src = {l[0]: (l[1], l[2]) for l in wf["links"]}
    out = {}
    for nid, n in nodes.items():
        inputs = {}
        for i in n.get("inputs", []):
            if i.get("link") is not None:
                s, slot = src[i["link"]]
                inputs[i["name"]] = [str(s), slot]
        for name, val in zip(WIDGET_NAMES.get(n["type"], []), n.get("widgets_values") or []):
            inputs[name] = val
        inputs.pop("__skip__", None)
        out[str(nid)] = {"class_type": n["type"], "inputs": inputs}
    return out


def run(base, name, w, h, length, steps):
    p = json.loads(json.dumps(base))
    p["8"]["inputs"].update({"width": w, "height": h, "length": length})
    p["9"]["inputs"].update({"steps": steps, "seed": 12345})
    p["12"]["inputs"]["filename_prefix"] = f"video/bench_{name}"
    t0 = time.time()
    pid = api("/prompt", {"prompt": p})["prompt_id"]
    while time.time() - t0 < 3600:
        h_ = api(f"/history/{pid}")
        if pid in h_:
            st = h_[pid].get("status", {})
            if st.get("status_str") == "error":
                print(f"{name}: 失败")
                return None
            el = time.time() - t0
            sec = length / 24.0
            print(f"{name:14} {w}x{h} {length}帧 {steps}步 -> {el:6.1f}秒  "
                  f"(片长 {sec:.1f}秒, 实时比 {el/sec:5.1f}x)")
            return el
        time.sleep(3)
    print(f"{name}: 超时")
    return None


if __name__ == "__main__":
    base = to_api(json.load(open(WF, encoding="utf-8")))
    print("档位对比（第一次含模型加载，后续为热启动）\n")
    run(base, "warmup", 640, 352, 25, 4)
    print()
    run(base, "草稿档", 640, 352, 121, 10)
    run(base, "预览档", 960, 544, 121, 20)
    run(base, "成片档", 1280, 704, 121, 30)
