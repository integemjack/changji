"""每个测试文件到底在验什么：**和 Python 比**，还是只钉自己的意图。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tools/contract_audit.py

**为什么要有这个。** 阶段 8 的闸门不是"对拍通过"，是
「对拍覆盖到你愿意永久放弃它的程度」。而"覆盖"有两种，价值差得很远：

  和 Python 比   期望值来自跑 Python，Python 改了或我们抄错了都会红
  只钉意图       期望值是人写在用例里的，**写错了照样绿**

这个差别不是学究。2026-09-08 一天之内撞到三次：

  - 装配层：用例把一个被 `%g` 截过位的响度值当成正确答案钉住了
  - 视频后端：两个后端都写死 REALISTIC，动画线提示词差一个分隔符
  - `test_jobs.cpp` 有条用例叫「Run 快照的字段和 Python 一致」，
    而那份字段名单是**手写在用例里的**，没有任何东西去问 Python

三处都是"绿着的错"。所以要能一眼看出哪些模块只有意图测试。

判据（故意保守，宁可少算不多算）：
  一个测试文件如果读了 `CHANGJI_GOLDEN_DIR` 下的某份语料，
  而那份语料由 `export_*.py` / `gen_*.py` 生成（那些脚本 import 引擎），
  就算「和 Python 比」。否则算「只钉意图」。

**它量的是"有没有和 Python 比过"，不是"比得全不全"。** 一个文件里
可能一半用例读语料、另一半是手写断言，这里只会记成"和 Python 比"。
真要看比得全不全，得看具体语料有多少条——那是人看的事。
"""

from __future__ import annotations

import io
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent      # cpp/
REPO = ROOT.parent                                          # changji/


def generated_goldens() -> dict[str, str]:
    """语料文件名 -> 生成它的脚本。

    认的是脚本里出现的 `xxx.json` 字面量。够用：这些脚本本来就只写
    自己那一两份。
    """
    out: dict[str, str] = {}
    for script in sorted(list((ROOT / "tests").glob("export_*.py")) +
                         list((ROOT / "tools").glob("gen_*.py"))):
        text = io.open(script, encoding="utf-8").read()
        # 两种写法都要认：
        #   一，直接写字面量 "xxx.json"
        #   二，**经过一个 helper**，比如 export_golden.py 里的
        #       `dump("tiers_for_vram", ...)`，文件名是
        #       `OUT / f"{name}.json"` 拼出来的。
        #
        # 只认第一种的话会**漏报**：`test_hardware.cpp` 明明在和 Python
        # 比（tiers_for_vram.json / tier_scaled_to.json 都是 export_golden.py
        # 生成的），却被归进"只钉意图"。而漏报的代价是有人照着这份清单
        # 去补一份已经存在的语料。
        for m in re.finditer(r'"([a-z_0-9]+\.json)"', text):
            out.setdefault(m.group(1), script.name)
        for m in re.finditer(r'dump\(\s*"([a-z_0-9]+)"', text):
            out.setdefault(m.group(1) + ".json", script.name)
    return out


def golden_refs(test_src: str) -> set[str]:
    """一个测试文件读了哪些语料。"""
    refs: set[str] = set()
    if "CHANGJI_GOLDEN_DIR" not in test_src:
        return refs
    # load_golden("x") / "/comfy/x.json" / "x.json" 三种写法都认
    for m in re.finditer(r'load_golden\("([^"]+)"\)', test_src):
        refs.add(m.group(1) + ".json")
    for m in re.finditer(r'"/?([a-z_0-9]+(?:/[a-z_0-9]+)*\.json)"', test_src):
        refs.add(m.group(1).split("/")[-1])
    return refs


def main() -> int:
    gen = generated_goldens()
    tests = sorted((ROOT / "tests" / "unit").glob("*.cpp")) + \
        sorted((ROOT / "tests" / "compat").glob("*.cpp"))

    compared: list[tuple[str, list[str]]] = []
    intent_only: list[str] = []

    for t in tests:
        if t.name == "main.cpp" and t.parent.name == "unit":
            continue
        src = io.open(t, encoding="utf-8").read()
        refs = sorted(r for r in golden_refs(src) if r in gen)
        if refs:
            compared.append((t.name, refs))
        else:
            intent_only.append(t.name)

    print("和 Python 比过的（期望值来自跑 Python）")
    print("-" * 78)
    for name, refs in compared:
        print(f"  {name:32} {', '.join(refs)}")

    print()
    print("只钉自己意图的（期望值手写在用例里，**写错了照样绿**）")
    print("-" * 78)
    for name in intent_only:
        print(f"  {name}")

    print()
    total = len(compared) + len(intent_only)
    print(f"合计 {total} 个测试文件：和 Python 比 {len(compared)}，"
          f"只钉意图 {len(intent_only)}")
    print()
    print("「只钉意图」不等于该改。很多东西 Python 侧根本没有")
    print("（WebSocket、进程内 sd.cpp、llama_tts、线程池），没有可比物；")
    print("**要警惕的是那些两边都有、却只钉了意图的**。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
