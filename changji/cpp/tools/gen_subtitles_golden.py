"""生成字幕断行与 ASS 输出的对拍语料。

跑法（在 changji/ 下）：
    .venv/Scripts/python.exe cpp/tools/gen_subtitles_golden.py

期望值全部由 Python 的真函数算出来。ASS 的**整份文件内容**也进语料——
它要喂给 libass，多一个空格少一个逗号都可能让整条字幕轨不渲染，
而那要到成片出来才看得见。
"""

from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
CPP = HERE.parent.parent
ROOT = CPP.parent
sys.path.insert(0, str(ROOT / "src"))

from changji.assembly.subtitles import (                             # noqa: E402
    SubtitleCue, build_ass, display_width, validate_cues, wrap_chinese,
    _ass_time,
)

OUT = CPP / "tests" / "golden" / "subtitles"


# 每条都盯着一个具体的断行情形。
WRAP_CASES = [
    # 短句不断
    ("你来了。", 15.0, 2),
    # 标点后断是自然的
    ("我等了你三年，从来没有一天不在等。", 15.0, 2),
    # 没有标点，只能在宽度处硬断
    ("这是一句没有任何标点符号的很长很长的中文台词需要按宽度断开", 15.0, 2),
    # 标点不能在行首：断点要往前躲
    ("他终于说出了那句话，好。", 11.0, 2),
    # 左括号不能在行尾
    ("她看着窗外（雨已经下了一整夜）轻声说。", 9.0, 2),
    # 超过行数上限：多出来的并进最后一行，不丢字
    ("第一句话说完了，第二句话也说完了，第三句话还在继续说个不停。", 10.0, 2),
    # 中英混排。半角算半格，所以一行能放更多
    ("他说 OK let us go 然后转身走了。", 12.0, 2),
    # 全角标点连着出现
    ("什么？！你说什么？！", 6.0, 2),
    # 一个字符就超宽
    ("啊", 0.5, 2),
    # 空白
    ("   ", 15.0, 2),
    ("", 15.0, 2),
    # 只允许一行
    ("我等了你三年，从来没有一天不在等。", 15.0, 1),
    # 表情符号（宽的）
    ("好耶🎉🎉🎉真的成了", 6.0, 2),
]

WIDTH_CASES = [
    "你好",            # 全角
    "hello",           # 半角
    "你好hello",       # 混排
    "",                # 空
    "！？，。",         # 全角标点
    "!?,.",            # 半角标点
    "🎉",              # 表情
    "ü",               # 带音标的拉丁字母：窄的
    "Ａ",              # 全角拉丁字母：宽的
    "①",               # 圈号：不是 W/F
]

ASS_CASES = {
    "typical": [
        SubtitleCue(0.0, 2.5, "我等了你三年。"),
        SubtitleCue(2.5, 6.0, "从来没有一天不在等，一天也没有。"),
        SubtitleCue(6.0, 8.0, "雨夜天台", style="title"),
        SubtitleCue(8.0, 11.0, "那一夜之后，她再没回来过。", style="narration"),
    ],
    # 认不出的样式退回 dialogue
    "unknown_style": [SubtitleCue(0.0, 2.0, "你好", style="根本没有这个样式")],
    # 空文本要跳过，不能产出一条空的 Dialogue
    "empty_text": [
        SubtitleCue(0.0, 2.0, "有内容"),
        SubtitleCue(2.0, 3.0, "   "),
        SubtitleCue(3.0, 5.0, "还有内容"),
    ],
    # 一条都没有时也要是合法的 ASS
    "no_cues": [],
    # 跨小时的时间戳
    "long_timeline": [SubtitleCue(3661.5, 3665.25, "一小时之后")],
}

TIME_CASES = [0.0, 0.5, 1.005, 59.99, 60.0, 3599.99, 3600.0, 3661.5, -1.0,
              7265.125]

VALIDATE_CASES = {
    "all_good": [
        SubtitleCue(0.0, 2.0, "第一句"),
        SubtitleCue(2.0, 4.0, "第二句"),
    ],
    "reversed_time": [SubtitleCue(3.0, 1.0, "时间倒挂")],
    "too_short": [SubtitleCue(0.0, 0.2, "一闪而过")],
    "empty": [SubtitleCue(0.0, 2.0, "  ")],
    "overlap": [
        SubtitleCue(0.0, 3.0, "前一条字幕的内容"),
        SubtitleCue(2.0, 5.0, "后一条字幕的内容"),
    ],
    "way_too_long": [
        SubtitleCue(0.0, 5.0, "这是一句长到断成两行之后每一行还是超出上限很多很多"
                              "很多很多很多很多很多很多的台词"),
    ],
}


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    payload = {
        "wrap": [], "width": [], "ass": [], "ass_time": [], "validate": [],
    }

    for text, w, lines in WRAP_CASES:
        payload["wrap"].append({
            "text": text, "max_width": w, "max_lines": lines,
            "expected": wrap_chinese(text, w, lines),
        })

    for t in WIDTH_CASES:
        payload["width"].append({"text": t, "expected": display_width(t)})

    for name, cues in ASS_CASES.items():
        payload["ass"].append({
            "name": name,
            "cues": [{"start_s": c.start_s, "end_s": c.end_s,
                      "text": c.text, "style": c.style} for c in cues],
            "expected": build_ass(cues),
        })

    for t in TIME_CASES:
        payload["ass_time"].append({"input": t, "expected": _ass_time(t)})

    for name, cues in VALIDATE_CASES.items():
        payload["validate"].append({
            "name": name,
            "cues": [{"start_s": c.start_s, "end_s": c.end_s,
                      "text": c.text, "style": c.style} for c in cues],
            "expected": validate_cues(cues),
        })

    dest = OUT / "subtitles.json"
    dest.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                    encoding="utf-8")
    print(f"断行 {len(payload['wrap'])} 条，宽度 {len(payload['width'])} 条，"
          f"ASS {len(payload['ass'])} 份，自检 {len(payload['validate'])} 组")
    print(f"写到 {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
