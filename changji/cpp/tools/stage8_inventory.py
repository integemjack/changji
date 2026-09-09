r"""阶段 8 的单向门有多重：数一遍删掉 Python 引擎会一起没掉什么。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tools/stage8_inventory.py

**为什么要有这个工具。** 方案里那张表是 2026-09-08 手数的："16 个脚本 import
引擎、3 个不依赖"。2026-09-09 再数是 31 和 7——快翻倍了，因为这期间每补一份
语料就多一个生成器。而那张表正是判断"这扇门该不该推"的依据：门后有多少东西
一起消失，直接决定删之前要补多少。

手数的数字会过期而且不会有人发现。这个脚本每次都从真实文件算，
和 `regen_golden.py` / `contract_audit.py` 是同一个思路：**别信文档里写的**。

判据是 `import changji` / `from changji`——那是"要一份能跑的 Python 引擎"
的充分条件。删了引擎，这些脚本连 import 都过不去。
"""
from __future__ import annotations

import io
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
CPP = HERE.parents[1]
REPO = CPP.parent

# 引擎自己在这儿。删的就是它。
ENGINE = REPO / "src" / "changji"

IMPORTS = re.compile(r"^\s*(?:from\s+changji[\s.]|import\s+changji\b)", re.M)

# 哪些目录里的脚本算数。cpp/tests 和 cpp/tools 是 C++ 这边的，
# 仓库根的 tools/ 是 Python 那边自己的周边。
SCAN = [CPP / "tests", CPP / "tools", REPO / "tools"]


def main() -> int:
    if not ENGINE.is_dir():
        print(f"找不到引擎目录 {ENGINE}——是不是已经删了？")
        return 1

    engine_files = sorted(ENGINE.rglob("*.py"))
    engine_bytes = sum(f.stat().st_size for f in engine_files)

    depends: list[Path] = []
    standalone: list[Path] = []
    for root in SCAN:
        if not root.is_dir():
            continue
        for f in sorted(root.rglob("*.py")):
            text = io.open(f, encoding="utf-8", errors="replace").read()
            (depends if IMPORTS.search(text) else standalone).append(f)

    def rel(p: Path) -> str:
        return p.relative_to(REPO).as_posix()

    print("引擎本体")
    print(f"  {len(engine_files)} 个 .py，{engine_bytes / 1024 / 1024:.1f} MB")
    print()
    print(f"**删了就废掉的脚本：{len(depends)} 个**")
    print("（它们 import changji——删了引擎连 import 都过不去）")
    by_dir: dict[str, list[str]] = {}
    for f in depends:
        by_dir.setdefault(rel(f.parent), []).append(f.name)
    for d in sorted(by_dir):
        print(f"  {d}/  {len(by_dir[d])} 个")
        for n in sorted(by_dir[d]):
            print(f"      {n}")
    print()
    print(f"删了照样能跑的：{len(standalone)} 个")
    for f in standalone:
        print(f"  {rel(f)}")
    print()

    # 能活下来的安全网
    golden = sorted((CPP / "tests" / "golden").rglob("*.json"))
    print(f"能活下来的安全网：tests/golden/ 里 {len(golden)} 份 JSON")
    print("  它们进了版本库，单元测试直接读文件、不碰 Python。")
    print("  所以删完之后「C++ 有没有改坏」仍然测得出来，")
    print("  测不出来的是「**Python 现在还是不是这样**」。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
