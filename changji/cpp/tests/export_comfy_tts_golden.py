"""导出 ComfyUI 配音后端**提交给服务端的那份工作流**。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_comfy_tts_golden.py

产出 cpp/tests/golden/comfy/tts_submit.json。

ComfyUI 那三条路（视频 / 首帧 / 配音）里，前两条已经有这种对比了
（`render_submit.json`、`frame_submit.json`），这一份补上第三条。
视频那条比出来过一个真 bug，首帧那条比出来一处没记录的偏差。

配音这条的填法和另外两条不一样：**不按节点类型找，按键名找**。
台词、音色、情绪各有一串候选键名，逐个节点扫过去，
遇到同名且**值是字符串**的就换掉（连线是 `["3", 0]`，不能碰）。
这套匹配规则错一点就是"台词没填进去"或者"填到了不该填的节点上"，
而两者在产出上都是"配出来的音不对"，从日志里看不出区别。

**用的是真的内置工作流**（`src/changji/workflows/tts.json`）。
它**本来就是接口版**，和 `video.json` 不一样——那份是界面版，
要拿 `object_info` 转。第一版我照着 video 那条路写了转换，
`convert()` 直接回了一句"这已经是接口版"。
"""
from __future__ import annotations

import asyncio
import io
import json
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.comfy.workflow import ApiWorkflow                       # noqa: E402
from changji.stages.audio import ComfyTTSBackend, _apply_text       # noqa: E402

CONVERT = REPO / "cpp" / "tests" / "golden" / "comfy" / "workflow_convert.json"
BUNDLED = REPO / "src" / "changji" / "workflows" / "tts.json"
DEST = REPO / "cpp" / "tests" / "golden" / "comfy" / "tts_submit.json"


class FakeResult:
    def files(self, kind: str = "images"):
        if kind == "audio":
            return [{"filename": "out.flac", "subfolder": "audio",
                     "type": "output"}]
        return []


class FakeClient:
    def __init__(self) -> None:
        self.submitted: list[dict] = []

    async def run(self, wf, on_progress=None):
        self.submitted.append(json.loads(json.dumps(wf.prompt)))
        return FakeResult()

    async def download(self, ref, dest):
        # 造一段够长的静音 wav，免得撞上"疑似空音频"那道闸
        import struct
        import wave
        dest = Path(dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(dest), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(24000)
            w.writeframes(struct.pack("<h", 0) * 24000 * 4)  # 4 秒
        return dest


class FakePaths:
    def __init__(self, root: Path) -> None:
        self.root = root

    def abs(self, rel) -> Path:
        return self.root / str(rel)


def bundled_api_workflow() -> dict:
    """内置的 tts.json。

    **它本来就是接口版**，和 video.json 不一样（那份是界面版，
    要拿 object_info 转）。第一版我照着 video 那条路写了转换，
    convert() 直接告诉我"这已经是接口版"。
    """
    return json.loads(io.open(BUNDLED, encoding="utf-8").read())


async def one(root: Path, api: dict, name: str, text: str,
              voice: str | None, emotion: str, out_name: str) -> dict:
    client = FakeClient()
    backend = ComfyTTSBackend(client, ApiWorkflow(json.loads(json.dumps(api))),
                              FakePaths(root))
    entry: dict = {"name": name, "text": text, "voice_id": voice,
                   "emotion": emotion, "out_stem": Path(out_name).stem,
                   # 喂进去的那份工作流也记下来，C++ 侧要用同一份
                   "workflow_used": api}
    try:
        await backend.synthesize(text, root / out_name, voice_id=voice,
                                 emotion=emotion)
        entry["ok"] = True
        entry["submitted"] = client.submitted[0]
    except Exception as exc:                                   # noqa: BLE001
        entry["ok"] = False
        entry["error_type"] = type(exc).__name__
    return entry


async def collect(root: Path) -> dict:
    api = bundled_api_workflow()
    cases = [
        await one(root, api, "普通一句", "你终于来了。", None, "neutral", "a.wav"),
        await one(root, api, "带音色", "你终于来了。", "旁白_女声.wav",
                  "neutral", "b.wav"),
        await one(root, api, "带情绪", "你终于来了。", None, "冷淡", "c.wav"),
        await one(root, api, "音色加情绪", "雨下了一整夜。", "voice_a.wav",
                  "低沉", "d.wav"),
    ]

    # 填不进去要抛。**这条最要紧**：填不进去而不报错的话，
    # 整集会拿工作流里预置的那句话去配音，而且每一镜都一样。
    empty = {"1": {"class_type": "KSampler",
                   "inputs": {"seed": 1, "steps": 20}}}
    cases.append(await one(root, empty, "工作流里没有可填文本的节点",
                           "你终于来了。", None, "neutral", "e.wav"))

    # 键名匹配那套规则单独导一份：不经过后端，直接调 _apply_text，
    # 这样能把"连线不许碰""neutral 不填"这些规则钉死。
    rules = []
    for rname, node_inputs, voice, emotion in [
        ("字符串的 text 要换掉", {"text": "预置的话"}, None, "neutral"),
        ("连线的 text 不许碰", {"text": ["3", 0]}, None, "neutral"),
        ("数字的 text 不许碰", {"text": 42}, None, "neutral"),
        ("第一个匹配的键胜出", {"prompt": "旧", "text": "旧"}, None, "neutral"),
        ("音色填进 speaker", {"text": "x", "speaker": "旧"}, "新音色", "neutral"),
        ("neutral 不填情绪", {"text": "x", "emotion": "旧"}, None, "neutral"),
        ("非 neutral 才填", {"text": "x", "emotion": "旧"}, None, "冷淡"),
        ("空音色不填", {"text": "x", "speaker": "旧"}, None, "neutral"),
    ]:
        wf = ApiWorkflow({"1": {"class_type": "AnyNode",
                                "inputs": json.loads(json.dumps(node_inputs))}})
        filled = _apply_text(wf, "新台词", voice, emotion)
        rules.append({"name": rname, "inputs_before": node_inputs,
                      "voice_id": voice, "emotion": emotion,
                      "filled": filled,
                      "inputs_after": wf.prompt["1"]["inputs"]})

    return {"cases": cases, "apply_rules": rules}


def main() -> int:
    root = Path(tempfile.mkdtemp(prefix="changji_配音语料_"))
    try:
        payload = asyncio.run(collect(root))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    payload["note"] = (
        "由 cpp/tests/export_comfy_tts_golden.py 生成，不要手改。"
        "submitted 是 ComfyTTSBackend.synthesize 真正提交的工作流；"
        "apply_rules 是键名匹配那套规则，直接调 _apply_text 得到。"
    )
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    ok = sum(1 for c in payload["cases"] if c.get("ok"))
    print(f"写入 {DEST}，{len(payload['cases'])} 条提交（{ok} 成功）、"
          f"{len(payload['apply_rules'])} 条填法规则")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
