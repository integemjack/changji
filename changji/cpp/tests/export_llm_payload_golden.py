"""导出发给大模型服务的**请求体**，和 Python 逐字段比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_llm_payload_golden.py

产出 cpp/tests/golden/llm_payload.json。

**为什么这一份算契约。** 请求体是真的发到外部服务上的东西。
`temperature` 差一点、`response_format` 少一层、`strict` 没带上，
模型给回来的就是另一种东西——而两边都会"成功"，
差异要到成片里才看得出来。

`test_llm_client.cpp` 里那条「请求体的形状」钉的是我们自己的意图，
没有任何东西去问 Python。`contract_audit.py` 把它归在
「两边都有、又没有语料兜着」那一类。

一处结构差异值得先说清：**Python 是三个阶段各拼各的**
（`bible.py` / `script.py` / `storyboard.py` 里各有一份 `_complete`），
**C++ 是一个 `build_payload` 三处共用**。所以这份语料按阶段导，
C++ 那边用同样的输入调那一个函数，看能不能拼出同样的三份。
共用的那份要是漏了某个阶段的特殊处理，就是在这里露出来。

两种模式都导：正常的 `json_schema`，和服务不支持时退回的 `json_object`
（两边都有这条退路，`bible.py:147`、`script.py:517`、`storyboard.py:296`）。
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.stages.bible import _BIBLE_SCHEMA                # noqa: E402

DEST = REPO / "cpp" / "tests" / "golden" / "llm_payload.json"

MODEL = "qwen3:14b"
TEMPERATURE = 0.35
PROMPT = "把这段剧本里的角色和场景抽出来。\n\n雨夜天台，两人对峙。"


def payload(schema_name: str, schema, *, json_schema_mode: bool) -> dict:
    """照 Python 三个 _complete 的写法拼。

    三处的写法逐字一样，只有 name 和 schema 不同——所以这里用一份代码
    代表它们，而**差异由 C++ 那边逐阶段比出来**。
    """
    p = {
        "model": MODEL,
        "messages": [{"role": "user", "content": PROMPT}],
        "temperature": TEMPERATURE,
        "response_format": {
            "type": "json_schema",
            "json_schema": {"name": schema_name, "strict": True,
                            "schema": schema},
        },
    }
    if not json_schema_mode:
        # 退回那一支：整个 response_format 换掉，不是往里加东西
        p["response_format"] = {"type": "json_object"}
    return p


def main() -> int:
    storyboard = json.loads(io.open(
        REPO / "cpp" / "tests" / "golden" / "stage_storyboard.json",
        encoding="utf-8").read())
    script = json.loads(io.open(
        REPO / "cpp" / "tests" / "golden" / "stage_script.json",
        encoding="utf-8").read())

    stages = [
        ("bible", _BIBLE_SCHEMA),
        ("script", script["script_schema"]),
        # **名字是复数**。script.py 的 generate_premises 传的是
        # name="premises"，C++ 那边 scripting.cpp:202 也是 "premises"。
        # 单复数写错了服务端不会报错，但 strict 模式下 schema 名对不上
        # 有些实现会拒。
        ("premises", script["premise_schema"]),
        ("storyboard", storyboard["schemas"][0] if isinstance(
            storyboard["schemas"], list) else storyboard["schemas"]),
    ]

    cases = []
    for name, schema in stages:
        for mode in (True, False):
            cases.append({
                "name": f"{name}_{'schema' if mode else '退回 json_object'}",
                "schema_name": name,
                "json_schema_mode": mode,
                "schema": schema,
                "payload": payload(name, schema, json_schema_mode=mode),
            })

    # 没给 schema 时不该带 response_format——带一个空的会被有些服务拒掉
    cases.append({
        "name": "没有 schema",
        "schema_name": "",
        "json_schema_mode": True,
        "schema": None,
        "payload": {
            "model": MODEL,
            "messages": [{"role": "user", "content": PROMPT}],
            "temperature": TEMPERATURE,
        },
    })

    out = {
        "model": MODEL,
        "temperature": TEMPERATURE,
        "prompt": PROMPT,
        "cases": cases,
        "note": ("由 cpp/tests/export_llm_payload_golden.py 生成，不要手改。"
                 "Python 是三个阶段各拼各的，C++ 是一个 build_payload 共用；"
                 "这份按阶段导，就是要看那一个函数能不能拼出同样的三份。"),
    }
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(out, ensure_ascii=False, indent=2) + "\n")
    print(f"写入 {DEST}，{len(cases)} 条")
    for c in cases:
        rf = c["payload"].get("response_format")
        print(f"  {c['name']:28} response_format="
              f"{rf['type'] if rf else '（没有）'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
