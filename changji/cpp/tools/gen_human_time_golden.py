"""把 Python 的 human_time 的输出录下来，给 C++ 侧对照。

`changji/pipeline.py` 的 human_time 用的是 f-string 的 `:.0f`，那是
**IEEE 754 的就近取偶**（banker's rounding），不是小学的四舍五入：
0.5 → "0 秒"，1.5 → "2 秒"，2.5 → "2 秒"。

C++ 那边用 `snprintf("%.0f")`，行为一样。但这件事**只有在恰好落在 .5 上
才看得出来**，随便挑几个数测是测不到的——所以边界值要专门挑，
而期望值必须由**真的调一遍 Python** 得出，不能手写。

跑法（在 changji/ 下）：
    .venv/Scripts/python.exe cpp/tools/gen_human_time_golden.py
"""

from __future__ import annotations

import io
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "src"))

from changji.pipeline import human_time  # noqa: E402

# 挑的是三个分支的两侧，加上"两种舍入会分家"的那几个点。
CASES = [
    -5, -0.4, 0, 0.4, 0.5, 1.5, 2.5, 30, 59, 59.4, 59.5, 59.6, 60,
    89, 90, 90.5, 149, 150, 3599, 3600, 3629, 3630, 5400, 7200,
    86400, 359999,
]


def main() -> int:
    out = [{"seconds": c, "text": human_time(float(c))} for c in CASES]
    dst = ROOT / "cpp" / "tests" / "golden" / "human_time.json"
    io.open(dst, "w", encoding="utf-8", newline="").write(
        json.dumps({"cases": out}, ensure_ascii=False, indent=2) + "\n")
    print(f"{len(out)} 条写进 {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
