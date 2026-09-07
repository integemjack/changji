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


# ══════════════════════════════════════════════════════════════════════
# hardware.py 的语料
# ══════════════════════════════════════════════════════════════════════

from changji.hardware import (  # noqa: E402
    HardwareProfile,
    Tier,
    TierSpec,
    _round32,
    tiers_for_vram,
)

# ── _round32 的银行家舍入 ───────────────────────────────────────────────
#
# Python 的 round() 是四舍六入五取偶，C++ 的 std::round 是四舍五入远离零。
# 恰好落在 .5 上时两者不同：round(12.5) Python 给 12、std::round 给 13。
# 这里把所有半整数情形都列出来钉死。
round32_cases = []
for n in (0, 1, 16, 17, 31, 32, 33,
          400, 416, 432, 448, 464, 480,   # 400/32=12.5, 432/32=13.5, 464/32=14.5
          352, 288, 256, 544, 704, 1088,
          -5, 15, 47, 48, 49):
    round32_cases.append({"n": n, "expected": _round32(n)})
dump("round32", round32_cases)


# ── 各档显存推导出的档位参数 ────────────────────────────────────────────

vram_cases = []
for vram in (4.0, 7.5, 8.0, 11.5, 12.0, 15.5, 15.9, 16.0, 23.5, 24.0, 48.0):
    tiers = tiers_for_vram(vram)
    vram_cases.append({
        "vram_gb": vram,
        "tiers": {
            t.value: {
                "width": s.width, "height": s.height, "steps": s.steps,
                "measured_seconds": s.measured_seconds,
            }
            for t, s in tiers.items()
        },
    })
dump("tiers_for_vram", vram_cases)


# ── scaled_to 的三种画幅 ────────────────────────────────────────────────

scale_cases = []
for w, h, steps in ((1280, 704, 30), (640, 352, 10), (768, 432, 18), (960, 544, 20)):
    spec = TierSpec(tier=Tier.FINAL, width=w, height=h, steps=steps)
    scale_cases.append({
        "input": {"width": w, "height": h, "steps": steps},
        "9:16": {"width": spec.scaled_to("9:16").width,
                 "height": spec.scaled_to("9:16").height},
        "16:9": {"width": spec.scaled_to("16:9").width,
                 "height": spec.scaled_to("16:9").height},
        "1:1": {"width": spec.scaled_to("1:1").width,
                "height": spec.scaled_to("1:1").height},
    })
dump("tier_scaled_to", scale_cases)


# ── describe() 的逐字节输出 ─────────────────────────────────────────────
#
# 这段直接打给用户看，格式对不上会很显眼。

describe_cases = []
for vram, has_gpu in ((15.9, True), (12.0, False), (6.0, True), (24.0, False)):
    prof = HardwareProfile(
        gpu=None, vram_gb=vram, tiers=tiers_for_vram(vram), detected=has_gpu)
    describe_cases.append({
        "vram_gb": vram, "detected": has_gpu, "gpu": None,
        "describe": prof.describe(),
        "estimate_episode_20_final": prof.estimate_episode(20, Tier.FINAL),
    })

# 带显卡信息的那一支
from changji.hardware import GPUInfo  # noqa: E402
gpu = GPUInfo(name="NVIDIA GeForce RTX 2060", vram_mb=6144, driver="610.88")
prof_gpu = HardwareProfile(gpu=gpu, vram_gb=gpu.vram_gb,
                           tiers=tiers_for_vram(gpu.vram_gb), detected=True)
describe_cases.append({
    "vram_gb": gpu.vram_gb, "detected": True,
    "gpu": {"name": gpu.name, "vram_mb": gpu.vram_mb, "driver": gpu.driver},
    "describe": prof_gpu.describe(),
    "estimate_episode_20_final": prof_gpu.estimate_episode(20, Tier.FINAL),
})
dump("hardware_describe", describe_cases)

print("\nhardware 语料也写好了")


# ══════════════════════════════════════════════════════════════════════
# 阶段 2 的只读接口：直接调真实路由拿响应
# ══════════════════════════════════════════════════════════════════════
#
# 不在这里照着路由代码重拼一遍 dict——那样测的是写脚本的人对代码的理解。
# 起一个 FastAPI TestClient 打真实接口，拿到什么存什么。
#
# vram_gb_override 钉死，否则语料会跟着这台机器的显卡走，
# 换台机器跑测试就红。C++ 侧用同一个值。

from fastapi.testclient import TestClient  # noqa: E402

from changji.config import Settings  # noqa: E402
from changji.web.server import create_app  # noqa: E402

PINNED_VRAM = 15.9   # 按 16GB 卡的实际上报值，落在 15.5 那一档

settings = Settings.model_validate({"vram_gb_override": PINNED_VRAM})
client = TestClient(create_app(settings))

proj_arg = str(PROJ_ROOT)

endpoint_cases = [
    {"name": "hardware", "url": "/api/hardware", "params": {}},
    {"name": "settings", "url": "/api/settings", "params": {}},
    {"name": "project", "url": "/api/project", "params": {"path": proj_arg}},
    {"name": "shots_ep01", "url": "/api/shots",
     "params": {"path": proj_arg, "episode_id": "ep01"}},
    {"name": "shots_ep02_empty", "url": "/api/shots",
     "params": {"path": proj_arg, "episode_id": "ep02"}},
    {"name": "assets", "url": "/api/assets", "params": {"path": proj_arg}},
    # 错误分支：契约里状态码和 {"detail": ...} 的形状都要对上
    {"name": "shots_missing_episode", "url": "/api/shots",
     "params": {"path": proj_arg, "episode_id": "ep99"}},
    {"name": "project_no_path", "url": "/api/project", "params": {"path": ""}},
    {"name": "project_not_a_project", "url": "/api/project",
     "params": {"path": str(OUT)}},
]

results = []
for case in endpoint_cases:
    resp = client.get(case["url"], params=case["params"])
    results.append({
        "name": case["name"],
        "url": case["url"],
        "params": case["params"],
        "status": resp.status_code,
        "body": resp.json(),
    })
    print(f"  {case['url']:18s} {case['params'].get('episode_id', ''):8s} "
          f"→ {resp.status_code}")

dump("endpoints_readonly", {
    "pinned_vram_gb": PINNED_VRAM,
    "project_path": proj_arg,
    "cases": results,
})

print("\n只读接口语料写好了")


# ══════════════════════════════════════════════════════════════════════
# 阶段 3：编辑接口
# ══════════════════════════════════════════════════════════════════════
#
# 编辑会写盘，所以每个用例跑在**一份全新的项目副本**上，
# 否则前一个用例的改动会污染后一个。
# 语料同时记下响应和改完之后的镜头状态，两边都要对上。

import tempfile  # noqa: E402

EDIT_ROOT = OUT / "编辑用例"
if EDIT_ROOT.exists():
    shutil.rmtree(EDIT_ROOT)
EDIT_ROOT.mkdir(parents=True)

edit_cases = [
    {"name": "改提示词_退回未开工",
     "patch": {"first_frame_prompt": "改过的提示词"}},
    {"name": "改景别_退回未开工",
     "patch": {"shot_size": "CU"}},
    {"name": "改字幕_不退回",
     "patch": {"subtitle_text": "换一句字幕"}},
    {"name": "改叙事功能_不退回",
     "patch": {"beat": "铺垫"}},
    {"name": "显式给了状态_即使改画面也不退回",
     "patch": {"first_frame_prompt": "又改了", "status": "locked"}},
    {"name": "改台词_清配音并解锁时长",
     "patch": {"dialogue_texts": ["换了的第一句。", "雨声盖过了他后半句。"]}},
    {"name": "台词条数对不上_400",
     "patch": {"dialogue_texts": ["只给一条"]}},
    {"name": "不认识的字段_400",
     "patch": {"没这个字段": "x"}},
    {"name": "枚举取值非法_400",
     "patch": {"shot_size": "XXL"}},
    {"name": "时长超上限_400",
     "patch": {"duration_s": 99}},
    {"name": "硬切给了转场时长_400",
     "patch": {"transition_in": "cut", "transition_dur_s": 0.5}},
    {"name": "改运镜_退回未开工",
     "patch": {"camera_move": "orbit"}},
]

edit_results = []
for i, case in enumerate(edit_cases):
    root = EDIT_ROOT / f"c{i:02d}"
    shutil.copytree(PROJ_ROOT, root)
    body = {"project": str(root), "episode_id": "ep01",
            "shot_id": "ep01_s03_sh007", "patch": case["patch"]}
    resp = client.post("/api/shot", json=body)

    after = None
    if resp.status_code == 200:
        st = ProjectStore(root)
        sh = st.load_project().episode_by_id("ep01").shot_by_id("ep01_s03_sh007")
        after = sh.model_dump(mode="json")

    # pydantic 生成的报错文字不参与对拍。
    #
    # 那串东西里嵌着 pydantic 的版本号和文档 URL（errors.pydantic.dev/2.13/...），
    # 在 C++ 里复刻既荒唐又会随上游版本腐烂；而且前端只是把这个字符串显示
    # 出来，不解析它。所以这类用例只比状态码和 body 的形状，不比 detail 的文字。
    body = resp.json()
    detail = body.get("detail") if isinstance(body, dict) else None
    msg_is_pydantic = isinstance(detail, str) and "validation error for" in detail

    edit_results.append({
        "name": case["name"],
        "dir": root.name,
        "patch": case["patch"],
        "status": resp.status_code,
        "body": body,
        # full = body 逐字段深比较；shape = 只比状态码和 detail 是个非空字符串
        "compare": "shape" if msg_is_pydantic else "full",
        "shot_after": after,
    })
    print(f"  {case['name']:32s} → {resp.status_code}")

# 剧集和镜头不存在的两条，不需要副本（不会写盘）
for name, ep_id, sh_id in (("剧集不存在_404", "ep99", "ep01_s03_sh007"),
                           ("镜头不存在_404", "ep01", "sh_no_such")):
    resp = client.post("/api/shot", json={
        "project": str(PROJ_ROOT), "episode_id": ep_id, "shot_id": sh_id,
        "patch": {"beat": "x"}})
    edit_results.append({"name": name, "dir": None,
                         "episode_id": ep_id, "shot_id": sh_id,
                         "patch": {"beat": "x"},
                         "status": resp.status_code, "body": resp.json(),
                         "compare": "full", "shot_after": None})
    print(f"  {name:32s} → {resp.status_code}")

# 副本本身不用留：shot_after 已经抽进 JSON 了，
# C++ 测试会自己从原始项目复制。留着的话是 12 份项目进版本库。
shutil.rmtree(EDIT_ROOT, ignore_errors=True)

dump("endpoints_shot_edit", {"cases": edit_results})
print("\n编辑接口语料写好了")


# ══════════════════════════════════════════════════════════════════════
# 阶段 3：资产类编辑接口
# ══════════════════════════════════════════════════════════════════════
#
# 这三个改的是全剧共用的东西，会连带把未锁定的镜头退回未开工。
# 所以语料除了响应还要记「重置了几个镜头」和「镜头状态变成什么」。

ASSET_EDIT_ROOT = OUT / "资产编辑用例"
if ASSET_EDIT_ROOT.exists():
    shutil.rmtree(ASSET_EDIT_ROOT)
ASSET_EDIT_ROOT.mkdir(parents=True)

asset_cases = [
    {"n": "改外观_触发重跑", "url": "/api/character",
     "extra": {"char_id": "c_lin_yuan"},
     "patch": {"face": "改成了单眼皮，长发"}},
    {"n": "改名字_不触发", "url": "/api/character",
     "extra": {"char_id": "c_lin_yuan"},
     "patch": {"name": "林渊（改名）"}},
    {"n": "外观提交同样的值_不触发", "url": "/api/character",
     "extra": {"char_id": "c_lin_yuan"},
     "patch": {"face": "单眼皮，短碎发，眼下有一道旧疤"}},
    {"n": "改外观但关掉重置", "url": "/api/character",
     "extra": {"char_id": "c_lin_yuan"}, "reset_shots": False,
     "patch": {"attire": "换了件风衣"}},
    {"n": "角色不存在_404", "url": "/api/character",
     "extra": {"char_id": "c_no_such"}, "patch": {"name": "x"}},
    {"n": "角色多余字段_422", "url": "/api/character",
     "extra": {"char_id": "c_lin_yuan"}, "patch": {"没这个": 1}},
    {"n": "改场景光线_触发重跑", "url": "/api/location",
     "extra": {"location_id": "loc_rooftop"},
     "patch": {"lighting": "改成暖调"}},
    {"n": "改场景名字_不触发", "url": "/api/location",
     "extra": {"location_id": "loc_rooftop"},
     "patch": {"name": "天台（改）"}},
    {"n": "场景不存在_404", "url": "/api/location",
     "extra": {"location_id": "loc_no_such"}, "patch": {"name": "x"}},
    {"n": "改全剧画风_触发重跑", "url": "/api/style", "extra": {},
     "patch": {"global_style": "换成冷峻写实"}},
    {"n": "改画幅_触发重跑", "url": "/api/style", "extra": {},
     "patch": {"aspect_ratio": "16:9"}},
    {"n": "风格提交同样的值_不触发", "url": "/api/style", "extra": {},
     "patch": {"global_style": "写实电影感，浅景深"}},
]

asset_results = []
for i, case in enumerate(asset_cases):
    root = ASSET_EDIT_ROOT / f"a{i:02d}"
    shutil.copytree(PROJ_ROOT, root)
    body = {"project": str(root), "patch": case["patch"]}
    body.update(case["extra"])
    if "reset_shots" in case:
        body["reset_shots"] = case["reset_shots"]
    resp = client.post(case["url"], json=body)

    after_assets, shot_status = None, None
    if resp.status_code == 200:
        st = ProjectStore(root)
        a = st.load_assets()
        after_assets = a.model_dump(mode="json")
        sh = st.load_project().episode_by_id("ep01").shot_by_id("ep01_s03_sh007")
        shot_status = sh.status.value

    b = resp.json()
    d = b.get("detail") if isinstance(b, dict) else None
    shape_only = isinstance(d, str) and "validation error for" in d

    asset_results.append({
        "name": case["n"], "url": case["url"],
        "extra": case["extra"], "patch": case["patch"],
        "reset_shots": case.get("reset_shots"),
        "status": resp.status_code, "body": b,
        "compare": "shape" if shape_only else "full",
        "assets_after": after_assets,
        "shot_status_after": shot_status,
    })
    print(f"  {case['n']:26s} {case['url']:16s} -> {resp.status_code}")

shutil.rmtree(ASSET_EDIT_ROOT, ignore_errors=True)
dump("endpoints_asset_edit", {"cases": asset_results})
print("\n资产编辑语料写好了")
