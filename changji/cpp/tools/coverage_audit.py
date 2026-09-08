"""看看每个头文件里导出的东西，有没有被测试碰过。

**这个脚本是被我自己坑出来的。** 我有两次说"这块没测"，都是 grep 的模式
写坏了：一次把 `+00:00` 里的 `+` 当成了正则量词，一次是模式里的字符类
没匹配上。而"没测"这个结论一旦错了，后面的活儿全建在沙子上——
我照着去补了一条已经存在的用例。

所以这里**一律用固定串匹配**（不走正则），并且比对的是**符号名**
而不是文件名——测试引不引某个头文件说明不了什么，调不调它导出的函数才算。

跑法（在 changji/cpp 下）：
    python tools/coverage_audit.py
    python tools/coverage_audit.py --verbose   # 连符号一起列
"""

from __future__ import annotations

import io
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
TESTS = [ROOT / "tests" / "unit", ROOT / "tests" / "compat"]

# 只认这几种声明：自由函数、类、结构体。宏和模板不算，噪音太大。
DECL = re.compile(
    r"^\s*(?:[A-Za-z_][\w:<>,&*\s]*?\s+)?"      # 返回类型（可能没有）
    r"([A-Za-z_]\w*)\s*\("                       # 函数名
    r"|^\s*(?:class|struct)\s+([A-Za-z_]\w*)",   # 或者类名
    re.M,
)

# 这些名字太常见，出现在测试里说明不了什么。
NOISE = {
    "if", "for", "while", "switch", "return", "sizeof", "static_cast",
    "operator", "catch", "throw", "and", "or", "not",
}


def symbols(header: pathlib.Path) -> set[str]:
    text = io.open(header, encoding="utf-8", errors="replace").read()
    # 去掉注释，免得把注释里提到的名字当成声明。
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    out = set()
    for m in DECL.finditer(text):
        name = m.group(1) or m.group(2)
        if name and name not in NOISE and not name.startswith("_"):
            out.add(name)
    return out


def main() -> int:
    verbose = "--verbose" in sys.argv
    blob = ""
    for d in TESTS:
        for f in d.rglob("*.cpp"):
            blob += io.open(f, encoding="utf-8", errors="replace").read()

    rows = []
    for hpp in sorted(SRC.rglob("*.hpp")):
        if hpp.name.endswith(".inc.hpp"):
            continue  # 生成出来的数据表，不是接口
        syms = symbols(hpp)
        if not syms:
            continue
        # **固定串匹配**，不走正则——这个脚本存在的理由就是这个。
        hit = {s for s in syms if s in blob}
        rows.append((hpp.relative_to(SRC).as_posix(), len(hit), len(syms),
                     sorted(syms - hit)))

    rows.sort(key=lambda r: (r[1] / r[2] if r[2] else 1, -r[2]))

    print(f"{'头文件':<34}{'碰过':>6}{'导出':>6}  没碰过的")
    print("-" * 100)
    for name, hit, total, missed in rows:
        mark = "  " if hit == total else ("!!" if hit == 0 else "  ")
        shown = "" if hit == total else "、".join(missed[:6])
        if not verbose and hit == total:
            continue
        print(f"{mark}{name:<32}{hit:>6}{total:>6}  {shown}")

    zero = [r for r in rows if r[1] == 0]
    part = [r for r in rows if 0 < r[1] < r[2]]
    full = [r for r in rows if r[1] == r[2]]
    print()
    print(f"一个都没碰过：{len(zero)}    碰过一部分：{len(part)}    全碰过：{len(full)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
