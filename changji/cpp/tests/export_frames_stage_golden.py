r"""导出首帧阶段跑完之后**镜头变成什么样**，和 Python 比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_frames_stage_golden.py

产出 cpp/tests/golden/frames_stage.json。

`test_frames.cpp` 在 `contract_audit.py` 的「两边都有、又没有语料兜着」
那一类里。这一层的逻辑不多，但**每一条都写在镜头状态上**，
而状态是后面每一个阶段的输入：

  - 成功：`status` 推到 `frame_done`，`frame_path` 填相对路径
  - 失败：**`attempts` 加一，`status` 不动**

第二条是要紧的。`attempts` 是闸门的重试计数，加错了要么永远重试、
要么第一次就判超限降级。而 `status` 不动意味着这一镜下一轮还会被捡起来
——改成推到别的状态，它就被跳过了，表现是"那一镜永远没有首帧"
而日志里只有一条早就滚掉的失败。

⚠️ **一处两边不一样，单独记**：Python 只捕 `(FrameError, RenderError)`，
**C++ 捕的是 `std::exception`**。也就是说渲染器抛别的类型时，
Python 让它穿出去（整个阶段中断），C++ 把它算成"这一镜失败"接着跑。
这份语料只喂两边都会捕的那类错误——喂别的就不是在比同一件事了。
差异写进方案。
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.hardware import Tier, TierSpec                    # noqa: E402
from changji.models.character import (                          # noqa: E402
    AppearanceBlock, AssetLibrary, Character, StyleLine,
)
from changji.models.shot import Shot, ShotStatus                # noqa: E402
from changji.stages.frames import FrameError, FrameStage        # noqa: E402
from changji.stages.render import PromptComposer                # noqa: E402

DEST = REPO / "cpp" / "tests" / "golden" / "frames_stage.json"


class ScriptedBackend:
    """按剧本成功或失败。第 i 个镜头看 plan[i]。"""

    name = "scripted"

    def __init__(self, plan: list[bool]) -> None:
        self.plan = plan
        self.i = 0

    async def generate(self, shot, prompts, spec, dest):
        ok = self.plan[self.i] if self.i < len(self.plan) else True
        self.i += 1
        if not ok:
            raise FrameError(f"{shot.shot_id} 出首帧失败：造出来的错")
        Path(dest).parent.mkdir(parents=True, exist_ok=True)
        Path(dest).write_bytes(b"png")
        return Path(dest)


class FakePaths:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.frames = root / "frames"

    def rel(self, p) -> str:
        return str(Path(p).relative_to(self.root)).replace("\\", "/")


def assets() -> AssetLibrary:
    a = AssetLibrary()
    a.characters["c_lin"] = Character(
        char_id="c_lin", name="林",
        appearance=AppearanceBlock(identity="二十七岁女性", body="偏瘦",
                                   face="黑色长直发", attire="白衬衫"))
    a.style.style_line = StyleLine.REALISTIC
    a.style.global_style = "电影感"
    a.style.negative_prompt = "低质量"
    a.style.aspect_ratio = "9:16"
    return a


def shots(n: int) -> list[Shot]:
    out = []
    for i in range(n):
        out.append(Shot(
            shot_id=f"ep01_sh{i + 1:03d}", scene_id="sc01", order=i,
            visual_desc="雨夜天台", first_frame_prompt="雨夜天台",
            motion_prompt="推近", duration_s=4.0,
            status=ShotStatus.AUDIO_DONE, attempts=0))
    return out


def snapshot(sh: Shot) -> dict:
    return {"shot_id": sh.shot_id, "status": sh.status.value,
            "attempts": sh.attempts, "frame_path": sh.frame_path}


async def one(root: Path, name: str, plan: list[bool]) -> dict:
    ss = shots(len(plan))
    stage = FrameStage(ScriptedBackend(plan), PromptComposer(assets()),
                       FakePaths(root))
    spec = TierSpec(tier=Tier.DRAFT, width=480, height=854, steps=4)
    outs = await stage.run(ss, spec)
    return {
        "name": name,
        "plan": plan,
        "outcomes": [{"shot_id": o.shot_id, "ok": o.ok,
                      "has_error": bool(o.error)} for o in outs],
        "shots_after": [snapshot(s) for s in ss],
    }


async def collect(root: Path) -> dict:
    return {"cases": [
        await one(root, "全成功", [True, True, True]),
        await one(root, "全失败", [False, False]),
        await one(root, "中间那个失败", [True, False, True]),
        await one(root, "第一个就失败", [False, True]),
        await one(root, "只有一镜且成功", [True]),
    ]}


def main() -> int:
    import asyncio
    import tempfile
    with tempfile.TemporaryDirectory(prefix="changji_首帧阶段_") as tmp:
        payload = asyncio.run(collect(Path(tmp)))
    payload["note"] = (
        "由 cpp/tests/export_frames_stage_golden.py 生成，不要手改。"
        "只喂两边都会捕的错误类型——Python 捕 (FrameError, RenderError)，"
        "C++ 捕 std::exception，喂别的就不是在比同一件事了。")
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    print(f"写入 {DEST}，{len(payload['cases'])} 条")
    for c in payload["cases"]:
        st = " ".join(f"{s['status']}/{s['attempts']}" for s in c["shots_after"])
        print(f"  {c['name']:16} {st}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
