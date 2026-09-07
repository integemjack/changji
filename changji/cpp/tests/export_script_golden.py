"""导出剧本/选题/预告片阶段的对拍语料。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_script_golden.py

产出 cpp/tests/golden/stage_script.json。

规矩同前两个：**调真实的 Python 函数**，不复述它的逻辑。
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.models.character import StyleLine                     # noqa: E402
from changji.stages.script import (                                # noqa: E402
    _PREMISE_SCHEMA, _SCHEMA, ScriptDraft, ScriptError, ScriptGenerator,
    budget_chars, build_premise_prompt, build_prompt, build_trailer_prompt,
    normalize_speaker, strip_leading_timecode, strip_wrapper,
)

PREMISE = "林晚在天台等一个七年没出现的人，而那个人今晚会来。"
PREV = "第一集：林晚辞职。\n第二集：她翻出旧照片。"
EPISODES = "第一集：林晚辞职。\n第二集：她翻出旧照片。\n第三集：陈默出现。"


def wrappers() -> list[dict]:
    cases = [
        "（他犹豫了一下）",
        "他犹豫了一下",
        "（甲说）乙答（丙笑）",          # 首尾不是一对，不能削
        "((双层))",
        "【标题】",
        "“一句台词”",
        '"英文引号"',
        "「日式引号」",
        "『双层日式』",
        "'单引号'",
        "（（（三层）））",
        "（）",                          # 削完是空的，不削
        "（   ）",                       # 里面全是空白
        "  （带空白）  ",
        "（未闭合",
        "未开始）",
        "（前）中间（后）",
        "",
        "（a）（b）",                    # 两对并列，首尾不是同一对
        "（他说“好”）",                  # 嵌套不同类型
        "【[混合]】",
        "（1）",
    ]
    return [{"in": c, "out": strip_wrapper(c)} for c in cases]


def timecodes() -> list[dict]:
    cases = [
        "[0-3秒] 画面特写：雨水",
        "【0-3秒】天台全景",
        "（0:03）近景",
        "(3s) 推进",
        "（他犹豫了）他开口",            # 没数字，不削
        "（第3次）他又问",               # 有数字没单位，不削
        "（他犹豫了3秒）他开口",         # 有数字有单位，削掉——这是已知的误伤
        "[1分20秒] 转场",
        "[0-3帧] 抖动",
        "没有任何标记的一行",
        "[未闭合 的方括号",
        "[]空的",
        "[0-3秒]",                       # 削完只剩空串
        "  [0-3秒] 前面有空白",
    ]
    return [{"in": c, "out": strip_leading_timecode(c)} for c in cases]


def speakers() -> list[dict]:
    cases = [
        "林晚", "none", "None", "NONE", "null", "n/a", "N/A", "NA",
        "-", "—", "无", "空", "旁白", "画外音", "narrator", "Narrator",
        "voiceover", "VO", "ost", "none.", "（无）", "(none)",
        "（林晚）", "  林晚  ", "", "  ", "旁 白",
    ]
    return [{"in": c, "out": normalize_speaker(c)} for c in cases]


def budgets() -> list[dict]:
    return [{"in": d, "out": budget_chars(d)}
            for d in [0.0, 1.0, 5.0, 7.0, 7.1, 20.0, 30.0, 60.0, 90.0,
                      120.0, 300.0, -5.0, 0.5]]


def prompts() -> list[dict]:
    out = []
    for line in (StyleLine.REALISTIC, StyleLine.ANIME):
        for prev, chars in [("", None), ("", ["林晚", "陈默"]),
                            (PREV, None), (PREV, ["林晚", "陈默"])]:
            out.append({
                "kind": "script",
                "style_line": line.value,
                "premise": PREMISE,
                "duration_s": 60.0,
                "previous": prev,
                "characters": chars or [],
                "prompt": build_prompt(PREMISE, 60.0, line,
                                       previous=prev, characters=chars),
            })
    # 时长走 f"{:.0f}"，是银行家舍入，挑几个卡在半整数上的
    for d in [0.5, 1.5, 2.5, 59.5, 60.4, 60.5]:
        out.append({
            "kind": "script", "style_line": "realistic", "premise": PREMISE,
            "duration_s": d, "previous": "", "characters": [],
            "prompt": build_prompt(PREMISE, d, StyleLine.REALISTIC),
        })
    # 前情超长要截断到 4000 字
    long_prev = "很长的前情。" * 1200
    out.append({
        "kind": "script", "style_line": "realistic", "premise": PREMISE,
        "duration_s": 60.0, "previous": long_prev, "characters": [],
        "prompt": build_prompt(PREMISE, 60.0, StyleLine.REALISTIC,
                               previous=long_prev),
    })

    for line in (StyleLine.REALISTIC, StyleLine.ANIME):
        for kw, existing in [("", None), ("都市 悬疑", None),
                             ("", ["复仇爽剧", "重生逆袭"]),
                             ("都市 悬疑", ["复仇爽剧", "重生逆袭"])]:
            out.append({
                "kind": "premise",
                "style_line": line.value,
                "keywords": kw,
                "count": 3,
                "existing": existing or [],
                "prompt": build_premise_prompt(kw, line, count=3,
                                               existing=existing),
            })
    # existing 超过六条只取前六，每条截到 60 字
    many = ["方向%d：%s" % (i, "描述" * 50) for i in range(10)]
    out.append({
        "kind": "premise", "style_line": "realistic", "keywords": "",
        "count": 5, "existing": many,
        "prompt": build_premise_prompt("", StyleLine.REALISTIC, count=5,
                                       existing=many),
    })

    for line in (StyleLine.REALISTIC, StyleLine.ANIME):
        for eps, chars in [("", None), ("", ["林晚"]),
                           (EPISODES, None), (EPISODES, ["林晚", "陈默"])]:
            out.append({
                "kind": "trailer",
                "style_line": line.value,
                "premise": PREMISE,
                "duration_s": 20.0,
                "episodes": eps,
                "characters": chars or [],
                "prompt": build_trailer_prompt(PREMISE, 20.0, line,
                                               episodes=eps,
                                               characters=chars),
            })
    long_eps = "很长的剧集。" * 1500
    out.append({
        "kind": "trailer", "style_line": "realistic", "premise": PREMISE,
        "duration_s": 20.0, "episodes": long_eps, "characters": [],
        "prompt": build_trailer_prompt(PREMISE, 20.0, StyleLine.REALISTIC,
                                       episodes=long_eps),
    })
    return out


MODEL_SCRIPT = {
    "title": "雨夜天台",
    "logline": "等了七年的人今晚来了。",
    "beats": [
        {"kind": "action", "speaker": "",
         "text": "[0-3秒] 夜。天台。雨水打在水泥地上。"},
        {"kind": "action", "speaker": "", "text": "（林晚站在护栏边，背对镜头。）"},
        {"kind": "dialogue", "speaker": "林晚", "text": "“你说过会来的。”"},
        {"kind": "action", "speaker": "", "text": "陈默从阴影里走出来。"},
        {"kind": "dialogue", "speaker": "陈默", "text": "我来了。晚了七年。"},
        # 说了话但没说是谁，要降级成动作
        {"kind": "dialogue", "speaker": "none", "text": "远处传来汽笛声。"},
        # 空文本，整条丢掉
        {"kind": "dialogue", "speaker": "林晚", "text": "   "},
        # 不是对象，跳过
        "这不是一个对象",
        # kind 是别的值，当动作
        {"kind": "narration", "speaker": "", "text": "雨停了。"},
    ],
}


def parses() -> list[dict]:
    raw = json.dumps(MODEL_SCRIPT, ensure_ascii=False)
    draft = ScriptGenerator._parse(raw)
    out = [{
        "name": "正常一集",
        "raw": raw,
        "title": draft.title,
        "logline": draft.logline,
        "beats": draft.beats,
        "speakers": draft.speakers,
        "dialogue_chars": draft.dialogue_chars,
        "render": draft.render(),
    }]
    # 裹在代码块里
    fenced = "```json\n" + raw + "\n```"
    d2 = ScriptGenerator._parse(fenced)
    out.append({
        "name": "裹在代码块里",
        "raw": fenced,
        "title": d2.title, "logline": d2.logline, "beats": d2.beats,
        "speakers": d2.speakers, "dialogue_chars": d2.dialogue_chars,
        "render": d2.render(),
    })
    return out


def parse_failures() -> list[dict]:
    cases = [
        ("没有 beats", json.dumps({"title": "x", "logline": "y"})),
        ("beats 是空数组", json.dumps({"title": "x", "beats": []})),
        ("返回数组不是对象", "[1, 2, 3]"),
        ("全是空文本", json.dumps(
            {"beats": [{"kind": "dialogue", "speaker": "a", "text": "  "}]},
            ensure_ascii=False)),
        ("一句台词都没有", json.dumps(
            {"beats": [{"kind": "action", "speaker": "", "text": "雨。"}]},
            ensure_ascii=False)),
        ("根本不是 JSON", "抱歉，我做不到。"),
    ]
    out = []
    for name, raw in cases:
        try:
            ScriptGenerator._parse(raw)
            out.append({"name": name, "raw": raw, "raises": False,
                        "python_error": None})
        except Exception as exc:                    # noqa: BLE001
            out.append({"name": name, "raw": raw, "raises": True,
                        "python_error": type(exc).__name__})
    return out


MODEL_PREMISES = {
    "ideas": [
        {"title": "《雨夜天台》", "premise": "（林晚在天台等一个七年没出现的人。）",
         "hook": "“他会来吗？”"},
        {"title": "旧照片", "premise": "一张照片牵出十年前的火灾真相。",
         "hook": "照片背面写着她的名字。"},
        {"title": "", "premise": "", "hook": "空的，要被跳过"},
        "这不是对象",
        {"title": "第三个", "premise": "外卖员送错一单，撞破一场骗局。", "hook": ""},
    ]
}


def premise_parses() -> list[dict]:
    raw = json.dumps(MODEL_PREMISES, ensure_ascii=False)
    ideas = ScriptGenerator._parse_premises(raw)
    out = [{
        "name": "正常",
        "raw": raw,
        "ideas": [{"title": i.title, "premise": i.premise, "hook": i.hook}
                  for i in ideas],
    }]
    # 顶层直接是数组
    raw2 = json.dumps(MODEL_PREMISES["ideas"], ensure_ascii=False)
    ideas2 = ScriptGenerator._parse_premises(raw2)
    out.append({
        "name": "顶层是数组",
        "raw": raw2,
        "ideas": [{"title": i.title, "premise": i.premise, "hook": i.hook}
                  for i in ideas2],
    })
    return out


def premise_failures() -> list[dict]:
    cases = [
        ("空数组", json.dumps({"ideas": []})),
        ("全是空的", json.dumps(
            {"ideas": [{"title": "a", "premise": "", "hook": "b"}]},
            ensure_ascii=False)),
        ("不是列表", json.dumps({"ideas": {"a": 1}})),
        ("根本不是 JSON", "做不到。"),
    ]
    out = []
    for name, raw in cases:
        try:
            ScriptGenerator._parse_premises(raw)
            out.append({"name": name, "raw": raw, "raises": False,
                        "python_error": None})
        except Exception as exc:                    # noqa: BLE001
            out.append({"name": name, "raw": raw, "raises": True,
                        "python_error": type(exc).__name__})
    return out


def renders() -> list[dict]:
    """render / speakers / dialogue_chars 的边界。"""
    cases = [
        ("空的", []),
        ("只有动作", [{"kind": "action", "speaker": "", "text": "雨。"}]),
        ("对白没有名字", [{"kind": "dialogue", "speaker": "", "text": "谁在说话"}]),
        ("同一个人说两次", [
            {"kind": "dialogue", "speaker": "林晚", "text": "一"},
            {"kind": "action", "speaker": "", "text": "停顿"},
            {"kind": "dialogue", "speaker": "林晚", "text": "二"},
            {"kind": "dialogue", "speaker": "陈默", "text": "三"},
        ]),
        ("文本两端有空白", [
            {"kind": "dialogue", "speaker": " 林晚 ", "text": "  台词  "},
        ]),
        ("空文本被跳过", [
            {"kind": "dialogue", "speaker": "林晚", "text": ""},
            {"kind": "action", "speaker": "", "text": "有内容"},
        ]),
    ]
    out = []
    for name, beats in cases:
        d = ScriptDraft(title="t", logline="l", beats=beats)
        out.append({
            "name": name, "beats": beats,
            "render": d.render(),
            "speakers": d.speakers,
            "dialogue_chars": d.dialogue_chars,
        })
    return out


def main() -> None:
    data = {
        "note": "由 cpp/tests/export_script_golden.py 生成，不要手改",
        "script_schema": _SCHEMA,
        "premise_schema": _PREMISE_SCHEMA,
        "wrappers": wrappers(),
        "timecodes": timecodes(),
        "speakers": speakers(),
        "budgets": budgets(),
        "prompts": prompts(),
        "parses": parses(),
        "parse_failures": parse_failures(),
        "premise_parses": premise_parses(),
        "premise_failures": premise_failures(),
        "renders": renders(),
    }
    target = HERE.parent / "golden" / "stage_script.json"
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
