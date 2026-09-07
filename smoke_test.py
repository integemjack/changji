# -*- coding: utf-8 -*-
"""提交一个低配置的 Wan 2.2 任务，验证整条链路能跑通（模型加载 → 采样 → 解码 → 存视频）。"""
import json, sys, time, urllib.request, urllib.error

HOST = "http://127.0.0.1:8188"
WF = r"E:\AI短剧\ComfyUI\user\default\workflows\Wan2.2_5B_图生视频.json"


def api(path, payload=None):
    url = f"{HOST}{path}"
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data,
                                 headers={"Content-Type": "application/json"} if data else {})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read() or b"{}")


def to_api_format(wf):
    """把界面版工作流转成 /prompt 接口要的格式。"""
    nodes = {n["id"]: n for n in wf["nodes"]}
    # link id -> (源节点, 源槽位)
    src = {l[0]: (l[1], l[2]) for l in wf["links"]}
    out = {}
    for nid, n in nodes.items():
        inputs = {}
        # 连线输入
        for i in n.get("inputs", []):
            if i.get("link") is not None:
                s_node, s_slot = src[i["link"]]
                inputs[i["name"]] = [str(s_node), s_slot]
        # 控件输入：按节点定义顺序填，跳过 KSampler 的 control_after_generate
        w = list(n.get("widgets_values") or [])
        names = WIDGET_NAMES.get(n["type"])
        if names:
            for name, val in zip(names, w):
                inputs[name] = val
        out[str(nid)] = {"class_type": n["type"], "inputs": inputs}
    return out


WIDGET_NAMES = {
    "UNETLoader": ["unet_name", "weight_dtype"],
    "CLIPLoader": ["clip_name", "type", "device"],
    "VAELoader": ["vae_name"],
    "ModelSamplingSD3": ["shift"],
    "CLIPTextEncode": ["text"],
    "LoadImage": ["image", "upload"],
    "Wan22ImageToVideoLatent": ["width", "height", "length", "batch_size"],
    # KSampler 的 widgets_values 里第 2 个是界面用的 control_after_generate，接口不要
    "KSampler": ["seed", "__skip__", "steps", "cfg", "sampler_name", "scheduler", "denoise"],
    "VAEDecode": [],
    "CreateVideo": ["fps", "bit_depth", "color_space"],
    "SaveVideo": ["filename_prefix", "format", "codec"],
}


def main():
    wf = json.load(open(WF, encoding="utf-8"))
    prompt = to_api_format(wf)
    for n in prompt.values():
        n["inputs"].pop("__skip__", None)

    # 冒烟测试：压到最小规模，只验证链路，不追求画质
    prompt["8"]["inputs"].update({"width": 640, "height": 352, "length": 25})
    prompt["9"]["inputs"].update({"steps": 4})
    prompt["12"]["inputs"]["filename_prefix"] = "video/smoke_test"

    print("提交任务 ...")
    r = api("/prompt", {"prompt": prompt})
    pid = r["prompt_id"]
    print("prompt_id:", pid)

    t0 = time.time()
    while time.time() - t0 < 1800:
        h = api(f"/history/{pid}")
        if pid in h:
            entry = h[pid]
            status = entry.get("status", {})
            if status.get("status_str") == "error" or not status.get("completed", True):
                for m in status.get("messages", []):
                    if m[0] in ("execution_error", "execution_interrupted"):
                        print("执行失败:", json.dumps(m[1], ensure_ascii=False)[:600])
                        return 1
            outs = entry.get("outputs", {})
            print("完成，耗时 %.0f 秒" % (time.time() - t0))
            print("输出:", json.dumps(outs, ensure_ascii=False)[:500])
            return 0
        time.sleep(5)
    print("超时")
    return 1


if __name__ == "__main__":
    sys.exit(main())
