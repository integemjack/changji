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


# ══════════════════════════════════════════════════════════════════════
# character.py 的语料
# ══════════════════════════════════════════════════════════════════════

from changji.models.character import (  # noqa: E402
    AppearanceBlock,
    AssetLibrary,
    Character,
    Location,
    StyleLine,
    StyleProfile,
    WardrobeVariant,
    guess_gender,
    pick_voice,
)

# ── 渲染输出的逐字节对拍 ────────────────────────────────────────────────
#
# 方案第六节：接口契约只要求结构兼容，但**提示词拼接必须逐字节相同**。
# 这一段的输出直接进提示词，差一个标点画面重心就变。

render_cases = []


def render_case(name: str, block: AppearanceBlock) -> None:
    render_cases.append({
        "name": name,
        "block": block.model_dump(mode="json"),
        "realistic": block.render(StyleLine.REALISTIC),
        "anime": block.render(StyleLine.ANIME),
    })


render_case("五段齐全", AppearanceBlock(
    identity="三十岁上下的男性，沉默寡言",
    body="偏瘦，肩背挺直",
    face="单眼皮，短碎发，眼下有一道旧疤",
    attire="深灰西装，衬衫领口松着",
    style="低饱和，胶片颗粒",
))

# 尾部标点是这个函数存在的理由：手写设定常带句号，
# 不剥的话拼出来是「冷静克制。，身姿笔挺。，」
render_case("各段都带尾部标点", AppearanceBlock(
    identity="冷静克制。",
    body="身姿笔挺。",
    face="浓眉，鹰钩鼻；",
    attire="黑色风衣，",
    style="硬朗光影、",
))

render_case("中间段为空", AppearanceBlock(
    identity="少女",
    body="",
    face="双马尾，琥珀色瞳",
    attire="水手服",
    style="",
))

render_case("只有必填段", AppearanceBlock(
    identity="老者", face="白须及胸", attire="灰布长衫",
))

render_case("带空白和标点混合", AppearanceBlock(
    identity="  中年女性，干练  ",
    body="  ",
    face="盘发，细框眼镜。。",
    attire="米色套装、、",
    style=" 冷调 ",
))

dump("appearance_render", render_cases)


# ── Character 的完整渲染 ────────────────────────────────────────────────

lin = Character(
    char_id="c_lin_yuan",
    name="林渊",
    appearance=AppearanceBlock(
        identity="三十岁上下的男性，沉默寡言。",
        body="偏瘦，肩背挺直",
        face="单眼皮，短碎发，眼下有一道旧疤",
        attire="深灰西装，衬衫领口松着",
        style="低饱和，胶片颗粒",
    ),
    wardrobe=[
        WardrobeVariant(wardrobe_id="suit_soaked", description="西装被雨淋透，紧贴身体"),
        WardrobeVariant(wardrobe_id="casual", description="灰色卫衣，牛仔裤"),
    ],
    ref_front="refs/c_lin_yuan_front.png",
    ref_three_quarter="refs/c_lin_yuan_tq.png",
    ref_back=None,
    lora_trigger="linyuan_v3",
    lora_strength=0.85,
    voice_gender="male",
    voice_order=1,
)

char_cases = {
    "character": lin.model_dump(mode="json"),
    "render": {
        "realistic_default": lin.render_prompt(StyleLine.REALISTIC),
        "realistic_soaked": lin.render_prompt(StyleLine.REALISTIC, "suit_soaked"),
        "anime_default": lin.render_prompt(StyleLine.ANIME),
        "anime_casual": lin.render_prompt(StyleLine.ANIME, "casual"),
        "unknown_wardrobe": lin.render_prompt(StyleLine.REALISTIC, "not_exist"),
    },
    "wardrobe_desc": {
        "default": lin.wardrobe_desc("default"),
        "suit_soaked": lin.wardrobe_desc("suit_soaked"),
        "not_exist": lin.wardrobe_desc("not_exist"),
        "empty": lin.wardrobe_desc(""),
    },
    # ref_back 是 None，测退级链
    "ref_for_pose": {
        p: lin.ref_for_pose(p)
        for p in ("front", "three_quarter", "profile", "back", "off_screen", "乱填")
    },
}
dump("character_render", char_cases)


# ── Location 与资产库 ───────────────────────────────────────────────────

rooftop = Location(
    location_id="loc_rooftop",
    name="天台",
    space="老式写字楼顶层，水泥地面，一圈锈蚀铁栏杆",
    lighting="冷调顶光，远处霓虹反光",
    palette="青灰为主，点缀品红",
    ref_empty="refs/loc_rooftop_empty.png",
)
alley = Location(
    location_id="loc_alley",
    name="小巷",
    space="两侧是斑驳砖墙的窄巷",
    lighting="暖调侧逆光",
    palette="",
)

lib = AssetLibrary(
    characters={"c_lin_yuan": lin},
    locations={"loc_rooftop": rooftop, "loc_alley": alley},
    style=StyleProfile(
        style_line=StyleLine.REALISTIC,
        global_style="写实电影感，浅景深",
        negative_prompt="模糊，低质量",
        aspect_ratio="9:16",
    ),
)

dump("asset_library", {
    "library": lib.model_dump(mode="json"),
    "character_ids": lib.character_ids(),
    "location_ids": lib.location_ids(),
    "location_render": {
        "rooftop_realistic": rooftop.render_prompt(StyleLine.REALISTIC),
        "rooftop_anime": rooftop.render_prompt(StyleLine.ANIME),
        "alley_realistic": alley.render_prompt(StyleLine.REALISTIC),
    },
    "validate_references": {
        "all_known": lib.validate_references({"c_lin_yuan"}, {"loc_rooftop"}),
        "missing_both": lib.validate_references(
            {"c_lin_yuan", "c_chen_mo"}, {"loc_alley", "loc_office"}),
    },
})


# ── 性别猜测与音色挑选 ──────────────────────────────────────────────────

gender_cases = [
    "一位年轻的女性，短发",
    "中年男人，络腮胡",
    "他是这家店的老板",
    "她的妈妈",
    "一个孩子",
    "",
    "父女二人",          # 男女词同时出现，应返回空串
]
dump("guess_gender", [{"identity": g, "expected": guess_gender(g)} for g in gender_cases])

voice_pool = [
    "none", "en_female_belinda.wav", "zh_male_chadwick.wav",
    "zh_female_sophie.wav", "en_male_freeman.wav", "narrator.wav",
]
voice_cases = []
for g in ("female", "male", ""):
    for idx in (0, 1, 2):
        voice_cases.append({
            "gender": g, "index": idx,
            "expected": pick_voice(voice_pool, g, idx),
        })
voice_cases.append({"gender": "female", "index": 0, "pool": [],
                    "expected": pick_voice([], "female", 0)})
voice_cases.append({"gender": "male", "index": 0, "pool": ["none"],
                    "expected": pick_voice(["none"], "male", 0)})
dump("pick_voice", {"pool": voice_pool, "cases": voice_cases})

print("\ncharacter 语料也写好了")


# ══════════════════════════════════════════════════════════════════════
# project.py 的语料：真的建一个项目目录出来
# ══════════════════════════════════════════════════════════════════════
#
# 阶段 1 的完成标志是"能读 Python 写的项目文件（含中文路径和中文内容），
# 字段全部对得上"。所以这里不导出 JSON 片段，而是**真的用 ProjectStore
# 建一个目录**，目录名带中文，让 C++ 侧去读它。
#
# 中文目录名是刻意的：MSVC 上 fs::path(std::string) 会按 ANSI 代码页解释
# 窄字符串，而项目里的 std::string 一律是 UTF-8，撞上就抛异常。
# 这个坑在 verify/RESULTS.md 里记过，这里用语料把它钉死。

import shutil  # noqa: E402

from changji.models.project import (  # noqa: E402
    Episode,
    Project,
    ProjectStore,
)

PROJ_ROOT = OUT / "项目_雨夜天台"
if PROJ_ROOT.exists():
    shutil.rmtree(PROJ_ROOT)

store = ProjectStore.create(PROJ_ROOT, project_id="yuye-tiantai",
                            title="雨夜天台", style_line=StyleLine.REALISTIC)

project = store.load_project()
project.premise = "一个关于错过与和解的都市短剧。每集三分钟，竖屏。"
project.episodes = [
    Episode(
        episode_id="ep01",
        title="第一集 · 雨",
        synopsis="林渊在天台等一个不会来的人。",
        target_duration_s=180.0,
        script="（雨声起）\n林渊站在栏杆前，没有回头。",
        shots=[full, minimal],   # 复用前面那两个镜头，order 分别是 6 和 0
    ),
    Episode(
        episode_id="ep02",
        title="第二集 · 晴",
        synopsis="三年后，同一个天台。",
        target_duration_s=200.0,
    ),
]
store.save_project(project)
store.save_assets(lib)

ep01 = project.episode_by_id("ep01")
dump("project_expectations", {
    "root_name": PROJ_ROOT.name,
    "project_id": project.project_id,
    "title": project.title,
    "premise": project.premise,
    "schema_version": project.schema_version,
    "episode_ids": [e.episode_id for e in project.episodes],
    "ep01": {
        "sorted_shot_ids": [s.shot_id for s in ep01.sorted_shots()],
        "planned_duration_s": ep01.planned_duration_s(),
        "counts_by_status": ep01.counts_by_status(),
        "shots_needing_planned": [s.shot_id for s in ep01.shots_needing(ShotStatus.PLANNED)],
        "shots_needing_audio_done": [
            s.shot_id for s in ep01.shots_needing(ShotStatus.AUDIO_DONE)],
    },
    "subdirs": sorted(p.name for p in PROJ_ROOT.iterdir() if p.is_dir()),
    # rel() 的正斜杠约定：Windows 上存的项目拿到 Linux 也要能读
    "rel_of_frames_file": store.paths.rel(PROJ_ROOT / "frames" / "a.png"),
    "rel_of_nested": store.paths.rel(PROJ_ROOT / "shots" / "draft" / "b.mp4"),
})

print(f"\n项目目录建在 {PROJ_ROOT}")
