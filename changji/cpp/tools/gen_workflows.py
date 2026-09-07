"""把内置工作流嵌进二进制。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tools/gen_workflows.py

产出：
    cpp/src/comfy/bundled_workflows.inc.hpp

**为什么要嵌进去而不是随程序装几个 json 文件。** "单一二进制、零运行时依赖"
是这个后端的立项理由之一。带着几个数据文件走，就会出现"exe 拷过去了、
workflows 目录忘了拷"这种事，而表现是运行到出片那一步才报"找不到工作流"——
前面几十分钟的配音和首帧已经跑完了。

项目自己的 workflows/<name>.json 仍然优先，覆盖内置的那份。
不同的剧用不同的模型靠的就是这个。
"""

from __future__ import annotations

import io
import json
from pathlib import Path

HERE = Path(__file__).resolve()
CPP = HERE.parent.parent
ROOT = CPP.parent
SRC = ROOT / "src" / "changji" / "workflows"
OUT = CPP / "src" / "comfy" / "bundled_workflows.inc.hpp"

DELIM = "CJWF"


def lit(s: str) -> str:
    assert ")" + DELIM + '"' not in s, "分隔符撞车了，换一个"
    return 'R"' + DELIM + "(" + s + ")" + DELIM + '"'


def main() -> int:
    files = sorted(p for p in SRC.glob("*.json"))
    if not files:
        print(f"{SRC} 下一个工作流都没有")
        return 1

    parts = [
        "// 由 cpp/tools/gen_workflows.py 生成，别手改。",
        "//",
        "// 源文件：changji/src/changji/workflows/*.json",
        "// 改了那边之后重跑生成器。",
        "",
        "// clang-format off",
        "",
    ]
    names = []
    for f in files:
        # 原样嵌入，**不重新序列化**：重新序列化会改 key 顺序和数字格式，
        # 而界面版工作流的 widgets_values 是按位置对参数的，
        # 顺序变了就是另一份工作流。
        text = f.read_text(encoding="utf-8")
        json.loads(text)   # 只验合法性，不改内容
        stem = f.stem
        names.append(stem)
        parts.append(f"// ---- {f.name}（{len(text)} 字节）----")
        parts.append(f"inline constexpr const char* kBundled_{stem} =")
        parts.append(lit(text) + ";")
        parts.append("")

    parts.append("/// 内置工作流的名字和内容。名字不带扩展名。")
    parts.append("inline const std::vector<std::pair<const char*, const char*>>&")
    parts.append("bundled_workflows() {")
    parts.append("    static const std::vector<std::pair<const char*, const char*>> v = {")
    for n in names:
        parts.append(f'        {{"{n}", kBundled_{n}}},')
    parts.append("    };")
    parts.append("    return v;")
    parts.append("}")
    parts.append("")
    parts.append("// clang-format on")
    parts.append("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(parts))
    total = sum(len(f.read_text(encoding="utf-8")) for f in files)
    print(f"嵌了 {len(files)} 个工作流，共 {total} 字节 -> {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
