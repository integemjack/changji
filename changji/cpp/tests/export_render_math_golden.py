r"""导出 frames_for 的换算结果，和 Python 逐个比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_render_math_golden.py

产出 cpp/tests/golden/render_math.json。

**为什么单独给它一份语料。** 帧数错了 Wan 不会报错——4n+1 是它的硬要求，
给别的数它自己截，而截在哪儿不告诉你。表现是"出来的片比预期短一点"，
和"模型没跟上运动描述"混在一起，几乎不可能联想到是这个换算错了。

而这段换算里藏着一个**很容易抄歪的地方**：

    raw = int(round(duration_s * fps))
    n   = max(1, round((raw - 1) / 4))

`round()` 在 Python 里是**四舍六入五取偶**（banker's rounding），
不是学校教的四舍五入。`round(2.5)` 是 2 不是 3。
`(raw-1)/4` 正好落在 .5 上的情形一点都不少见——raw=11 就是 2.5。

C++ 那边用 `std::nearbyint`（默认舍入模式也是取偶）来对齐。
**这份语料就是钉这一条**：专挑落在 .5 上的时长，还有上限截断那一段。

`test_render.cpp` 里原来只有三个手写的期望值（2.0→49、3.0→73），
那是"我们以为应该是多少"，不是"Python 给多少"。
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.stages.render import MAX_FRAMES, frames_for      # noqa: E402

DEST = REPO / "cpp" / "tests" / "golden" / "render_math.json"


def main() -> int:
    durations = []

    # 常见时长
    durations += [0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0]

    # **专挑 (raw-1)/4 落在 .5 上的**——banker's rounding 和普通四舍五入
    # 在这里给出不同答案。raw = 24*d，要 (raw-1)/4 = x.5 即 raw = 4x+3。
    # 24d = 3, 7, 11, 15, 19, 23, 27 ... → d = raw/24
    for raw in (3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43):
        durations.append(round(raw / 24.0, 6))

    # duration*fps 自己落在 .5 上的（第一层 round 也走 banker's）
    for half in (0.5, 1.5, 2.5, 3.5):
        durations.append(round((half + 0.5) / 24.0 * 24 / 24, 6))
    durations += [x / 48.0 for x in (1, 3, 5, 7, 9, 11)]

    # 上限那一段：MAX_FRAMES = 121，24fps 下 5.0 秒就到顶
    durations += [5.0, 5.2, 6.0, 20.0, 100.0]

    # 边角
    durations += [0.0, 0.01, -1.0]

    seen = set()
    cases = []
    for d in durations:
        for fps in (24, 30):
            key = (d, fps)
            if key in seen:
                continue
            seen.add(key)
            cases.append({"duration_s": d, "fps": fps,
                          "frames": frames_for(d, fps)})

    out = {
        "max_frames": MAX_FRAMES,
        "cases": cases,
        "note": ("由 cpp/tests/export_render_math_golden.py 生成，不要手改。"
                 "专门覆盖 banker's rounding 的 .5 边界和上限截断。"),
    }
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(out, ensure_ascii=False, indent=2) + "\n")
    print(f"写入 {DEST}，{len(cases)} 条（MAX_FRAMES={MAX_FRAMES}）")
    # 把落在 .5 上的那几条单独打出来看看
    for raw in (3, 7, 11, 15):
        d = round(raw / 24.0, 6)
        print(f"  d={d:<10} fps=24 → raw={raw}  (raw-1)/4="
              f"{(raw - 1) / 4:<5} frames={frames_for(d, 24)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
