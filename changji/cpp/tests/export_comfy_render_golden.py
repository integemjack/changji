"""导出 ComfyUI 视频后端**提交给服务端的那份工作流**。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_comfy_render_golden.py

产出 cpp/tests/golden/comfy/render_submit.json。

**为什么需要这一份。** `test_comfy_renderers.cpp` 里那八条用例是
2026-09-08 补的，但它们钉的是**我们自己的意图**，不是"和 Python 一样"。
装配层刚刚证明过这个差别不是学究：那边的用例把一个截过位的响度值当成
正确答案钉住了，绿了很久。

而这一层出过一次真的：两个视频后端都写死了 `StyleLine::REALISTIC`，
动画线的项目正向提示词和 Python 差一个分隔符。那次是肉眼看出来的。

输入用的是**真的内置工作流**（`src/changji/workflows/video.json`
经 `WorkflowConverter` 转成接口版），不是手搓的小工作流——
`set_by_class` / `find_by_class` 在真实结构上才有意义。
转换用的 `object_info` 取自 `golden/comfy/workflow_convert.json`。

期望值由**调真的 `RenderStage.render_video`** 得到：假一个 client
把提交上来的工作流记下来。
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
from changji.models.character import (                             # noqa: E402
    AppearanceBlock, AssetLibrary, Character, StyleLine,
)
from changji.models.shot import (                                  # noqa: E402
    CameraAngle, CameraMove, CharacterInShot, FacePose, Shot, ShotSize,
)
from changji.stages.render import PromptComposer, RenderStage       # noqa: E402

CONVERT = REPO / "cpp" / "tests" / "golden" / "comfy" / "workflow_convert.json"
DEST = REPO / "cpp" / "tests" / "golden" / "comfy" / "render_submit.json"


class FakeResult:
    def first_file(self):
        return {"filename": "out.mp4", "subfolder": "video", "type": "output"}

    def files(self, kind: str = "images"):
        return [self.first_file()]


class FakeClient:
    """只记不跑。提交上来的工作流原样留下。"""

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
        Path(dest).write_bytes(b"fake")


class FakePaths:
    """RenderStage 只用到 abs() 和产出目录。"""

    def __init__(self, root: Path) -> None:
        self.root = root

    def abs(self, rel) -> Path:
        return self.root / str(rel)

    def shots(self, tier: str) -> Path:
        d = self.root / "shots" / tier
        d.mkdir(parents=True, exist_ok=True)
        return d

    def rel(self, p) -> str:
        return str(Path(p).relative_to(self.root)).replace("\\", "/")


def assets(line: StyleLine) -> AssetLibrary:
    a = AssetLibrary()
    lin = Character(
        char_id="c_lin_wan", name="林晚",
        appearance=AppearanceBlock(
            identity="二十七岁女性，外表冷静",
            body="偏瘦，中等身高",
            face="黑色长直发，单眼皮",
            attire="白色衬衫",
        ),
    )
    a.characters["c_lin_wan"] = lin
    a.style.style_line = line
    a.style.global_style = "电影感，冷色调"
    a.style.negative_prompt = "低质量，多余的手指"
    a.style.aspect_ratio = "9:16"
    return a


def shot() -> Shot:
    return Shot(
        shot_id="ep01_sh007", scene_id="sc01", order=6,
        visual_desc="雨夜天台，两人对峙",
        first_frame_prompt="雨夜天台，两人对峙",
        motion_prompt="镜头缓慢推近",
        shot_size=ShotSize.MS, camera_angle=CameraAngle.EYE_LEVEL,
        camera_move=CameraMove.PUSH_IN,
        characters=[CharacterInShot(char_id="c_lin_wan", expression="落寞",
                                    action="转身", face_pose=FacePose.FRONT,
                                    wardrobe_state="default")],
        duration_s=5.0, attempts=0,
    )


def bundled_api_workflow() -> dict:
    """内置的 video.json 转成接口版之后的样子。"""
    d = json.loads(io.open(CONVERT, encoding="utf-8").read())
    for case in d["cases"]:
        if case["name"] == "bundled_video":
            return case["expected"]
    raise SystemExit("语料里没有 bundled_video 这一条")


SEED_SENTINEL = "<SEED>"


def blank_seed(prompt: dict) -> dict:
    """把 KSampler 的 seed 换成占位符。

    **种子这一项两边不可能一样，而且 Python 自己每次重启都不一样。**
    `render.py` 的 `_seed_for` 用的是 `abs(hash(shot.shot_id))`，
    而 Python 的 str `hash()` 按进程随机化（PYTHONHASHSEED 默认随机）——
    实测同一个 shot_id 三次不同进程给出 762199586 / 740115691 / 1194400939。

    这是一处**有意不复刻的 Python bug**，方案里「首帧种子每次重启都变」
    那一节记着（那节写的是 frames.py，render.py 这个是同一回事）。
    C++ 用 SHA-1 前 4 字节，跨进程跨机器都一样。

    不换成占位符的话这份语料每导一次就变一次，永远比不过。
    C++ 侧对自己的种子另有断言：值等于 render_seed(shot_id, attempts)，
    且跟着 attempts 变。
    """
    out = json.loads(json.dumps(prompt))
    for node in out.values():
        if isinstance(node, dict) and node.get("class_type") == "KSampler":
            node["inputs"]["seed"] = SEED_SENTINEL
    return out


def norm(text: str, root: Path) -> str:
    """临时目录每台机器都不一样，换成字面量 <ROOT>，反斜杠换正斜杠。"""
    out = text.replace(str(root), "<ROOT>")
    out = out.replace(str(root).replace("\\", "/"), "<ROOT>")
    return out.replace("\\", "/")


async def collect(root: Path) -> dict:
    api = bundled_api_workflow()
    out: dict = {"workflow": api, "cases": []}

    # 两条风格线各跑一遍。**动画线那条是重点**：分隔符不一样，
    # 而两个后端原来都写死了写实线。
    for line in (StyleLine.REALISTIC, StyleLine.ANIME):
        for tier, start in ((Tier.DRAFT, None), (Tier.FINAL, "frames/a.png")):
            client = FakeClient()
            composer = PromptComposer(assets(line))
            stage = RenderStage(client, composer, FakePaths(root),
                                ApiWorkflow(json.loads(json.dumps(api))))
            spec = TierSpec(tier=tier, width=480, height=854, steps=4)
            plan = stage.plan(shot(), spec)
            await stage.render_video(shot(), plan, start_image=start)

            out["cases"].append({
                "name": f"{line.value}_{tier.value}"
                        + ("_带首帧" if start else "_无首帧"),
                "style_line": line.value,
                "tier": tier.value,
                "start_image": start,
                "spec": {"width": spec.width, "height": spec.height,
                         "steps": spec.steps},
                "plan": {"frames": plan.frames,
                         "width": plan.spec.width, "height": plan.spec.height,
                         "steps": plan.spec.steps,
                         "positive": plan.prompts.positive,
                         "negative": plan.prompts.negative,
                         "motion": plan.motion},
                "uploaded": [norm(u, root) for u in client.uploaded],
                "submitted": blank_seed(client.submitted[0]),
            })
    return out


def main() -> int:
    root = Path(tempfile.mkdtemp(prefix="changji_渲染语料_"))
    try:
        payload = asyncio.run(collect(root))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    payload["note"] = (
        "由 cpp/tests/export_comfy_render_golden.py 生成，不要手改。"
        "submitted 是 RenderStage.render_video 真正提交给服务端的那份工作流。"
    )
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    print(f"写入 {DEST}，{len(payload['cases'])} 条")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
