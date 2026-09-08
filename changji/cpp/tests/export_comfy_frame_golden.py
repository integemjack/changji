"""导出 ComfyUI 首帧后端**提交给服务端的那份工作流**。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_comfy_frame_golden.py

产出 cpp/tests/golden/comfy/frame_submit.json。

视频那条路已经有 `render_submit.json` 了，这一份补上首帧那条。
两条是同一个类别的代码，而**视频那条比出来过一个真 bug**
（两个后端都写死了 REALISTIC），首帧这条只有 C++ 自己的意图测试。

⚠️ **工作流是造的，不是内置的。** 仓库里只带了 `tts.json` 和 `video.json`；
图像工作流是用户自己放在 `workflows/image.json` 的，没有可用的真样本。
所以这里造一个结构典型的：两个 CLIPTextEncode 靠连线区分、
一个 EmptyLatentImage、两个 LoadImage、一个 SaveImage。
比的是**同样的输入两边改出同样的东西**，造的工作流不影响这个判断。

另外单造一份**只有 ModelSamplingSD3、没有潜空间节点**的，
专门照两边"尺寸节点候选名单"的差别——那两份名单现在是不一样的，
而没有任何东西记着为什么。
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

from changji.comfy.workflow import ApiWorkflow                     # noqa: E402
from changji.hardware import Tier, TierSpec                        # noqa: E402
from changji.models.character import StyleLine                     # noqa: E402
from changji.models.shot import Shot                               # noqa: E402
from changji.stages.frames import ImageModelFrameBackend           # noqa: E402
from changji.stages.render import PromptBundle                     # noqa: E402

DEST = REPO / "cpp" / "tests" / "golden" / "comfy" / "frame_submit.json"

SEED_SENTINEL = "<SEED>"


def blank_seed(prompt: dict) -> dict:
    """种子换占位符。

    和 render_submit.json 同一个理由：`_seed_for` 用 `abs(hash(...))`，
    而 Python 的 str hash() 按进程随机化，导一次变一次。
    这是方案里「首帧种子每次重启都变」记着的那处有意不复刻的 Python bug。
    C++ 侧对自己的种子另有断言。
    """
    out = json.loads(json.dumps(prompt))
    for node in out.values():
        if isinstance(node, dict) and node.get("class_type") == "KSampler":
            node["inputs"]["seed"] = SEED_SENTINEL
    return out


class FakeResult:
    def first_file(self):
        return {"filename": "out.png", "subfolder": "", "type": "output"}


class FakeClient:
    def __init__(self) -> None:
        self.submitted: list[dict] = []
        self.uploaded: list[str] = []

    async def run(self, wf, on_progress=None):
        self.submitted.append(json.loads(json.dumps(wf.prompt)))
        return FakeResult()

    async def upload_image(self, path):
        self.uploaded.append(str(path))
        return f"up_{len(self.uploaded)}.png"

    async def download(self, ref, dest):
        Path(dest).parent.mkdir(parents=True, exist_ok=True)
        Path(dest).write_bytes(b"png")
        return Path(dest)


class FakePaths:
    def __init__(self, root: Path) -> None:
        self.root = root

    def abs(self, rel) -> Path:
        return self.root / str(rel)


def image_workflow() -> dict:
    """结构典型的图像工作流。正的是 3、负的是 4，靠连线区分。"""
    return {
        "3": {"class_type": "CLIPTextEncode", "inputs": {"text": ""}},
        "4": {"class_type": "CLIPTextEncode", "inputs": {"text": ""}},
        "5": {"class_type": "EmptyLatentImage",
              "inputs": {"width": 512, "height": 512, "batch_size": 1}},
        "6": {"class_type": "LoadImage", "inputs": {"image": "a.png"}},
        "7": {"class_type": "LoadImage", "inputs": {"image": "b.png"}},
        "8": {"class_type": "SaveImage",
              "inputs": {"filename_prefix": "ComfyUI"}},
        "9": {"class_type": "KSampler", "inputs": {
            "positive": ["3", 0], "negative": ["4", 0],
            "seed": 0, "steps": 20}},
    }


def no_latent_workflow() -> dict:
    """没有潜空间节点，只有 ModelSamplingSD3。

    **专门照两边尺寸节点名单的差别。** Python 那份名单里有
    ModelSamplingSD3，C++ 那份没有（C++ 有 Wan22ImageToVideoLatent
    和 EmptyHunyuanLatentVideo，Python 没有）。
    """
    wf = image_workflow()
    del wf["5"]
    wf["10"] = {"class_type": "ModelSamplingSD3",
                "inputs": {"model": ["1", 0], "shift": 8}}
    return wf


def shot(shot_id: str = "ep01_sh007", attempts: int = 0) -> Shot:
    return Shot(shot_id=shot_id, scene_id="sc01", order=6,
                visual_desc="雨夜天台", first_frame_prompt="雨夜天台，两人对峙",
                motion_prompt="镜头缓慢推近", duration_s=5.0, attempts=attempts)


def bundle(refs: list[str]) -> PromptBundle:
    return PromptBundle(positive="雨夜天台，两人对峙，电影感",
                        negative="低质量，多余的手指",
                        reference_images=refs)


def norm(text: str, root: Path) -> str:
    out = text.replace(str(root), "<ROOT>")
    out = out.replace(str(root).replace("\\", "/"), "<ROOT>")
    return out.replace("\\", "/")


async def one(root: Path, name: str, wf: dict, refs: list[str],
              make_files: list[str]) -> dict:
    for rel in make_files:
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"png")

    client = FakeClient()
    backend = ImageModelFrameBackend(client, ApiWorkflow(json.loads(json.dumps(wf))),
                                     FakePaths(root))
    spec = TierSpec(tier=Tier.DRAFT, width=480, height=854, steps=4)
    entry: dict = {"name": name, "workflow": wf,
                   "reference_images": refs,
                   "existing_files": make_files,
                   "spec": {"width": spec.width, "height": spec.height,
                            "steps": spec.steps},
                   "prompts": {"positive": bundle(refs).positive,
                               "negative": bundle(refs).negative}}
    try:
        await backend.generate(shot(), bundle(refs), spec, root / "out.png")
        entry["ok"] = True
        entry["submitted"] = blank_seed(client.submitted[0])
        entry["uploaded"] = [norm(u, root) for u in client.uploaded]
    except Exception as exc:                                   # noqa: BLE001
        entry["ok"] = False
        entry["error_type"] = type(exc).__name__
    return entry


async def collect(root: Path) -> dict:
    cases = [
        await one(root, "无参考图", image_workflow(), [], []),
        await one(root, "两张参考图", image_workflow(),
                  ["refs/a.png", "refs/b.png"], ["refs/a.png", "refs/b.png"]),
        await one(root, "参考图比 LoadImage 多", image_workflow(),
                  ["refs/a.png", "refs/b.png", "refs/c.png"],
                  ["refs/a.png", "refs/b.png", "refs/c.png"]),
        await one(root, "参考图文件不在", image_workflow(),
                  ["refs/没这个.png", "refs/b.png"], ["refs/b.png"]),
        await one(root, "没有潜空间节点只有 ModelSamplingSD3",
                  no_latent_workflow(), [], []),
    ]
    for c in cases:
        if c["name"] == "没有潜空间节点只有 ModelSamplingSD3":
            c["cpp_differs"] = (
                "Python 的尺寸节点名单里有 ModelSamplingSD3，于是它把 "
                "width/height 塞到一个根本没有这两个输入的节点上——"
                "看着成功了，实际什么也没设成。C++ 的名单里没有它，"
                "set_size 这一条返回假。两边都没真的调整尺寸，"
                "区别只在 Python 会往工作流里塞两个无效输入。"
                "C++ 不复刻这个。")

    # SaveImage 缺席：Python 吞掉 WorkflowError 照跑
    wf = image_workflow()
    del wf["8"]
    cases.append(await one(root, "没有 SaveImage", wf, [], []))
    return {"cases": cases}


def main() -> int:
    root = Path(tempfile.mkdtemp(prefix="changji_首帧语料_"))
    try:
        payload = asyncio.run(collect(root))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    payload["note"] = (
        "由 cpp/tests/export_comfy_frame_golden.py 生成，不要手改。"
        "submitted 是 ImageModelFrameBackend.generate 真正提交的工作流；"
        "KSampler 的 seed 换成了 <SEED>（Python 每次重启都不一样）。"
    )
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    ok = sum(1 for c in payload["cases"] if c.get("ok"))
    print(f"写入 {DEST}，{len(payload['cases'])} 条（{ok} 条成功）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
