#!/usr/bin/env python3
"""把 Python 侧的真实模型导出成对拍语料。

用完即弃的一次性脚本。方案第六节定的是"对拍程序全用 C++、彻底去 Python"，
但语料本身必须由**真实的 pydantic 模型**生成——手写 JSON 只能测出写的人
对字段的理解，测不出真实格式。所以留这一个脚本，跑一次产出 golden/ 下的
文件，之后 C++ 侧只读文件。

用法（在 changji/ 目录下）：

    ./.venv/Scripts/python.exe cpp/tests/export_golden.py

上游 models/ 改了字段就重跑一次。
"""

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "src"))

from changji.models.shot import (  # noqa: E402
    CameraAngle,
    CameraMove,
    CharacterInShot,
    DialogueLine,
    FacePose,
    Shot,
    ShotSize,
    ShotStatus,
    Transition,
    derive_needs_lipsync,
)

OUT = pathlib.Path(__file__).resolve().parent / "golden"
OUT.mkdir(parents=True, exist_ok=True)


def dump(name: str, payload) -> None:
    path = OUT / f"{name}.json"
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"  {path.name}")


# ── 1. 只有必填字段的镜头 ────────────────────────────────────────────────
#
# 测的是默认值。C++ 侧的默认值和 pydantic 的必须逐个对得上，
# 否则读一个 Python 写的省略字段的文件时会静默拿到不同的值。
minimal = Shot(shot_id="ep01_s01_sh001", scene_id="ep01_s01", order=0)
dump("shot_minimal", minimal.model_dump(mode="json"))


# ── 2. 字段填满的镜头，内容全中文 ────────────────────────────────────────
#
# 中文是刻意的。pydantic 的 max_length 数的是字符，C++ 的 std::string::size()
# 数字节，一个汉字差三倍。只用英文语料测不出这个差异。
full = Shot(
    shot_id="ep01_s03_sh007",
    scene_id="ep01_s03",
    order=6,
    visual_desc="雨夜天台，男主背对镜头站在栏杆前，远处霓虹在雨幕里散成一片光晕",
    first_frame_prompt="cinematic, rooftop at night, heavy rain, neon bokeh, back view",
    last_frame_prompt="cinematic, rooftop at night, man turns around, rain on face",
    motion_prompt="缓慢推进，雨丝斜掠，人物微微转头",
    negative_prompt="模糊, 低质量, 多余的手指",
    shot_size=ShotSize.MCU,
    camera_angle=CameraAngle.LOW,
    camera_move=CameraMove.PUSH_IN,
    camera_id="cam_rooftop_a",
    characters=[
        CharacterInShot(
            char_id="lin_yuan",
            expression="隐忍",
            action="握紧栏杆，指节发白",
            wardrobe_state="suit_soaked",
            face_pose=FacePose.THREE_QUARTER,
            screen_pos="center",
        ),
        CharacterInShot(
            char_id="chen_mo",
            expression="愕然",
            action="停在门口，没有再往前",
            face_pose=FacePose.PROFILE,
            screen_pos="left",
        ),
    ],
    location_id="loc_rooftop",
    prop_ids=["prop_umbrella", "prop_phone"],
    duration_s=6.5,
    duration_locked=True,
    dialogue=[
        DialogueLine(
            char_id="lin_yuan",
            text="你要是早半个小时来，什么都还来得及。",
            emotion="压抑",
            emotion_intensity=0.8,
            voice_id="voice_lin",
            audio_path="audio/ep01_s03_sh007_01.wav",
            actual_duration_s=3.2,
        ),
        DialogueLine(
            text="雨声盖过了他后半句。",
            emotion="neutral",
            emotion_intensity=0.3,
        ),
    ],
    sfx=["雨声", "远处车流"],
    bgm_cue="bgm_tension_low",
    transition_in=Transition.DISSOLVE,
    transition_dur_s=0.8,
    subtitle_text="你要是早半个小时来，什么都还来得及。",
    beat="反转",
    continuity_notes="上一镜他还没湿透，这里服装状态要接上 suit_soaked",
    missing_info=["未确定陈墨是否带伞"],
    status=ShotStatus.AUDIO_DONE,
    attempts=2,
    frame_path="frames/ep01_s03_sh007.png",
    video_path=None,
    gate_notes=["草稿档画面偏暗，已调高曝光重跑"],
)
dump("shot_full", full.model_dump(mode="json"))


# ── 3. 口型判定的真值表 ──────────────────────────────────────────────────
#
# derive_needs_lipsync 是"必须用规则算不能交给大模型"的那条。
# C++ 侧的实现要和这张表逐行一致。
cases = []


def case(name: str, shot: Shot) -> None:
    cases.append({
        "name": name,
        "shot": shot.model_dump(mode="json"),
        "expected_needs_lipsync": derive_needs_lipsync(shot),
    })


def speaker(pose: FacePose) -> list:
    return [CharacterInShot(char_id="a", face_pose=pose)]


base = dict(shot_id="t", scene_id="t", order=0)
line = [DialogueLine(char_id="a", text="一句话")]
narration = [DialogueLine(text="旁白")]

case("无台词", Shot(**base, characters=speaker(FacePose.FRONT)))
case("只有旁白", Shot(**base, characters=speaker(FacePose.FRONT), dialogue=narration))
case("特写正脸", Shot(**base, shot_size=ShotSize.CU,
                      characters=speaker(FacePose.FRONT), dialogue=line))
case("中景正脸", Shot(**base, shot_size=ShotSize.MS,
                      characters=speaker(FacePose.FRONT), dialogue=line))
case("中远景正脸_太远", Shot(**base, shot_size=ShotSize.MLS,
                             characters=speaker(FacePose.FRONT), dialogue=line))
case("特写背对镜头", Shot(**base, shot_size=ShotSize.CU,
                          characters=speaker(FacePose.BACK), dialogue=line))
case("特写侧脸", Shot(**base, shot_size=ShotSize.CU,
                      characters=speaker(FacePose.PROFILE), dialogue=line))
case("顶拍特写正脸", Shot(**base, shot_size=ShotSize.CU,
                          camera_angle=CameraAngle.OVERHEAD,
                          characters=speaker(FacePose.FRONT), dialogue=line))
case("俯拍特写正脸", Shot(**base, shot_size=ShotSize.CU,
                          camera_angle=CameraAngle.HIGH,
                          characters=speaker(FacePose.FRONT), dialogue=line))
case("画外音角色", Shot(**base, shot_size=ShotSize.CU,
                        characters=speaker(FacePose.OFF_SCREEN), dialogue=line))

dump("lipsync_truth_table", cases)


# ── 4. 应该被校验拒绝的镜头 ──────────────────────────────────────────────
#
# 只存原始 JSON 和一句人话的理由——这些数据构造不出 Shot 实例，
# 所以不能用 model_dump，得手写。C++ 侧要保证同样拒绝。
invalid = [
    {
        "why": "硬切却给了转场时长",
        "json": {"shot_id": "a", "scene_id": "b", "order": 0,
                 "transition_in": "cut", "transition_dur_s": 0.5},
    },
    {
        "why": "溶解转场时长为 0",
        "json": {"shot_id": "a", "scene_id": "b", "order": 0,
                 "transition_in": "dissolve", "transition_dur_s": 0},
    },
    {
        "why": "台词说话人不在本镜角色列表里",
        "json": {"shot_id": "a", "scene_id": "b", "order": 0,
                 "characters": [{"char_id": "x"}],
                 "dialogue": [{"char_id": "y", "text": "谁在说话"}]},
    },
    {
        "why": "shot_id 有大写字母",
        "json": {"shot_id": "EP01", "scene_id": "b", "order": 0},
    },
    {
        "why": "镜头时长超过 30 秒",
        "json": {"shot_id": "a", "scene_id": "b", "order": 0, "duration_s": 31},
    },
    {
        "why": "台词为空串",
        "json": {"shot_id": "a", "scene_id": "b", "order": 0,
                 "characters": [{"char_id": "x"}],
                 "dialogue": [{"char_id": "x", "text": ""}]},
    },
    {
        "why": "visual_desc 超过 300 字（用汉字，字节数是 900）",
        "json": {"shot_id": "a", "scene_id": "b", "order": 0,
                 "visual_desc": "雨" * 301},
    },
]

# 顺带确认这些真的会被 pydantic 拒绝——不然语料本身就是错的
for item in invalid:
    try:
        Shot.model_validate(item["json"])
    except Exception:
        pass
    else:
        raise SystemExit(f"语料有误：'{item['why']}' 居然通过了 pydantic 校验")

dump("shot_invalid", invalid)


# ── 5. 恰好 300 个汉字，必须通过 ────────────────────────────────────────
#
# 和上面那条 301 字的配对。C++ 侧如果按字节数判，这一条会被误拒。
boundary = Shot(**base, visual_desc="雨" * 300)
dump("shot_boundary_300_chars", boundary.model_dump(mode="json"))

print(f"\n语料写入 {OUT}")
