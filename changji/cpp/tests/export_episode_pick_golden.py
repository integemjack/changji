r"""导出各阶段**到底挑哪些镜头跑**，和 Python 逐个比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_episode_pick_golden.py

产出 cpp/tests/golden/episode_pick.json。

`test_episode.cpp` 是 `contract_audit.py` 工作单上最后一条
（「两边都有、又只钉了意图」）。这一份补上。

**为什么挑错镜头是最难查的一类错。** 每个阶段都从「哪些状态算这一阶段的
入口」里挑镜头。挑漏了，那一镜**永远轮不到它**——不报错、不重试、
日志里连一行都没有，表现是"成片里少了一个镜头"，而你会先怀疑分镜、
怀疑渲染、怀疑装配，最后才想到是入口状态少写了一个。
挑多了则是白烧显卡：已经出好的镜头又渲一遍。

入口状态一共这么几处，每一处少一个都是上面那种病：

  - 配音：`PLANNED`
  - 首帧：`AUDIO_DONE`
  - 草稿档渲染：`FRAME_DONE` + `AUDIO_DONE` + `DRAFT_REJECTED`
    （**`AUDIO_DONE` 那个是关键**——首帧失败的镜头状态停在这里，
    不收的话它就永远出不了片；收了就退回纯文生视频，
    画面一致性差一些，但整集不会卡住）
  - 成片档渲染：`DRAFT_DONE` + `FINAL_REJECTED`
  - 装配：`FINAL_DONE`/`DRAFT_DONE`/`FALLBACK`/`LOCKED`，**且要有 video_path**

**期望值不是我写的。** 这里把 `changji.pipeline` 里那几个阶段类换成
只做记录的假货，然后跑**真的** `Pipeline.run_audio` / `run_render` /
`assemble`——那几句挑镜头的列表推导是原封不动跑的，我只是把结果接出来。
自己照着源码抄一遍 predicate 的话，抄歪了语料和代码会一起歪。

矩阵是**全枚举**：每一个 `ShotStatus` × 每一个阶段 × force 开关。
少枚举一个状态，就正好可能是漏掉的那个。
"""
from __future__ import annotations

import asyncio
import io
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji import pipeline as P                              # noqa: E402
from changji.config import Settings                            # noqa: E402
from changji.hardware import Tier                              # noqa: E402
from changji.models.character import AssetLibrary              # noqa: E402
from changji.models.project import (                           # noqa: E402
    Episode, Project, ProjectStore,
)
from changji.models.shot import Shot, ShotStatus               # noqa: E402

DEST = REPO / "cpp" / "tests" / "golden" / "episode_pick.json"

ALL_STATUSES = list(ShotStatus)


class Recorded(Exception):
    """记下 todo 就不往下跑了——后面要真的显卡。"""

    def __init__(self, shot_ids: list[str]) -> None:
        super().__init__("recorded")
        self.shot_ids = shot_ids


def shots_one_per_status(with_video: bool = True) -> list[Shot]:
    """每个状态造一个镜头。order 故意和数组顺序反着来。"""
    out = []
    n = len(ALL_STATUSES)
    for i, st in enumerate(ALL_STATUSES):
        s = Shot(
            shot_id=f"sh_{st.value}", scene_id="sc01",
            order=n - i,                      # 倒着编，验排序
            visual_desc="雨夜天台", first_frame_prompt="雨夜天台",
            motion_prompt="推近", duration_s=4.0, status=st,
        )
        if with_video:
            s.video_path = f"video/{st.value}.mp4"
        out.append(s)
    return out


def make_project(store: ProjectStore, with_video: bool = True) -> Project:
    ep = Episode(episode_id="ep01", title="第一集",
                 shots=shots_one_per_status(with_video))
    return Project(project_id="p01", title="测试", episodes=[ep])


def pipe(store: ProjectStore) -> P.Pipeline:
    return P.Pipeline(store=store, settings=Settings(),
                      comfy_client=None, video_workflow=None)


async def pick_audio(store: ProjectStore, force: bool) -> list[str]:
    class Rec:
        def __init__(self, *a, **k) -> None:
            pass

        async def run(self, shots, *a, **k):
            raise Recorded([s.shot_id for s in shots])

    old_stage, old_backend = P.AudioStage, P.build_backend
    P.AudioStage = Rec
    P.build_backend = lambda *a, **k: type("B", (), {"name": "estimate"})()
    try:
        proj = make_project(store)
        try:
            await pipe(store).run_audio(proj, proj.episodes[0],
                                        AssetLibrary(), force=force)
        except Recorded as r:
            return r.shot_ids
        return []          # todo 空，阶段直接返回了
    finally:
        P.AudioStage, P.build_backend = old_stage, old_backend


async def pick_frames(store: ProjectStore, force: bool) -> list[str]:
    class Rec:
        def __init__(self, *a, **k) -> None:
            pass

        async def run(self, shots, *a, **k):
            raise Recorded([s.shot_id for s in shots])

    old = P.FrameStage
    P.FrameStage = Rec
    try:
        proj = make_project(store)
        try:
            await pipe(store).run_frames(proj, proj.episodes[0],
                                         AssetLibrary(), force=force)
        except Recorded as r:
            return r.shot_ids
        return []
    finally:
        P.FrameStage = old


async def pick_render(store: ProjectStore, tier: Tier, force: bool) -> list[str]:
    """渲染那一段挑完 todo 之后是逐镜调 `_render_one`，没有一处能拿到整个列表。

    所以在 `_render_one` 上截：它每镜被调一次，按调用顺序攒起来就是 todo，
    连顺序一起验了。
    """
    seen: list[str] = []

    async def rec(self, project, shot, *a, **k):
        seen.append(shot.shot_id)
        return None

    old = P.Pipeline._render_one
    P.Pipeline._render_one = rec
    try:
        proj = make_project(store)
        await pipe(store).run_render(proj, proj.episodes[0], AssetLibrary(),
                                     tier, force=force)
    finally:
        P.Pipeline._render_one = old
    return seen


async def collect(root: Path) -> dict:
    store = ProjectStore(root)
    out: dict = {"statuses": [s.value for s in ALL_STATUSES], "stages": []}

    for force in (False, True):
        out["stages"].append({
            "stage": "audio", "force": force,
            "picked": await pick_audio(store, force)})
        out["stages"].append({
            "stage": "frames", "force": force,
            "picked": await pick_frames(store, force)})
        for tier in (Tier.DRAFT, Tier.FINAL):
            out["stages"].append({
                "stage": f"render_{tier.value}", "force": force,
                "picked": await pick_render(store, tier, force)})
    return out


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="changji_挑镜头_") as tmp:
        payload = asyncio.run(collect(Path(tmp)))

    # **硬闸。** force=True 的意思就是"无视状态全部重跑"，所以那几行
    # 必须把每个状态都挑上。挑不满只有一种解释：截断点没接上，导出来的是
    # 空壳子而不是 Python 的真答案。
    # 上一版正是这样导出了四行「0 个」还一声不吭——语料写成那样，
    # C++ 那边照着比只会一起绿，把"根本没验"伪装成"验过了"。
    n = len(ALL_STATUSES)
    for row in payload["stages"]:
        if row["force"] and len(row["picked"]) != n:
            raise SystemExit(
                f"导出中止：{row['stage']} force=True 只挑到 "
                f"{len(row['picked'])} 个，应该是全部 {n} 个。\n"
                f"这不是 Python 的行为，是这个脚本没在对的地方截住。"
                f"先把截断点修好，别把空壳子写进语料。")
        if not row["force"] and not row["picked"]:
            raise SystemExit(
                f"导出中止：{row['stage']} force=False 一个都没挑到。"
                f"每个阶段至少该有它自己的入口状态命中——同上，先查截断点。")
    payload["note"] = (
        "由 cpp/tests/export_episode_pick_golden.py 生成，不要手改。"
        "期望值来自把阶段类换成假货、跑真的 Pipeline 方法接出来的 todo，"
        "不是照着源码抄的 predicate。")
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    print(f"写入 {DEST}")
    for row in payload["stages"]:
        ids = [i.replace("sh_", "") for i in row["picked"]]
        print(f"  {row['stage']:14} force={str(row['force']):5} "
              f"{len(ids):2} 个：{'、'.join(ids) if ids else '（一个都没挑）'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
