"""导出画面闸门的判定，和 Python 逐条比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_gates_golden.py

产出 cpp/tests/golden/gates.json。

**为什么要这一份。** `contract_audit.py` 把 `test_gates.cpp` 归在
「两边都有、又没有语料兜着」那一类。闸门的判定决定一镜是重试、
退回上一阶段、还是降级——**两边判得不一样，就是同一段素材在两个后端
上得到不同的质量结论**，而那正是这套东西存在的意义。

零件是对得上的：`PixelStats` 的阈值两边一模一样
（`spread < 8.0`、`mean < 6.0 || mean > 249.0`）。要比的是**把这些零件
组装成一个判定**的那一步——今天已经在别处栽过三次这个形状了
（装配层的响度、视频后端的风格线、首帧的尺寸节点名单）。

探测和取样都是注入的，所以这份语料是确定性的：喂同样的
probe 结果和 signalstats 样本，两边应该给出同样的 verdict / reasons /
metrics。
"""
from __future__ import annotations

import asyncio
import io
import json
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.assembly.ffmpeg import MediaInfo, PixelStats          # noqa: E402
from changji.config import GateConfig                              # noqa: E402
from changji.gates.checks import gate_video                        # noqa: E402
from changji.models.shot import Shot                               # noqa: E402

DEST = REPO / "cpp" / "tests" / "golden" / "gates.json"


class FakeFF:
    """喂固定的探测结果和取样结果。"""

    def __init__(self, info: MediaInfo | None, samples, probe_raises=False,
                 stats_raises=False):
        self._info = info
        self._samples = samples
        self._probe_raises = probe_raises
        self._stats_raises = stats_raises

    async def probe(self, path):
        if self._probe_raises:
            from changji.assembly.ffmpeg import FFmpegError
            raise FFmpegError("探测失败")
        return self._info

    async def sample_pixel_stats(self, path, samples=3):
        if self._stats_raises:
            from changji.assembly.ffmpeg import FFmpegError
            raise FFmpegError("取样失败")
        return self._samples


def stats(mean, spread):
    return PixelStats(mean=mean, spread=spread, minimum=0.0, maximum=255.0)


def info(duration=4.0, w=480, h=854, has_video=True):
    return MediaInfo(path=Path("x.mp4"), duration_s=duration, width=w, height=h,
                     fps=24.0, has_video=has_video, has_audio=False)


def shot():
    return Shot(shot_id="ep01_sh001", scene_id="sc01", order=0,
                visual_desc="雨夜天台", first_frame_prompt="雨夜天台",
                motion_prompt="推近", duration_s=4.0)


async def one(root: Path, name, *, file_bytes=4096, ff=None,
              expected_duration=None, expected_size=None,
              cfg=None) -> dict:
    """跑一条，**输入和结果都记下来**。

    输入必须进语料：C++ 那边假的是 Runner（回 ffmpeg 的文本），
    层次和这里不一样，得按同一组数把文本造出来。
    靠用例名去对应输入的话，这边改个数字那边就悄悄不一样了。
    """
    p = root / f"{name}.mp4"
    if file_bytes is None:
        pass                       # 不建文件：测"文件不存在"
    else:
        p.write_bytes(b"x" * file_bytes)

    config = cfg or GateConfig()
    r = await gate_video(shot(), p, ff, config,
                         expected_duration_s=expected_duration,
                         expected_size=expected_size)
    probe = ff._info
    return {
        "name": name,
        "file_bytes": file_bytes,
        # ---- 输入 ----
        "probe": None if probe is None else {
            "duration_s": probe.duration_s, "width": probe.width,
            "height": probe.height, "has_video": probe.has_video,
        },
        "probe_raises": ff._probe_raises,
        "stats_raises": ff._stats_raises,
        "samples": [{"mean": x.mean, "spread": x.spread}
                    for x in (ff._samples or [])],
        "expected_duration_s": expected_duration,
        "expected_size": list(expected_size) if expected_size else None,
        "verdict": r.verdict.value,
        "reasons": list(r.reasons),
        "metrics": {k: v for k, v in sorted((r.metrics or {}).items())},
    }


async def collect(root: Path) -> dict:
    good = [stats(120.0, 45.0), stats(122.0, 44.0), stats(118.0, 46.0)]
    cases = [
        await one(root, "正常一镜", ff=FakeFF(info(), good),
                  expected_duration=4.0, expected_size=(480, 854)),
        await one(root, "文件不存在", file_bytes=None, ff=FakeFF(info(), good)),
        await one(root, "文件几乎是空的", file_bytes=100,
                  ff=FakeFF(info(), good)),
        await one(root, "探测失败", ff=FakeFF(None, good, probe_raises=True)),
        await one(root, "没有视频轨",
                  ff=FakeFF(info(has_video=False), good)),
        await one(root, "时长对不上", ff=FakeFF(info(duration=9.0), good),
                  expected_duration=4.0),
        await one(root, "分辨率对不上", ff=FakeFF(info(w=640, h=360), good),
                  expected_size=(480, 854)),
        await one(root, "取样失败", ff=FakeFF(info(), [], stats_raises=True)),
        await one(root, "取不到画面", ff=FakeFF(info(), [])),
        await one(root, "纯色画面",
                  ff=FakeFF(info(), [stats(120.0, 2.0), stats(120.0, 2.1),
                                     stats(120.0, 1.9)])),
        await one(root, "全黑",
                  ff=FakeFF(info(), [stats(3.0, 40.0), stats(3.1, 41.0),
                                     stats(2.9, 39.0)])),
        await one(root, "过曝",
                  ff=FakeFF(info(), [stats(252.0, 40.0), stats(251.0, 41.0),
                                     stats(253.0, 39.0)])),
        await one(root, "细节偏少但不是纯色",
                  ff=FakeFF(info(), [stats(120.0, 9.0), stats(121.0, 9.5),
                                     stats(119.0, 9.2)])),
        await one(root, "亮度中途跳变",
                  ff=FakeFF(info(), [stats(30.0, 40.0), stats(200.0, 41.0),
                                     stats(35.0, 39.0)])),
    ]
    return {"cases": cases}


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="changji_闸门语料_") as tmp:
        payload = asyncio.run(collect(Path(tmp)))
    payload["note"] = (
        "由 cpp/tests/export_gates_golden.py 生成，不要手改。"
        "探测和取样都是注入的固定值，所以判定是确定性的。"
    )
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    print(f"写入 {DEST}，{len(payload['cases'])} 条")
    for c in payload["cases"]:
        print(f"  {c['name']:22} {c['verdict']:9} {c['reasons']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
