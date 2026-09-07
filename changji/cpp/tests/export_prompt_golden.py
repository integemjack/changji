"""导出提示词组装的对拍语料。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_prompt_golden.py

产出 cpp/tests/golden/prompt_compose.json。

这是阶段 5 里唯一算契约的东西：出图后端换成什么都无所谓，
但**拼给模型的那串字必须和 Python 逐字节一致**，
否则同一个项目在两个后端上出的画面不一样。
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.models.character import (                              # noqa: E402
    AppearanceBlock, AssetLibrary, Character, Location, StyleLine,
    StyleProfile, WardrobeVariant,
)
from changji.models.shot import (                                   # noqa: E402
    CameraAngle, CameraMove, CharacterInShot, FacePose, Shot, ShotSize,
)
from changji.stages.render import (                                 # noqa: E402
    _ANGLE_ZH, _MOVE_ZH, _SHOT_SIZE_ZH, PromptComposer,
)


def assets(style_line: StyleLine) -> AssetLibrary:
    lin = Character(
        char_id="c_lin_wan", name="林晚",
        appearance=AppearanceBlock(
            identity="二十七岁女性，外表冷静",
            body="偏瘦，中等身高",
            face="黑色长直发，鹅蛋脸，杏眼",
            attire="白色衬衫，深色西装裤"),
        ref_front="refs/c_lin_wan_front.png",
        ref_three_quarter="refs/c_lin_wan_three_quarter.png",
    )
    # 带换装状态的角色，验"只替换 attire 那一层，其余逐字节不变"
    lin.wardrobe.append(WardrobeVariant(wardrobe_id="雨中", description="湿透的白衬衫"))

    chen = Character(
        char_id="c_chen_mo", name="陈默",
        appearance=AppearanceBlock(
            identity="三十出头男性", body="高瘦",
            face="短寸黑发，方脸", attire="黑色风衣"),
    )
    # 没有任何参考图的角色
    plain = Character(
        char_id="c_plain", name="路人",
        appearance=AppearanceBlock(identity="中年男性", face="圆脸", attire="工装"),
    )

    rooftop = Location(
        location_id="loc_rooftop", name="夜间天台",
        space="水泥地面，锈蚀护栏", lighting="夜间冷调顶光", palette="冷蓝",
        ref_empty="refs/loc_rooftop_empty.png")
    alley = Location(
        location_id="loc_alley", name="小巷",
        space="狭窄砖墙", lighting="路灯昏黄")

    return AssetLibrary(
        characters={"c_lin_wan": lin, "c_chen_mo": chen, "c_plain": plain},
        locations={"loc_rooftop": rooftop, "loc_alley": alley},
        style=StyleProfile(
            style_line=style_line,
            global_style="电影感，浅景深，冷色调",
            negative_prompt="低质量，模糊",
            aspect_ratio="9:16"),
    )


def shot(**kw) -> Shot:
    base = dict(
        shot_id="ep01_sh001", scene_id="sc01", order=0,
        first_frame_prompt="雨夜天台，女子背对镜头",
        shot_size=ShotSize.MS, camera_angle=CameraAngle.EYE_LEVEL,
        camera_move=CameraMove.STATIC, duration_s=5.0,
        characters=[], dialogue=[],
    )
    base.update(kw)
    return Shot(**base)


def cis(char_id, **kw) -> CharacterInShot:
    base = dict(char_id=char_id, expression="", action="",
                face_pose=FacePose.FRONT, wardrobe_state="default")
    base.update(kw)
    return CharacterInShot(**base)


def cases() -> list[dict]:
    out = []

    def add(name, s, line=StyleLine.REALISTIC, lib=None):
        a = lib or assets(line)
        composer = PromptComposer(a)
        try:
            b = composer.compose(s)
            entry = {
                "name": name, "style_line": line.value,
                "shot": json.loads(s.model_dump_json()),
                "ok": True,
                "positive": b.positive,
                "negative": b.negative,
                "reference_images": b.reference_images,
                "motion": composer.motion_prompt(s),
            }
        except Exception as exc:                          # noqa: BLE001
            entry = {"name": name, "style_line": line.value,
                     "shot": json.loads(s.model_dump_json()),
                     "ok": False, "error_type": type(exc).__name__}
        out.append(entry)

    # ---- 基本 ----
    add("空镜：没有角色没有场景", shot())
    add("一个角色", shot(characters=[cis("c_lin_wan", expression="落寞",
                                         action="扶着护栏")]))
    add("两个角色", shot(characters=[
        cis("c_lin_wan", expression="落寞", action="扶着护栏"),
        cis("c_chen_mo", expression="克制", action="握伞",
            face_pose=FacePose.THREE_QUARTER)]))
    add("带场景", shot(location_id="loc_rooftop",
                       characters=[cis("c_lin_wan")]))
    add("场景没有空景图", shot(location_id="loc_alley"))
    add("角色没有参考图", shot(characters=[cis("c_plain")]))

    # ---- 换装 ----
    add("换装状态", shot(characters=[cis("c_lin_wan", wardrobe_state="雨中")]))
    add("换装状态不存在时退回默认",
        shot(characters=[cis("c_lin_wan", wardrobe_state="没这个状态")]))

    # ---- 面部朝向决定用哪张参考图 ----
    for pose in FacePose:
        add(f"面向 {pose.value}",
            shot(characters=[cis("c_lin_wan", face_pose=pose)]))

    # ---- 景别和机位的全部取值 ----
    for size in ShotSize:
        add(f"景别 {size.value}", shot(shot_size=size))
    for angle in CameraAngle:
        add(f"机位 {angle.value}", shot(camera_angle=angle))
    for move in CameraMove:
        add(f"运镜 {move.value}",
            shot(camera_move=move, motion_prompt="雨丝斜掠",
                 characters=[cis("c_lin_wan", action="转身")]))

    # ---- 空字段 ----
    add("没有首帧描述", shot(first_frame_prompt=""))
    add("镜头级负向提示词", shot(negative_prompt="不要有手"))
    add("表情和动作都空", shot(characters=[cis("c_lin_wan")]))

    # ---- 全剧风格为空 ----
    empty_style = assets(StyleLine.REALISTIC)
    empty_style.style.global_style = ""
    empty_style.style.negative_prompt = ""
    add("全剧风格为空", shot(characters=[cis("c_lin_wan")]), lib=empty_style)

    # ---- 动漫线：分隔符不一样 ----
    for name, s in [
        ("动漫线：一个角色", shot(characters=[cis("c_lin_wan", expression="落寞")])),
        ("动漫线：带场景", shot(location_id="loc_rooftop",
                                characters=[cis("c_chen_mo")])),
        ("动漫线：运镜", shot(camera_move=CameraMove.PUSH_IN,
                              motion_prompt="缓慢推近")),
    ]:
        add(name, s, StyleLine.ANIME)

    # ---- 该报错的 ----
    add("引用了未注册角色", shot(characters=[cis("c_nobody")]))
    add("引用了未注册场景", shot(location_id="loc_nobody"))

    return out


def main() -> None:
    data = {
        "note": "由 cpp/tests/export_prompt_golden.py 生成，不要手改",
        # 三张中文标签表也导出来。表里少一项的话那一层拼出来是空的，
        # 而空的景别层意味着模型自己决定构图——同一集里景别会乱跳。
        "shot_size_zh": _SHOT_SIZE_ZH,
        "angle_zh": _ANGLE_ZH,
        "move_zh": _MOVE_ZH,
        "cases": cases(),
    }
    target = HERE.parent / "golden" / "prompt_compose.json"
    with io.open(target, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("写好了", target, "（%d 条用例）" % len(data["cases"]))


if __name__ == "__main__":
    main()
