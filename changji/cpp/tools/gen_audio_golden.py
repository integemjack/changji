"""生成配音时长估算与拆分的对拍语料。

跑法（在 changji/ 下）：
    .venv/Scripts/python.exe cpp/tools/gen_audio_golden.py

期望值全部由 Python 的真函数算出来。

**这一层错了的后果是成片里两个人同时说话**：一个镜头装不下自己的台词，
混音时后面的声音盖到下一镜上去。而这件事在装配之前没有任何迹象——
分镜表看着正常，每一段音频单独听也正常。所以估时长和拆分的每一条
边界都要钉住。
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
CPP = HERE.parent.parent
ROOT = CPP.parent
sys.path.insert(0, str(ROOT / "src"))

from changji.models.shot import (                                    # noqa: E402
    CameraAngle, CharacterInShot, DialogueLine, Shot, ShotSize,
)
from changji.stages.audio import (                                   # noqa: E402
    estimate_speech_duration, max_line_seconds, split_long_text,
    split_overlong_shots, _free_shot_id, _group_lines,
)

OUT = CPP / "tests" / "golden" / "audio"


DURATION_CASES = [
    "",
    "   ",
    "你好",
    "我等了你三年。",
    # 标点密集：把标点当字算的话会低估近一秒
    "什么？！你说什么？！",
    "一，二，三，四，五。",
    # 省略号和破折号
    "他张了张嘴……终究什么也没说——",
    # 中英混排
    "他说 OK 然后转身走了。",
    # 带空白：空白要全部去掉，不只是首尾
    "  我 等 了 你 三 年 。  ",
    # 很长一段
    "那一夜的雨下得特别大，她站在天台边上，看着楼下川流不息的车灯，"
    "忽然觉得这座城市里没有一个人在等她回家。",
    # 纯标点
    "。。。",
    # 换行
    "第一句。\n第二句。",
]

SPLIT_CASES = [
    # 短的不切
    ("我等了你三年。", 5.0),
    # 在句末标点处切
    ("我等了你三年。你却什么都没说。那一夜之后我再没见过你。", 3.0),
    # 句末标点不够，退到逗号
    ("那一夜的雨下得特别大，她站在天台边上，看着楼下川流不息的车灯，"
     "忽然觉得这座城市里没有一个人在等她回家", 3.0),
    # 一个标点都没有，只能按字数硬切
    ("这是一句完全没有任何标点符号的很长很长很长的台词需要按照字数硬切开来才行", 3.0),
    # 上限很小，逼出多次硬切
    ("这是一句完全没有任何标点符号的很长很长很长的台词", 1.0),
    # 上限比一个字还小
    ("一二三四五六七八九十", 0.3),
    # 空
    ("", 5.0),
    # 刚好在边界上
    ("十个字的一句话啊", 2.5),
]

FREE_ID_CASES = [
    ("ep01_sh001", []),
    ("ep01_sh001", ["ep01_sh001"]),
    ("ep01_sh001", ["ep01_sh001", "ep01_sh001_b"]),
    ("ep01_sh001", ["ep01_sh001", "ep01_sh001_b", "ep01_sh001_c"]),
    # 二十五个后缀全用光，退回数字
    ("x", ["x_" + c for c in "bcdefghijklmnopqrstuvwxyz"]),
]


def line(text: str, dur=None, char_id="c_lin"):
    d = {"text": text, "char_id": char_id}
    if dur is not None:
        d["actual_duration_s"] = dur
    return DialogueLine(**d)


def shot(shot_id: str, order: int, lines, duration=5.0):
    # Shot 会校验台词里的 char_id 在不在 characters 里，所以要带上。
    return Shot(
        shot_id=shot_id, scene_id="sc01", order=order,
        shot_size=ShotSize.MS, camera_angle=CameraAngle.EYE_LEVEL,
        first_frame_prompt="雨夜天台", duration_s=duration, dialogue=lines,
        characters=[CharacterInShot(char_id="c_lin")],
    )


def dump_shot(s: Shot) -> dict:
    return {
        "shot_id": s.shot_id, "order": s.order,
        "status": s.status.value, "attempts": s.attempts,
        "duration_locked": s.duration_locked,
        "frame_path": s.frame_path, "video_path": s.video_path,
        "dialogue": [{"text": l.text,
                      "actual_duration_s": l.actual_duration_s} for l in s.dialogue],
    }


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    payload = {
        "constants": {
            "chars_per_second": 4.6,
            "lead_in_s": 0.15,
            "tail_s": 0.25,
            "max_line_seconds_24fps": max_line_seconds(24),
            "max_line_seconds_30fps": max_line_seconds(30),
        },
        "duration": [{"text": t, "expected": estimate_speech_duration(t)}
                     for t in DURATION_CASES],
        "split": [{"text": t, "max_seconds": m,
                   "expected": split_long_text(t, m)}
                  for t, m in SPLIT_CASES],
        "free_id": [{"base": b, "used": u, "expected": _free_shot_id(b, set(u))}
                    for b, u in FREE_ID_CASES],
        "group": [],
        "split_shots": [],
    }

    # 打包：一个镜头的台词分几组
    group_cases = {
        "single_line": shot("sh001", 0, [line("就一句话。")]),
        "two_short": shot("sh002", 1, [line("第一句。"), line("第二句。")]),
        "three_long": shot("sh003", 2, [
            line("那一夜的雨下得特别大，她站在天台边上。"),
            line("看着楼下川流不息的车灯，忽然觉得这座城市里没有一个人在等她。"),
            line("她转身走了。"),
        ]),
        # 用真实配音时长而不是估算
        "with_real_durations": shot("sh004", 3, [
            line("第一句。", 3.0), line("第二句。", 3.0), line("第三句。", 3.0),
        ]),
        # 一句就超预算：也要单独成组，不能丢
        "one_line_over_budget": shot("sh005", 4, [
            line("这一句非常非常长长到一个镜头根本装不下它需要单独占一个镜头才行呢", 9.0),
            line("短的。", 1.0),
        ]),
        "no_dialogue": shot("sh006", 5, []),
    }
    for name, s in group_cases.items():
        groups = _group_lines(s, max_line_seconds(24))
        payload["group"].append({
            "name": name,
            "shot": dump_shot(s),
            "expected": [[l.text for l in g] for g in groups],
        })

    # 整批拆分
    split_shot_cases = {
        "nothing_to_split": [
            shot("sh001", 0, [line("短的一句。", 2.0)]),
            shot("sh002", 1, [line("也很短。", 1.5)]),
        ],
        "one_splits": [
            shot("sh001", 0, [line("A。", 3.0), line("B。", 3.0), line("C。", 3.0)]),
            shot("sh002", 1, [line("短的。", 1.0)]),
        ],
        # 重跑：已经拆过一次，编号不能撞
        "already_split_once": [
            shot("sh001", 0, [line("A。", 3.0), line("B。", 3.0), line("C。", 3.0)]),
            shot("sh001_b", 1, [line("旧的拆分。", 1.0)]),
        ],
    }
    for name, shots in split_shot_cases.items():
        # split_overlong_shots 会就地改 shot.dialogue，所以每个 case
        # 都用新造的对象
        fresh = [s.model_copy(deep=True) for s in shots]
        out = split_overlong_shots(fresh, max_line_seconds(24))
        payload["split_shots"].append({
            "name": name,
            "shots": [dump_shot(s) for s in shots],
            "expected": [dump_shot(s) for s in out],
        })
        print(f"split_shots/{name}: {len(shots)} -> {len(out)} 镜")

    dest = OUT / "audio_plan.json"
    dest.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                    encoding="utf-8")
    print(f"时长 {len(payload['duration'])} 条，切句 {len(payload['split'])} 条，"
          f"打包 {len(payload['group'])} 组")
    print(f"写到 {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
