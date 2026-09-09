r"""导出「渲染→过闸门→重试/退回/降级」那个循环的每一条路，和 Python 逐个比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_render_loop_golden.py

产出 cpp/tests/golden/render_loop.json。

**这个循环以前在 C++ 里根本不存在。** `gate_video` 和 `decide_next` 都
移植了、也和 Python 一条不差地对过（golden/gates.json），但真二进制里
从来没人调它们——出完片直接置 DRAFT_DONE。也就是说：C++ 从不拦废片、
从不重试、从不降级，而 Python 默认（`[gates] enabled = true`）每一镜都过。
整套质量控制在 C++ 这边是空的。

现在接上了。接的是 `Pipeline._render_one` 那个循环，它决定：

  - 渲染抛异常 → attempts+1，没到上限就重来，到了就降级（FALLBACK）
  - 闸门没开 → 直接置完成
  - 闸门过了 → 置完成，清 gate_notes
  - 闸门没过 → 问 decide_next：RETRY 就 attempts+1 重来；FALLBACK 就降级；
    REGRESS 就标 *_REJECTED 交给人

每一条判错的后果都不一样：多重试一次是白烧显卡，少重试一次是一镜
本来能救回来却降级成静帧；把 REGRESS 判成 RETRY 是无限重跑同一个
必败的镜头。所以每一条路都要有语料。

**期望值不是我写的。** 这里把渲染器和 `gate_video` 换成按剧本出结果的
假货，然后跑**真的** `Pipeline._render_one`，`decide_next` 也是真的。
记下来的是它改完之后镜头长什么样（状态 / attempts / gate_notes /
video_path）、渲染被叫了几次、以及吐出来的事件序列。
C++ 那边用同一份剧本喂同一个循环，逐字段比。
"""
from __future__ import annotations

import asyncio
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji import pipeline as P                              # noqa: E402
from changji.config import Settings                            # noqa: E402
from changji.gates.checks import GateResult, Verdict           # noqa: E402
from changji.hardware import Tier, TierSpec                    # noqa: E402
from changji.models.project import (                           # noqa: E402
    Episode, Project, ProjectStore,
)
from changji.models.shot import Shot, ShotStatus               # noqa: E402

DEST = REPO / "cpp" / "tests" / "golden" / "render_loop.json"


# ---------------------------------------------------------------------------
# 剧本。每一条：渲染每次调用是成功还是抛错；闸门每次调用给什么判定。
# 列表用完了就一直用最后一个——循环真要多跑几轮的话不会因为剧本短了炸掉。
#
# 每个 case 都写清楚**为什么这条路值得单独钉**。
# ---------------------------------------------------------------------------
CASES = [
    {
        "name": "闸门没开：一次就完",
        "why": "enabled=false 时不该碰 gate_video，一次渲染直接置完成",
        "tier": "draft", "enabled": True, "fallback_on_exhausted": True,
        "gates_enabled": False,
        "render": ["ok"], "gate": [],
    },
    {
        "name": "闸门过了",
        "why": "最普通的一条路；gate_notes 要被清空，不能留上一轮的",
        "tier": "draft", "gates_enabled": True, "fallback_on_exhausted": True,
        "render": ["ok"], "gate": ["pass"],
        "preset_gate_notes": ["上一轮留下的"],
    },
    {
        "name": "成片档过了",
        "why": "状态要落到 FINAL_DONE 而不是 DRAFT_DONE；两个不能混",
        "tier": "final", "gates_enabled": True, "fallback_on_exhausted": True,
        "render": ["ok"], "gate": ["pass"],
    },
    {
        "name": "渲染连续失败到上限",
        "why": "attempts 每次+1，到 max 才降级；少一次是提前放弃，多一次是白烧显卡",
        "tier": "draft", "gates_enabled": True, "fallback_on_exhausted": True,
        "render": ["fail", "fail", "fail"], "gate": [],
    },
    {
        "name": "渲染失败一次然后过了",
        "why": "第二次成功后 attempts 停在 1，状态照样是完成",
        "tier": "draft", "gates_enabled": True, "fallback_on_exhausted": True,
        "render": ["fail", "ok"], "gate": ["pass"],
    },
    {
        "name": "闸门 RETRY 一次然后过了",
        "why": "RETRY 要 attempts+1 再渲一次（换种子），过了之后 gate_notes 清空",
        "tier": "draft", "gates_enabled": True, "fallback_on_exhausted": True,
        "render": ["ok", "ok"], "gate": ["retry", "pass"],
    },
    {
        "name": "闸门一直 RETRY 直到超限",
        "why": "decide_next 看的是 attempts+1>=max，所以是第 2 次没过就降级，不是第 3 次",
        "tier": "draft", "gates_enabled": True, "fallback_on_exhausted": True,
        "render": ["ok", "ok", "ok", "ok"], "gate": ["retry", "retry", "retry", "retry"],
    },
    {
        "name": "闸门 REGRESS",
        "why": "重跑没用，标 DRAFT_REJECTED 交给人；attempts 不动，不再渲染",
        "tier": "draft", "gates_enabled": True, "fallback_on_exhausted": True,
        "render": ["ok"], "gate": ["regress"],
    },
    {
        "name": "成片档 REGRESS",
        "why": "成片档退回的是 FINAL_REJECTED",
        "tier": "final", "gates_enabled": True, "fallback_on_exhausted": True,
        "render": ["ok"], "gate": ["regress"],
    },
    {
        "name": "超限但不许降级",
        "why": "fallback_on_exhausted=false 时超限走 REGRESS，标 *_REJECTED 而不是 FALLBACK",
        "tier": "draft", "gates_enabled": True, "fallback_on_exhausted": False,
        "render": ["ok", "ok", "ok"], "gate": ["retry", "retry", "retry"],
    },
    {
        "name": "带着 attempts 进来",
        "why": "断点续跑：镜头上已经有 attempts=2，再失败一次就该到上限",
        "tier": "draft", "gates_enabled": True, "fallback_on_exhausted": True,
        "render": ["fail", "fail"], "gate": [],
        "preset_attempts": 2,
    },
]


def make_shot(case: dict) -> Shot:
    s = Shot(
        shot_id="sh001", scene_id="sc01", order=0,
        visual_desc="雨夜天台", first_frame_prompt="雨夜天台",
        motion_prompt="推近", duration_s=3.0,
        status=ShotStatus.FRAME_DONE if case["tier"] == "draft" else ShotStatus.DRAFT_DONE,
    )
    s.attempts = int(case.get("preset_attempts", 0))
    s.gate_notes = list(case.get("preset_gate_notes", []))
    return s


class Script:
    """按剧本出结果的假渲染器 + 假闸门。同时数自己被叫了几次。"""

    def __init__(self, case: dict, store: ProjectStore) -> None:
        self.case = case
        self.store = store
        self.render_calls = 0
        self.gate_calls = 0

    def _pick(self, seq: list, i: int):
        if not seq:
            raise SystemExit(f"{self.case['name']}：剧本里没给这一步，循环跑到了没写的地方")
        return seq[min(i, len(seq) - 1)]

    # 假的 RenderStage：只要 plan() 和 render_video()
    def plan(self, shot, spec, aspect):
        class _Spec:
            width, height = 448, 256
        class _Plan:
            pass
        p = _Plan()
        p.spec = _Spec()
        return p

    async def render_video(self, shot, plan, start_image=None):
        i = self.render_calls
        self.render_calls += 1
        what = self._pick(self.case["render"], i)
        if what == "fail":
            raise RuntimeError(f"造出来的错 #{i + 1}")
        p = self.store.paths.shots(self.case["tier"]) / f"{shot.shot_id}.mp4"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"x" * 4096)
        return p

    async def gate_video(self, shot, video_path, ff, cfg, **kw):
        i = self.gate_calls
        self.gate_calls += 1
        what = self._pick(self.case["gate"], i)
        gate_name = kw.get("gate_name", "画面闸门")
        if what == "pass":
            return GateResult(shot.shot_id, Verdict.PASS, gate_name)
        if what == "retry":
            return GateResult(shot.shot_id, Verdict.RETRY, gate_name,
                              [f"造出来的理由 #{i + 1}", "第二条理由"])
        if what == "regress":
            return GateResult(shot.shot_id, Verdict.REGRESS, gate_name,
                              ["重跑也没用的那种"])
        raise SystemExit(f"剧本里不认识的闸门结果：{what}")


async def run_case(case: dict, root: Path) -> dict:
    store = ProjectStore(root / case["name"])
    settings = Settings()
    settings.gates.enabled = case["gates_enabled"]
    settings.gates.fallback_on_exhausted = case["fallback_on_exhausted"]

    events: list[dict] = []
    pipe = P.Pipeline(
        store=store, settings=settings, comfy_client=None, video_workflow=None,
        listener=lambda e: events.append({
            "kind": e.kind, "message": e.message, "shot_id": e.shot_id,
            "current": e.current, "total": e.total,
        }),
    )

    shot = make_shot(case)
    proj = Project(project_id="p01", title="测试",
                   episodes=[Episode(episode_id="ep01", title="一", shots=[shot])])
    tier = Tier.DRAFT if case["tier"] == "draft" else Tier.FINAL
    spec = TierSpec(tier=tier, width=448, height=256, steps=8)
    stage_name = P.Stage.DRAFT if tier is Tier.DRAFT else P.Stage.FINAL
    want_after = ShotStatus.DRAFT_DONE if tier is Tier.DRAFT else ShotStatus.FINAL_DONE

    script = Script(case, store)
    old_gate = P.gate_video
    P.gate_video = script.gate_video            # 模块里绑的那个名字
    try:
        result = await pipe._render_one(
            proj, shot, script, spec, "9:16", stage_name, want_after, 1, 1)
    finally:
        P.gate_video = old_gate

    return {
        "name": case["name"], "why": case["why"],
        "tier": case["tier"],
        "gates_enabled": case["gates_enabled"],
        "fallback_on_exhausted": case["fallback_on_exhausted"],
        "max_attempts_per_shot": settings.gates.max_attempts_per_shot,
        "preset_attempts": int(case.get("preset_attempts", 0)),
        "preset_gate_notes": list(case.get("preset_gate_notes", [])),
        "render": case["render"], "gate": case["gate"],
        # ---- 结果 ----
        "status": shot.status.value,
        "attempts": shot.attempts,
        "gate_notes": list(shot.gate_notes),
        "has_video_path": bool(shot.video_path),
        "render_calls": script.render_calls,
        "gate_calls": script.gate_calls,
        "returned_verdict": None if result is None else result.verdict.value,
        "events": events,
    }


async def collect(root: Path) -> dict:
    rows = [await run_case(c, root) for c in CASES]

    # **硬闸。** 每一条至少渲染过一次——一次都没有只有一种解释：
    # 假渲染器没接上，循环根本没跑，导出来的是一具空壳。
    for r in rows:
        if r["render_calls"] < 1:
            raise SystemExit(f"导出中止：{r['name']} 一次渲染都没记到，截断点没接上")
        # 闸门开着而且渲染成功过的，闸门就得被叫过
        if r["gates_enabled"] and "ok" in r["render"] and r["gate_calls"] < 1:
            raise SystemExit(f"导出中止：{r['name']} 闸门开着却一次没被叫，gate_video 没换上")

    return {"cases": rows}


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="changji_出片循环_") as tmp:
        payload = asyncio.run(collect(Path(tmp)))
    DEST.parent.mkdir(parents=True, exist_ok=True)
    DEST.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8")
    print(f"写了 {len(payload['cases'])} 条到 {DEST}")
    for r in payload["cases"]:
        kinds = ",".join(e["kind"] for e in r["events"])
        print(f"  {r['name']}: {r['status']} attempts={r['attempts']} "
              f"渲染{r['render_calls']}次 闸门{r['gate_calls']}次 [{kinds}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
