r"""导出配音阶段跑完之后**镜头变成什么样**，和 Python 比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_audio_stage_golden.py

产出 cpp/tests/golden/audio_stage.json。

`audio_plan.json` 已经覆盖了**算**的那部分（时长估算、拆镜头）。
这一份补的是**编排**那部分（`stages/audio.cpp` / `AudioStage.run`）：
一句一句配、把量到的时长写回台词、反推锁定镜头时长、推状态。

要紧的是**时长回写**：`actual_duration_s` 是后面所有环节的输入——
字幕的时间戳、装配的 adelay 偏移、闸门的音画同步判定，全从它来。
写错了不会报错，成片里表现为字幕和人声对不上，而那时候已经隔了三个阶段。

⚠️ **一处两边不一样，导的时候要绕开**：Python 的 `run` 有个
`concurrency` 参数（默认 2），C++ 是顺序跑的（头文件里写了理由：
配音服务通常就一张卡，并发只会让每句更慢；而且 `shot.dialogue` 是就地改的，
两条线程改同一个镜头结果是乱的）。
这里用 `concurrency=1` 导，否则比的就不是同一件事。

⚠️ **第二处不一样：一镜配音失败之后会怎样。** 这份语料**不覆盖失败**，
因为两边在失败上根本不是同一个形状，喂进去比不出东西来：

  - Python 的 run 是 `asyncio.gather(...)`，**没带 return_exceptions**。
    一镜抛错整个阶段就断了，异常穿到 pipeline，
    那句 `for shot in todo: shot.status = AUDIO_DONE` 压根不会执行。
    实测（三镜、第二镜抛错）：抛 RuntimeError，第三镜没跑，
    **三镜全停在 planned**——连已经配好的第一镜也没推进，
    而它的 `actual_duration_s` 已经写进去了。整集的配音就这么废了。
  - C++ 逐镜 try/catch，失败那镜停在 PLANNED，其余照推 AUDIO_DONE，
    整条继续跑。

C++ 这边是**有意做得更稳**（和首帧阶段那处差异同一个形状）。
差异写进方案，不在这里假装两边一样。
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

from changji.config import TTSConfig                            # noqa: E402
from changji.models.character import AssetLibrary               # noqa: E402
from changji.models.shot import (                               # noqa: E402
    DialogueLine, Shot, ShotStatus,
)
from changji.stages.audio import AudioStage, SynthesisResult    # noqa: E402

DEST = REPO / "cpp" / "tests" / "golden" / "audio_stage.json"


class ScriptedTTS:
    """每句回一个固定时长。按调用顺序取。"""

    name = "scripted"

    def __init__(self, durations: list[float]) -> None:
        self.durations = durations
        self.i = 0

    async def synthesize(self, text, out_path, voice_id=None,
                         emotion="neutral", intensity=0.5):
        d = self.durations[self.i] if self.i < len(self.durations) else 1.0
        self.i += 1
        p = Path(out_path)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"wav")
        return SynthesisResult(duration_s=d, audio_path=p)

    async def available(self) -> bool:
        return True


class FakePaths:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.audio = root / "audio"

    def rel(self, p) -> str:
        return str(Path(p).relative_to(self.root)).replace("\\", "/")


def shot(sid: str, lines: list[str], duration: float = 5.0) -> Shot:
    return Shot(
        shot_id=sid, scene_id="sc01", order=0,
        visual_desc="雨夜天台", first_frame_prompt="雨夜天台",
        motion_prompt="推近", duration_s=duration,
        status=ShotStatus.PLANNED,
        dialogue=[DialogueLine(text=t, char_id=None) for t in lines])


def snap(s: Shot) -> dict:
    return {
        "shot_id": s.shot_id,
        "status": s.status.value,
        "duration_s": round(s.duration_s, 4),
        "duration_locked": s.duration_locked,
        "dialogue": [{"text": l.text,
                      "actual_duration_s": (None if l.actual_duration_s is None
                                            else round(l.actual_duration_s, 4)),
                      "has_audio": bool(l.audio_path)}
                     for l in s.dialogue],
    }


async def one(root: Path, name: str, shots: list[Shot],
              durations: list[float]) -> dict:
    # **输入要先记下来。** shots_after 里的台词是**拆过之后**的，
    # 拿它当输入重建就不是同一个场景了——拆句本身正是被测的行为。
    # （闸门那份语料上犯过同样的错，这里不重犯。）
    before = [{"shot_id": s.shot_id, "duration_s": round(s.duration_s, 4),
               "dialogue": [l.text for l in s.dialogue]} for s in shots]
    stage = AudioStage(ScriptedTTS(durations), TTSConfig(), FakePaths(root))
    plans = await stage.run(shots, AssetLibrary(), concurrency=1)
    # **状态推进两边在不同的层。** Python 的 AudioStage.run 根本不碰 status，
    # 是 pipeline.py:211 在 run 返回之后统一推的：
    #     for shot in todo: shot.status = ShotStatus.AUDIO_DONE
    # C++ 是在 audio.cpp:356 阶段内部逐镜推的。
    # 只比 run 的话 Python 这边全是 planned，比的就不是同一个边界了——
    # 下游（出首帧）读的是**合起来之后**的状态。所以这里把 pipeline 那一句
    # 补上，让语料落在真正有意义的边界上。
    for sh in shots:
        sh.status = ShotStatus.AUDIO_DONE
    return {
        "name": name,
        "synth_durations": durations,
        "shots_before": before,
        "plans": [{"shot_id": p.shot_id,
                   "speech_duration_s": round(p.speech_duration_s, 4),
                   "locked_duration_s": round(p.locked_duration_s, 4),
                   "slack_s": round(p.slack_s, 4),
                   "lines": p.lines,
                   "is_tight": p.is_tight} for p in plans],
        "shots_after": [snap(s) for s in shots],
    }


async def collect(root: Path) -> dict:
    return {"cases": [
        await one(root, "一镜一句", [shot("sh001", ["你终于来了。"])], [2.0]),
        await one(root, "一镜两句", [shot("sh002", ["你终于来了。", "雨下了一夜。"])],
                  [2.0, 1.5]),
        await one(root, "没有台词", [shot("sh003", [])], []),
        await one(root, "两镜各一句",
                  [shot("sh004", ["第一句。"]), shot("sh005", ["第二句。"])],
                  [1.2, 2.4]),
        # **这一条走的是重新拆句那条路，不是"简单超时"。**
        # 一句长台词先被 _split_long_lines 拆成两句，配出来还是太长，
        # 于是再拆一轮（C++ 那边叫 kMaxResplits）。
        # 第一版我把它命名成"语音超出原定时长"，看数字才发现拆了两轮——
        # **名字和它实际走的路不一样，是给下一个人挖坑**。
        await one(root, "长台词触发重新拆句",
                  [shot("sh006", ["很长的一句台词，念起来要好一会儿。"], duration=2.0)],
                  [6.0]),
        # 真正的"语音比镜头长"：短台词但配出来很长，不触发拆句
        await one(root, "语音比镜头长，直接拉长镜头",
                  [shot("sh007", ["等等。"], duration=2.0)], [5.0]),
    ]}


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="changji_配音阶段_") as tmp:
        payload = asyncio.run(collect(Path(tmp)))
    payload["note"] = (
        "由 cpp/tests/export_audio_stage_golden.py 生成，不要手改。"
        "concurrency=1 导——Python 的 run 默认并发 2，C++ 是顺序跑的。")
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    print(f"写入 {DEST}，{len(payload['cases'])} 条")
    for c in payload["cases"]:
        for p in c["plans"]:
            print(f"  {c['name']:16} {p['shot_id']} 语音{p['speech_duration_s']}s "
                  f"锁定{p['locked_duration_s']}s 留白{p['slack_s']}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
