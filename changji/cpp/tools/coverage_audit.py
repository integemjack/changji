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
    """头文件里**外面调得到**的名字。

    ⚠️ **要跳过私有成员。** 第一版没跳，于是它指着 `AudioStage` 的
    `split_long_lines` / `lock_duration` 说"没测过"——那几个是 private，
    外面根本调不到，测不了也不该测。一个会指向死路的清单比没有清单更糟：
    照着它去做，做到一半才发现这活儿干不了。

    识别办法很土：数花括号深度，记住每个 class/struct 是在哪一层开的，
    class 默认 private、struct 默认 public，见到 `public:` 之类就改。
    够用了——这个文件读的是自家的头文件，不是任意 C++。
    """
    text = io.open(header, encoding="utf-8", errors="replace").read()
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)

    out = set()
    depth = 0
    # 栈里放 (开始深度, 当前可见性)
    scopes: list[tuple[int, str]] = []

    for line in text.split("\n"):
        stripped = line.strip()

        # 可见性切换
        m = re.match(r"^(public|private|protected)\s*:", stripped)
        if m and scopes:
            scopes[-1] = (scopes[-1][0], m.group(1))

        # 新的 class / struct
        m = re.match(r"^(class|struct)\s+([A-Za-z_]\w*)", stripped)
        if m and not stripped.rstrip().endswith(";"):   # 前向声明不算
            if not scopes or scopes[-1][1] == "public":
                out.add(m.group(2))
            scopes.append((depth, "private" if m.group(1) == "class" else "public"))
        elif m:
            if not scopes or scopes[-1][1] == "public":
                out.add(m.group(2))

        visible = (not scopes) or scopes[-1][1] == "public"
        if visible:
            fm = re.match(
                r"^(?:[A-Za-z_][\w:<>,&*\s]*?\s+)?([A-Za-z_]\w*)\s*\(", stripped)
            if fm:
                name = fm.group(1)
                if name not in NOISE and not name.startswith("_"):
                    out.add(name)

        depth += line.count("{") - line.count("}")
        while scopes and depth <= scopes[-1][0]:
            scopes.pop()

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
