"""导出角色圣经阶段的对拍语料。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_bible_golden.py

产出 cpp/tests/golden/stage_bible.json。

和 export_golden.py 一样的规矩：**调真实的 Python 函数**，不复述它的逻辑。
在这里手写一份"期望输出"，只能证明我理解得和自己一致，证明不了
和 Python 一致——而后者才是对拍要回答的问题。
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]          # .../changji
sys.path.insert(0, str(REPO / "src"))

from changji.models.character import StyleLine                       # noqa: E402
from changji.stages.bible import (                                   # noqa: E402
    BibleError, BibleGenerator, _BIBLE_SCHEMA, _clean, _default_negative,
    _slug, build_prompt,
)
from changji.stages.storyboard import _extract_json                  # noqa: E402

SCRIPT = """第一集《雨夜天台》

林晚站在天台边缘，雨水打湿了她的白衬衫。
林晚：你说过会来的。
陈默从阴影里走出来，手里攥着一把伞。
陈默：我来了。晚了七年。
"""


def prompts() -> list[dict]:
    out = []
    for line in (StyleLine.REALISTIC, StyleLine.ANIME):
        out.append({
            "style_line": line.value,
            "script": SCRIPT,
            "prompt": build_prompt(SCRIPT, line),
        })
    # 空剧本也要能拼出提示词，不能因为空串就少一段
    out.append({
        "style_line": StyleLine.REALISTIC.value,
        "script": "",
        "prompt": build_prompt("", StyleLine.REALISTIC),
    })
    return out


def slugs() -> list[dict]:
    cases = [
        "lin_wan", "Lin Wan", "LIN-WAN", "  lin__wan  ", "office_night",
        "林晚",                       # 纯中文，走 sha1 退路
        "陈默 Chen Mo",               # 中英混排
        "",                          # 空串也走退路
        "___", "123", "a1_b2",
        "天台", "雨夜天台-A",
        "café",                      # 非 ASCII 拉丁字母
        "a好b",                      # 中文夹在中间
    ]
    return [{"input": c, "output": _slug(c)} for c in cases]


def cleans() -> list[dict]:
    cases = [
        "冷静克制。",
        "  多余   空白  ",
        "身姿笔挺，",
        "结尾一堆标点。，、；;,. ",
        "中间。不动。的句号。",
        "",
        "只有标点。。。",
        "换行\n和\t制表符",
    ]
    return [{"input": c, "output": _clean(c)} for c in cases]


def extracts() -> list[dict]:
    raws = [
        '{"a": 1}',
        '  {"a": 1}  ',
        '```json\n{"a": 1}\n```',
        '```\n{"a": 1}\n```',
        '这是我的回答：\n```json\n{"a": 1}\n```\n希望有帮助',
        '好的，结果是 {"a": 1} 就这样',
        '[1, 2, 3]',
        '前面一句话\n[1, 2, 3]',
        '```json\n[{"x": 1}]\n```',
        # 围栏里不是合法 JSON，要退回到括号扫描
        '```json\n不是 JSON\n```\n{"b": 2}',
        # 嵌套对象，括号扫描要数对深度
        '答：{"a": {"b": [1, 2]}, "c": 3} 完毕',
    ]
    out = []
    for r in raws:
        try:
            out.append({"raw": r, "ok": True, "value": _extract_json(r)})
        except Exception as exc:                      # noqa: BLE001
            out.append({"raw": r, "ok": False, "error_type": type(exc).__name__})
    # 一定失败的几个
    for r in ["", "完全没有 JSON", "{不平衡的括号"]:
        try:
            out.append({"raw": r, "ok": True, "value": _extract_json(r)})
        except Exception as exc:                      # noqa: BLE001
            out.append({"raw": r, "ok": False, "error_type": type(exc).__name__})
    return out


MODEL_OUTPUT = {
    "characters": [
        {"key": "lin_wan", "name": "林晚",
         "identity": "二十七岁女性，外表冷静，内里执拗。",
         "body": "偏瘦，中等身高",
         "face": "黑色长直发，鹅蛋脸，杏眼，眼尾微垂，薄唇。",
         "attire": "白色衬衫，深色西装裤"},
        {"key": "chen_mo", "name": "陈默",
         "identity": "三十出头男性，沉默寡言",
         "body": "高瘦",
         "face": "短寸黑发，方脸，浓眉，单眼皮",
         "attire": "黑色风衣"},
        # key 和 name 都是中文，走 slug 的 sha1 退路
        {"key": "", "name": "路人甲",
         "identity": "中年男性", "body": "", "face": "圆脸，秃顶", "attire": "工装"},
    ],
    "locations": [
        {"key": "rooftop_night", "name": "夜间天台",
         "space": "水泥地面，锈蚀的护栏，远处是楼群",
         "lighting": "夜间冷调顶光，霓虹反光。",
         "palette": "冷蓝为主，霓虹粉点缀"},
        {"key": "office", "name": "办公室",
         "space": "开放式工位", "lighting": "白天，日光灯", "palette": ""},
    ],
    "global_style": "电影感，浅景深，冷色调，胶片颗粒。",
}


def parses() -> list[dict]:
    """跑真实的 _parse，把产出的资产库整份 dump 出来。"""
    out = []
    for line in (StyleLine.REALISTIC, StyleLine.ANIME):
        gen = BibleGenerator.__new__(BibleGenerator)   # 不需要 config，_parse 用不上
        lib = gen._parse(json.dumps(MODEL_OUTPUT, ensure_ascii=False), line, "9:16")
        out.append({
            "style_line": line.value,
            "aspect_ratio": "9:16",
            "raw": json.dumps(MODEL_OUTPUT, ensure_ascii=False),
            "assets": json.loads(lib.model_dump_json()),
        })
    # 裹在代码块里也要能解析出一模一样的结果
    fenced = "```json\n" + json.dumps(MODEL_OUTPUT, ensure_ascii=False) + "\n```"
    gen = BibleGenerator.__new__(BibleGenerator)
    lib = gen._parse(fenced, StyleLine.REALISTIC, "16:9")
    out.append({
        "style_line": StyleLine.REALISTIC.value,
        "aspect_ratio": "16:9",
        "raw": fenced,
        "assets": json.loads(lib.model_dump_json()),
    })
    return out


def parse_failures() -> list[dict]:
    """必须报错的几种输入。

    只比"是否报错"，不比错误文字。但**记下抛的是哪个异常类型**——
    这里有一处 Python 的 bug：_parse 里 _extract_json 抛的是
    StoryboardError 不是 BibleError，而 /api/bible 只 catch BibleError，
    于是"模型返回了一坨不是 JSON 的东西"这种最常见的失败会变成 500
    而不是带提示的 400。C++ 侧统一成 BibleError（也就是 400），
    这是**有意的行为差异**，详见 C++重构方案.md。
    """
    cases = [
        ("没有角色", json.dumps({"characters": [], "locations": [],
                                 "global_style": "x"}, ensure_ascii=False)),
        ("返回数组不是对象", "[1, 2, 3]"),
        ("根本不是 JSON", "抱歉，我无法完成这个请求。"),
    ]
    out = []
    for name, raw in cases:
        gen = BibleGenerator.__new__(BibleGenerator)
        try:
            gen._parse(raw, StyleLine.REALISTIC, "9:16")
            out.append({"name": name, "raw": raw, "raises": False,
                        "python_error": None, "python_http_status": 200})
        except Exception as exc:                       # noqa: BLE001
            kind = type(exc).__name__
            out.append({
                "name": name, "raw": raw, "raises": True,
                "python_error": kind,
                # /api/bible 只 catch BibleError，别的一律穿到最外面变 500
                "python_http_status": 400 if kind == "BibleError" else 500,
            })
    return out


def main() -> None:
    data = {
        "note": "由 cpp/tests/export_bible_golden.py 生成，不要手改",
        "schema": _BIBLE_SCHEMA,
        "prompts": prompts(),
        "slugs": slugs(),
        "cleans": cleans(),
        "extracts": extracts(),
        "negatives": [
            {"style_line": line.value, "output": _default_negative(line)}
            for line in (StyleLine.REALISTIC, StyleLine.ANIME)
        ],
        "parses": parses(),
        "parse_failures": parse_failures(),
    }
    target = HERE.parent / "golden" / "stage_bible.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    with io.open(target, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("写好了", target)
    for k, v in data.items():
        if isinstance(v, list):
            print(f"  {k}: {len(v)} 条")


if __name__ == "__main__":
    main()
