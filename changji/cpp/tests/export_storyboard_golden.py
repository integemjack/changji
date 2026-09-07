"""导出分镜阶段的对拍语料。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_storyboard_golden.py

产出 cpp/tests/golden/stage_storyboard.json。

规矩同 export_bible_golden.py：**调真实的 Python 函数**，不复述它的逻辑。
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]          # .../changji
sys.path.insert(0, str(REPO / "src"))

from changji.models.character import (                              # noqa: E402
    AppearanceBlock, AssetLibrary, Character, Location, StyleProfile,
)
from changji.models.shot import Shot                                # noqa: E402
from changji.stages.storyboard import (                             # noqa: E402
    DURATION_SLOTS, DurationQuota, StoryboardError, StoryboardGenerator,
    _add_missing_speakers, build_prompt, ceil_duration, link_location,
    llm_shot_schema, rebalance_durations, snap_duration,
)

SCRIPT = """第一集《雨夜天台》

林晚站在天台边缘，雨水打湿了她的白衬衫。
林晚：你说过会来的。
陈默从阴影里走出来，手里攥着一把伞。
陈默：我来了。晚了七年。
"""


def assets() -> AssetLibrary:
    return AssetLibrary(
        characters={
            "c_lin_wan": Character(
                char_id="c_lin_wan", name="林晚", voice_order=0,
                appearance=AppearanceBlock(
                    identity="二十七岁女性", body="偏瘦",
                    face="黑色长直发，杏眼", attire="白色衬衫")),
            "c_chen_mo": Character(
                char_id="c_chen_mo", name="陈默", voice_order=1,
                appearance=AppearanceBlock(
                    identity="三十出头男性", body="高瘦",
                    face="短寸黑发，浓眉", attire="黑色风衣")),
        },
        locations={
            "loc_rooftop": Location(
                location_id="loc_rooftop", name="夜间天台",
                space="水泥地面，锈蚀护栏", lighting="夜间冷调顶光"),
        },
        style=StyleProfile(global_style="电影感，冷色调"),
    )


def durations() -> dict:
    snaps = [0.0, 0.5, 1.0, 2.0, 2.4, 2.5, 2.6, 3.5, 4.4, 4.5, 5.0,
             7.0, 100.0, -3.0]
    ceils = [0.0, 1.9, 2.0, 2.0001, 2.5, 3.0, 4.9, 5.0, 5.1, 99.0]
    return {
        "slots": list(DURATION_SLOTS),
        "max_shot_duration_s": __import__(
            "changji.stages.render", fromlist=["x"]).max_shot_duration_s(),
        "snap": [{"in": s, "out": snap_duration(s)} for s in snaps],
        "ceil": [{"in": s, "out": ceil_duration(s)} for s in ceils],
    }


def quotas() -> list[dict]:
    out = []
    for target in [1.0, 5.0, 10.0, 30.0, 45.0, 60.0, 90.0, 120.0, 300.0,
                   7.5, 33.3, 0.5]:
        q = DurationQuota.for_duration(target)
        out.append({
            "target_s": target,
            # 键是 float，JSON 里只能用字符串，用 repr 保留精度
            "slots": [[d, n] for d, n in q.slots.items()],
            "total_s": q.total_s,
            "shot_count": q.shot_count,
            "describe": q.describe(),
        })
    return out


def prompts() -> list[dict]:
    a = assets()
    out = []
    for target, ep in [(60.0, "ep_01"), (30.0, "ep_02")]:
        q = DurationQuota.for_duration(target)
        out.append({
            "target_s": target,
            "episode_id": ep,
            "script": SCRIPT,
            "prompt": build_prompt(SCRIPT, a, q, ep),
        })
    # 没有场景时那句占位文字要出现
    no_loc = assets()
    no_loc.locations = {}
    q = DurationQuota.for_duration(60.0)
    out.append({
        "target_s": 60.0,
        "episode_id": "ep_03",
        "script": SCRIPT,
        "no_locations": True,
        "prompt": build_prompt(SCRIPT, no_loc, q, "ep_03"),
    })
    return out


def schemas() -> list[dict]:
    a = assets()
    out = [{"name": "带场景", "schema": llm_shot_schema(a)}]
    no_loc = assets()
    no_loc.locations = {}
    out.append({"name": "没场景", "schema": llm_shot_schema(no_loc)})
    return out


def links() -> list[dict]:
    known = {"loc_rooftop", "loc_office"}
    cases = [
        {"scene_id": "loc_rooftop"},                          # 接上
        {"scene_id": "loc_rooftop", "location_id": "loc_office"},  # 已有，不动
        {"scene_id": "sc_01"},                                # 不是已注册场景
        {"scene_id": "loc_rooftop", "location_id": ""},       # 空串算没填
        {"scene_id": "loc_rooftop", "location_id": None},     # null 也算没填
        {},                                                    # 什么都没有
    ]
    out = []
    for c in cases:
        item = json.loads(json.dumps(c))
        changed = link_location(item, known)
        out.append({"before": c, "changed": changed, "after": item})
    return out


def speakers() -> list[dict]:
    known = {"c_lin_wan", "c_chen_mo"}
    cases = [
        # 说话人不在 characters 里，要补上
        {"dialogue": [{"char_id": "c_lin_wan", "text": "你说过会来的"}],
         "characters": []},
        # 已经在里面了，不重复补
        {"dialogue": [{"char_id": "c_lin_wan", "text": "x"}],
         "characters": [{"char_id": "c_lin_wan"}]},
        # 旁白（char_id 为空）不补
        {"dialogue": [{"char_id": None, "text": "旁白"}], "characters": []},
        # 不认识的 id 不补
        {"dialogue": [{"char_id": "c_unknown", "text": "x"}], "characters": []},
        # 没有 characters 字段，要建出来
        {"dialogue": [{"char_id": "c_chen_mo", "text": "x"}]},
        # 两句台词两个人
        {"dialogue": [{"char_id": "c_lin_wan", "text": "a"},
                      {"char_id": "c_chen_mo", "text": "b"},
                      {"char_id": "c_lin_wan", "text": "c"}],
         "characters": []},
        # 空对白，什么都不做
        {"dialogue": [], "characters": []},
    ]
    out = []
    for c in cases:
        item = json.loads(json.dumps(c))
        _add_missing_speakers(item, known)
        out.append({"before": c, "after": item})
    return out


MODEL_SHOTS = {
    "shots": [
        {
            "shot_id": "ep_01_sh001", "scene_id": "loc_rooftop", "order": 0,
            "first_frame_prompt": "夜间天台，雨中，女子背对镜头站在护栏边",
            "motion_prompt": "缓慢推进",
            "shot_size": "MS", "camera_angle": "eye_level",
            "camera_move": "push_in", "camera_id": "cam_a",
            "duration_s": 5.0,
            "characters": [{"char_id": "c_lin_wan", "expression": "落寞",
                            "action": "扶着护栏", "face_pose": "back"}],
            "dialogue": [],
            "transition_in": "cut", "transition_dur_s": 0.0,
            "beat": "开场",
        },
        {
            # 故意不填 order 和 location_id，考 setdefault 和 link_location
            "shot_id": "ep_01_sh002", "scene_id": "loc_rooftop",
            "first_frame_prompt": "近景，女子转身，雨水顺着脸颊",
            "shot_size": "CU", "camera_angle": "eye_level",
            "duration_s": 3.0,
            # 说话人没进 characters，考 _add_missing_speakers
            "characters": [],
            "dialogue": [{"char_id": "c_lin_wan", "text": "你说过会来的"}],
            "transition_in": "cut",
        },
        {
            # duration_s 不在档位上，要被吸附
            "shot_id": "ep_01_sh003", "scene_id": "loc_rooftop", "order": 2,
            "first_frame_prompt": "男子从阴影里走出，手里攥着伞",
            "shot_size": "MCU", "camera_angle": "eye_level",
            "duration_s": 4.4,
            "characters": [{"char_id": "c_chen_mo", "expression": "克制",
                            "action": "握伞", "face_pose": "front"}],
            "dialogue": [{"char_id": "c_chen_mo", "text": "我来了。晚了七年。"}],
            # 场景切换用 dissolve 但忘了填时长，要补 0.4
            "transition_in": "dissolve",
        },
    ]
}


def parses() -> list[dict]:
    a = assets()
    gen = StoryboardGenerator.__new__(StoryboardGenerator)
    raw = json.dumps(MODEL_SHOTS, ensure_ascii=False)
    shots = gen._parse(raw, a)
    out = [{
        "name": "正常一集",
        "raw": raw,
        "shots": [json.loads(s.model_dump_json()) for s in shots],
    }]
    # 顶层直接是数组也要认
    raw2 = json.dumps(MODEL_SHOTS["shots"], ensure_ascii=False)
    shots2 = gen._parse(raw2, a)
    out.append({
        "name": "顶层是数组",
        "raw": raw2,
        "shots": [json.loads(s.model_dump_json()) for s in shots2],
    })
    return out


def parse_failures() -> list[dict]:
    a = assets()
    gen = StoryboardGenerator.__new__(StoryboardGenerator)
    cases = [
        ("空数组", json.dumps({"shots": []})),
        ("不是列表", json.dumps({"shots": {"a": 1}})),
        ("引用了没注册的角色", json.dumps({"shots": [{
            "shot_id": "ep_01_sh001", "scene_id": "s", "order": 0,
            "first_frame_prompt": "x", "shot_size": "MS",
            "duration_s": 5.0,
            "characters": [{"char_id": "c_nobody", "expression": "x",
                            "action": "x", "face_pose": "front"}],
            "dialogue": [],
        }]}, ensure_ascii=False)),
        ("根本不是 JSON", "抱歉，我做不到。"),
    ]
    out = []
    for name, raw in cases:
        try:
            gen._parse(raw, a)
            out.append({"name": name, "raw": raw, "raises": False,
                        "python_error": None})
        except Exception as exc:                    # noqa: BLE001
            out.append({"name": name, "raw": raw, "raises": True,
                        "python_error": type(exc).__name__})
    return out


def coverages() -> list[dict]:
    a = assets()
    gen = StoryboardGenerator.__new__(StoryboardGenerator)
    full = gen._parse(json.dumps(MODEL_SHOTS, ensure_ascii=False), a)

    # 一句台词都没有的版本
    mute = json.loads(json.dumps(MODEL_SHOTS))
    for s in mute["shots"]:
        s["dialogue"] = []
    mute_shots = gen._parse(json.dumps(mute, ensure_ascii=False), a)

    # 一个角色都没有的版本（也就没有台词了）
    empty = json.loads(json.dumps(MODEL_SHOTS))
    for s in empty["shots"]:
        s["dialogue"] = []
        s["characters"] = []
    empty_shots = gen._parse(json.dumps(empty, ensure_ascii=False), a)

    cases = [
        ("有台词有角色", SCRIPT, full),
        ("剧本有对白但分镜没台词", SCRIPT, mute_shots),
        ("剧本没有对白", "全是画面描述，没有冒号后面跟内容", full),
        ("一个角色都没有", SCRIPT, empty_shots),
        ("空分镜表", SCRIPT, []),
    ]
    out = []
    for name, script, shots in cases:
        out.append({
            "name": name, "script": script,
            "shots": [json.loads(s.model_dump_json()) for s in shots],
            "problems": StoryboardGenerator.check_coverage(script, shots),
        })
    return out


def rebalances() -> list[dict]:
    a = assets()
    gen = StoryboardGenerator.__new__(StoryboardGenerator)
    out = []
    for target, tol in [(12.0, 3.0), (30.0, 3.0), (5.0, 3.0),
                        (13.0, 0.5), (13.0, 100.0)]:
        shots = gen._parse(json.dumps(MODEL_SHOTS, ensure_ascii=False), a)
        before = [s.duration_s for s in shots]
        rebalance_durations(shots, target, tol)
        out.append({
            "target_s": target, "tolerance_s": tol,
            "before": before,
            "after": [s.duration_s for s in shots],
        })
    return out


def main() -> None:
    data = {
        "note": "由 cpp/tests/export_storyboard_golden.py 生成，不要手改",
        "durations": durations(),
        "quotas": quotas(),
        "prompts": prompts(),
        "schemas": schemas(),
        "links": links(),
        "speakers": speakers(),
        "parses": parses(),
        "parse_failures": parse_failures(),
        "coverages": coverages(),
        "rebalances": rebalances(),
    }
    target = HERE.parent / "golden" / "stage_storyboard.json"
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
