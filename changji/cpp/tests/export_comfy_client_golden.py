r"""导出 ComfyUI 客户端那几个**纯函数**的结果，和 Python 逐条比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_comfy_client_golden.py

产出 cpp/tests/golden/comfy_client.json。

`comfy/workflow.py` 那一半已经有语料了（工作流 JSON 逐块比）。
这一份补 `comfy/client.py` 里剩下的、能脱开网络单独跑的三件事。
`contract_audit.py` 一直把 `test_comfy_client.cpp` 列在
「两边都有、又只钉了意图」那一类里。

**一、`JobResult.files()` / `first_file()` —— 挑错文件不会报错。**
`outputs` 是「节点 id -> {images: [...]}」。`files()` 把所有节点的摊平，
`first_file()` 取第一个。所以**哪个节点排在前面，决定了成片用哪张图**。

Python 那边是 dict，走插入顺序（也就是服务端 history JSON 里的顺序）。
C++ 用的是 `nlohmann::ordered_json`，也是插入顺序——这是有意选的。
可要是哪天有人顺手换成 `nlohmann::json`，它按键名字典序排，
`"10"` 会跑到 `"9"` 前面，`first_file()` 就换了一张图，
**而且一声不吭**：文件存在、格式正确、流程全绿，只是画面不对。
所以这份语料专门挑**插入顺序和字典序不一样**的节点 id。

顺带钉住一条：**视频节点也把结果放在 `images` 键下**，带 `animated` 标记。
按 `"videos"` 找一个都找不到——这条写在 C++ 头文件的注释里，
但没有任何东西去问 Python 是不是真这样。

**二、`ValidationError.human_summary()` —— 用户唯一能看懂的那句话。**
服务端拒了工作流，原样透传是一坨嵌套 JSON。这个函数把它翻成
"哪个节点要的哪个文件服务端上没有"。翻错了不会有人发现，
用户只会觉得"报错看不懂"，然后去装一堆其实不缺的东西。

**三、`_error_from_history()` —— 从 history 里挖执行失败的原因。**
形状是 `["execution_error", {...}]`，数组套数组。
形状认错了就退回一句没有信息量的"任务执行失败"。
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.comfy.client import (                            # noqa: E402
    ComfyClient, JobResult, PromptValidationError,
)

DEST = REPO / "cpp" / "tests" / "golden" / "comfy_client.json"


def outputs_cases() -> list[dict]:
    def img(node: str, name: str, **extra) -> dict:
        d = {"filename": name, "subfolder": "", "type": "output"}
        d.update(extra)
        return d

    cases: list[tuple[str, dict]] = []

    cases.append(("一个节点一张图",
                  {"9": {"images": [img("9", "out_001.png")]}}))

    # **插入顺序和字典序不一样**：插入 "9" 再 "10"，
    # 字典序是 "10" 在前。换成 nlohmann::json 就在这里露出来。
    cases.append(("两个节点，插入顺序 9→10（字典序相反）",
                  {"9": {"images": [img("9", "先出的.png")]},
                   "10": {"images": [img("10", "后出的.png")]}}))

    # 反过来插一遍：这一条字典序和插入序一致，
    # 两条一起看才能说明"跟的是插入序"而不是碰巧
    cases.append(("两个节点，插入顺序 10→9",
                  {"10": {"images": [img("10", "先出的.png")]},
                   "9": {"images": [img("9", "后出的.png")]}}))

    # 三个节点，中间那个没有 images
    cases.append(("中间的节点没有 images",
                  {"3": {"images": [img("3", "a.png")]},
                   "7": {"latents": [{"filename": "x.latent"}]},
                   "5": {"images": [img("5", "b.png")]}}))

    # 一个节点里好几张
    cases.append(("一个节点出好几张",
                  {"9": {"images": [img("9", "a.png"), img("9", "b.png"),
                                    img("9", "c.png")]}}))

    # **视频**：也在 images 下，带 animated
    cases.append(("视频节点的产出也在 images 下",
                  {"12": {"images": [img("12", "clip.mp4", animated=True)]}}))

    cases.append(("完全没有产出", {}))
    cases.append(("有节点但没有任何文件", {"9": {}}))
    cases.append(("images 不是数组", {"9": {"images": "坏掉的"}}))

    # 两边有意不一样的，在这里标出来，**不假装一致**。
    # 键是用例名，值是「C++ 怎么做、为什么」。
    DIVERGENT = {
        "images 不是数组":
            "Python 的 files() 是 out.extend(node_out.get(kind, []))，"
            "喂个字符串进去它按字符摊平：files() 回 ['坏','掉','的']，"
            "first_file() 回 '坏' —— 一个字符串，而调用方等的是带 filename "
            "的字典，于是炸在下游（'str' object has no attribute 'get'），"
            "报错指向的地方离真正的原因隔了好几层。"
            "C++ 判 is_array()，不是数组就跳过，first_file() 回 nullopt，"
            "调用方当成「没有产出文件」正常报错。C++ 这边是有意做得更稳。",
    }

    out = []
    for name, outputs in cases:
        r = JobResult(prompt_id="p1", outputs=outputs)
        row = {
            "name": name,
            "outputs": outputs,
            "files_images": r.files(),
            "files_videos": r.files("videos"),
            "first_file": r.first_file(),
        }
        if name in DIVERGENT:
            row["divergent"] = DIVERGENT[name]
        out.append(row)
    return out


def validation_cases() -> list[dict]:
    cases: list[tuple[str, str, dict | None]] = []

    cases.append(("没有 node_errors 就原样返回", "服务端拒了这个工作流", None))
    cases.append(("空的 node_errors 也原样返回", "服务端拒了", {}))

    # 最常见的一类：模型文件在那台机器上有，在这台没有
    cases.append(("模型文件服务端上没有", "校验失败", {
        "4": {"class_type": "CheckpointLoaderSimple",
              "errors": [{"type": "value_not_in_list",
                          "message": "Value not in list: ckpt_name",
                          "extra_info": {"input_name": "ckpt_name",
                                         "received_value": "某个.safetensors"}}]},
    }))

    cases.append(("别的类型的错原样带出来", "校验失败", {
        "7": {"class_type": "KSampler",
              "errors": [{"type": "value_bigger_than_max",
                          "message": "steps 太大了",
                          "extra_info": {"input_name": "steps"}}]},
    }))

    cases.append(("好几个节点好几条错", "校验失败", {
        "4": {"class_type": "CheckpointLoaderSimple",
              "errors": [{"message": "Value not in list: ckpt_name",
                          "extra_info": {"input_name": "ckpt_name",
                                         "received_value": "a.safetensors"}}]},
        "11": {"class_type": "LoraLoader",
               "errors": [{"message": "Value not in list: lora_name",
                           "extra_info": {"input_name": "lora_name",
                                          "received_value": "b.safetensors"}},
                          {"message": "strength 超范围"}]},
    }))

    cases.append(("节点没写 class_type", "校验失败", {
        "9": {"errors": [{"message": "随便什么错"}]},
    }))

    # errors 是空的 → lines 空 → 退回原消息
    cases.append(("errors 是空数组", "校验失败", {
        "9": {"class_type": "KSampler", "errors": []},
    }))

    cases.append(("整个节点没有 errors 键", "校验失败", {
        "9": {"class_type": "KSampler"},
    }))

    # extra_info 缺字段：not in list 那一支照样要能拼出话来
    cases.append(("not in list 但 extra_info 是空的", "校验失败", {
        "4": {"class_type": "CheckpointLoaderSimple",
              "errors": [{"message": "Value not in list: ckpt_name"}]},
    }))

    out = []
    for name, message, node_errors in cases:
        e = PromptValidationError(message, node_errors)
        out.append({
            "name": name,
            "message": message,
            "node_errors": node_errors,
            "human_summary": e.human_summary(),
        })
    return out


def history_cases() -> list[dict]:
    cases: list[tuple[str, dict]] = []

    cases.append(("正常的 execution_error", {
        "messages": [
            ["execution_start", {"prompt_id": "p1"}],
            ["execution_error", {"node_type": "KSampler",
                                 "exception_message": "CUDA out of memory"}],
        ]}))

    cases.append(("没有 messages", {}))
    cases.append(("messages 是空的", {"messages": []}))
    cases.append(("有 messages 但没有 execution_error", {
        "messages": [["execution_start", {"prompt_id": "p1"}],
                     ["execution_cached", {"nodes": ["1", "2"]}]]}))

    cases.append(("好几条错，取第一条", {
        "messages": [
            ["execution_error", {"node_type": "KSampler",
                                 "exception_message": "第一个"}],
            ["execution_error", {"node_type": "VAEDecode",
                                 "exception_message": "第二个"}],
        ]}))

    cases.append(("消息只有一个元素", {
        "messages": [["execution_error"]]}))

    cases.append(("消息不是数组", {
        "messages": [{"type": "execution_error"}]}))

    # ⚠️ 缺字段这一条两边不一样，见文件末尾的说明
    cases.append(("execution_error 里缺 node_type", {
        "messages": [["execution_error",
                      {"exception_message": "炸了"}]]}))

    DIVERGENT = {
        "execution_error 里缺 node_type":
            "Python 是 f\"节点 {detail.get('node_type')} ...\"，缺字段时 "
            "None 被 f-string 直接渲染进去，用户看到「节点 None 执行失败」"
            "——一个中文句子里夹着 Python 的 None。"
            "C++ 用 str_or(detail, \"node_type\", \"?\") 渲染成「节点 ?」。"
            "两边都没信息，但 ? 不会让人以为有个叫 None 的节点。",
    }

    out = []
    for name, status in cases:
        row = {
            "name": name,
            "status": status,
            "error": ComfyClient._error_from_history(status),
        }
        if name in DIVERGENT:
            row["divergent"] = DIVERGENT[name]
        out.append(row)
    return out


def progress_cases() -> list[dict]:
    from changji.comfy.client import JobProgress
    out = []
    for step, total in ((0, 0), (0, 20), (5, 20), (20, 20), (25, 20), (3, 0)):
        p = JobProgress(prompt_id="p1", step=step, total=total)
        out.append({"step": step, "total": total,
                    "fraction": round(p.fraction, 10)})
    return out


def main() -> int:
    payload = {
        "outputs": outputs_cases(),
        "validation": validation_cases(),
        "history": history_cases(),
        "progress": progress_cases(),
        "note": ("由 cpp/tests/export_comfy_client_golden.py 生成，不要手改。"
                 "outputs 那组专挑插入顺序和字典序不一样的节点 id——"
                 "C++ 用 ordered_json 是有意的，换成 json 就会挑错文件。"),
    }
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")

    n = sum(len(v) for k, v in payload.items() if isinstance(v, list))
    print(f"写入 {DEST}，{n} 条")
    for c in payload["outputs"]:
        ff = c["first_file"]
        if isinstance(ff, dict):
            shown = ff.get("filename", "?")
        elif ff is None:
            shown = "（没有）"
        else:
            shown = f"{ff!r}（不是字典！）"
        mark = "  ← 两边有意不一样" if c.get("divergent") else ""
        print(f"  {c['name']:32} first_file={shown}{mark}")
    div = [c["name"] for c in payload["outputs"] if c.get("divergent")]
    if div:
        print(f"其中 {len(div)} 条两边有意不一样：{'、'.join(div)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
