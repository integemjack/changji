"""分镜表：整套系统的中枢数据结构。

设计上最重要的一条：这里没有任何描述角色长相、发型、服装的字段。

保持角色跨镜头一致的常见做法是在提示词里反复强调，那不可靠。这里的做法是让
大模型在结构上就没有写错的机会：它只能填 char_id 和本镜可变项，外观描述在
渲染时由程序从角色资产库机械拼接，逐字节相同。

同一份定义既用于运行时校验，也用于导出 JSON Schema 约束大模型输出。
"""

from __future__ import annotations

from enum import Enum

from pydantic import BaseModel, Field, model_validator


class ShotSize(str, Enum):
    """景别。取值顺序由近到远。"""

    ECU = "ECU"  # 大特写
    CU = "CU"  # 特写
    MCU = "MCU"  # 近景
    MS = "MS"  # 中景
    MLS = "MLS"  # 中远景
    LS = "LS"  # 远景
    ELS = "ELS"  # 大远景


# 能看清嘴部动作的景别。口型判定用，不要改成靠模型判断。
LIPSYNC_CAPABLE_SIZES = frozenset({ShotSize.ECU, ShotSize.CU, ShotSize.MCU, ShotSize.MS})


class CameraAngle(str, Enum):
    LOW = "low"  # 仰拍
    EYE_LEVEL = "eye_level"  # 平视
    HIGH = "high"  # 俯拍
    OVERHEAD = "overhead"  # 顶拍
    DUTCH = "dutch"  # 斜角


# 俯拍和顶拍看不清嘴，排除在口型之外。
LIPSYNC_CAPABLE_ANGLES = frozenset(
    {CameraAngle.LOW, CameraAngle.EYE_LEVEL, CameraAngle.HIGH, CameraAngle.DUTCH}
)


class CameraMove(str, Enum):
    STATIC = "static"
    PAN_LEFT = "pan_left"
    PAN_RIGHT = "pan_right"
    TILT_UP = "tilt_up"
    TILT_DOWN = "tilt_down"
    PUSH_IN = "push_in"
    PULL_OUT = "pull_out"
    HANDHELD = "handheld"
    ORBIT = "orbit"


class FacePose(str, Enum):
    """角色面部朝向。口型判定用。"""

    FRONT = "front"
    THREE_QUARTER = "three_quarter"
    PROFILE = "profile"
    BACK = "back"
    OFF_SCREEN = "off_screen"


LIPSYNC_CAPABLE_POSES = frozenset({FacePose.FRONT, FacePose.THREE_QUARTER, FacePose.PROFILE})


class Transition(str, Enum):
    CUT = "cut"
    DISSOLVE = "dissolve"
    FADE_IN = "fade_in"
    FADE_OUT = "fade_out"
    WHIP = "whip"


class ShotStatus(str, Enum):
    """镜头在流水线上的位置。断点续跑靠它。"""

    PLANNED = "planned"  # 分镜已出，未开工
    AUDIO_DONE = "audio_done"  # 配音已出，时长已锁
    FRAME_DONE = "frame_done"  # 首帧已出
    DRAFT_DONE = "draft_done"  # 草稿档视频已出
    DRAFT_REJECTED = "draft_rejected"  # 草稿未过闸门
    FINAL_DONE = "final_done"  # 成片档已出
    FINAL_REJECTED = "final_rejected"  # 成片未过闸门
    FALLBACK = "fallback"  # 重试超限，降级为静帧加运镜
    LOCKED = "locked"  # 人工确认，不再重跑


class CharacterInShot(BaseModel):
    """角色在本镜头中的表现。

    只有可变项。外观描述属于角色资产，不在这里，也不允许大模型在这里写。
    """

    model_config = {"extra": "forbid"}

    char_id: str = Field(description="角色 id，必须是已注册角色之一")
    expression: str = Field(default="", max_length=40, description="表情，如 愕然、隐忍")
    action: str = Field(default="", max_length=80, description="本镜动作，如 后退半步")
    wardrobe_state: str = Field(
        default="default", max_length=40, description="服装状态 id，如 suit_torn"
    )
    face_pose: FacePose = Field(default=FacePose.FRONT, description="面部朝向")
    screen_pos: str = Field(default="center", description="画面位置：left / center / right")


class DialogueLine(BaseModel):
    """一句台词。时长字段由配音阶段回填，分镜阶段不填。"""

    model_config = {"extra": "forbid"}

    char_id: str | None = Field(default=None, description="说话角色。为空表示旁白")
    text: str = Field(min_length=1, max_length=200)
    emotion: str = Field(default="neutral", description="情绪标签")
    emotion_intensity: float = Field(default=0.5, ge=0.0, le=1.0)
    voice_id: str | None = Field(default=None, description="音色 id，由角色资产决定")
    # 以下由配音阶段回填
    audio_path: str | None = Field(default=None, description="相对项目根的路径")
    actual_duration_s: float | None = Field(default=None, ge=0)


class Shot(BaseModel):
    """一个镜头。"""

    model_config = {"extra": "forbid"}

    shot_id: str = Field(pattern=r"^[a-z0-9_]+$", description="全局唯一，如 ep01_s03_sh007")
    scene_id: str = Field(pattern=r"^[a-z0-9_]+$")
    order: int = Field(ge=0, description="集内顺序")

    # ---- 画面 ----
    visual_desc: str = Field(default="", max_length=300, description="给人看的中文描述")
    first_frame_prompt: str = Field(default="", max_length=1200)
    last_frame_prompt: str | None = Field(
        default=None, max_length=1200, description="为空则走单帧图生视频"
    )
    motion_prompt: str = Field(default="", max_length=400, description="运动描述，给视频模型")
    negative_prompt: str = Field(default="")

    shot_size: ShotSize = ShotSize.MS
    camera_angle: CameraAngle = CameraAngle.EYE_LEVEL
    camera_move: CameraMove = CameraMove.STATIC
    camera_id: str | None = Field(
        default=None, description="复用机位 id。同场景同机位保证不越轴"
    )

    # ---- 引用（只放 id，不放描述）----
    characters: list[CharacterInShot] = Field(default_factory=list)
    location_id: str | None = None
    prop_ids: list[str] = Field(default_factory=list)

    # ---- 时间 ----
    duration_s: float = Field(default=5.0, gt=0, le=30, description="镜头时长")
    duration_locked: bool = Field(
        default=False, description="真表示已由配音时长反推锁定，不可再调"
    )

    # ---- 声音 ----
    dialogue: list[DialogueLine] = Field(default_factory=list)
    sfx: list[str] = Field(default_factory=list)
    bgm_cue: str | None = None
    needs_lipsync: bool = Field(
        default=False, description="由 derive_needs_lipsync 规则推导，不要让模型填"
    )

    # ---- 剪辑 ----
    transition_in: Transition = Transition.CUT
    transition_dur_s: float = Field(default=0.0, ge=0, le=2.0)
    subtitle_text: str = Field(default="")

    # ---- 质控 ----
    beat: str = Field(default="", max_length=20, description="叙事功能，如 反转、铺垫")
    continuity_notes: str = Field(default="", max_length=200)
    missing_info: list[str] = Field(
        default_factory=list, description="模型自报的信息缺口，供校验阶段检查"
    )

    # ---- 运行时状态（大模型不填）----
    status: ShotStatus = ShotStatus.PLANNED
    attempts: int = Field(default=0, ge=0)
    frame_path: str | None = None
    video_path: str | None = None
    gate_notes: list[str] = Field(default_factory=list)

    @model_validator(mode="after")
    def _check_transition(self) -> Shot:
        if self.transition_in == Transition.CUT and self.transition_dur_s != 0:
            raise ValueError("硬切的转场时长必须为 0")
        if self.transition_in != Transition.CUT and self.transition_dur_s <= 0:
            raise ValueError(f"{self.transition_in.value} 需要一个大于 0 的转场时长")
        return self

    @model_validator(mode="after")
    def _check_dialogue_chars(self) -> Shot:
        """台词里出现的角色必须也在 characters 里，否则是分镜自相矛盾。"""
        present = {c.char_id for c in self.characters}
        for line in self.dialogue:
            if line.char_id is not None and line.char_id not in present:
                raise ValueError(
                    f"台词说话人 {line.char_id} 不在本镜角色列表中。"
                    f"旁白请把 char_id 留空"
                )
        return self

    @property
    def has_onscreen_dialogue(self) -> bool:
        return any(line.char_id is not None for line in self.dialogue)

    @property
    def total_dialogue_duration_s(self) -> float | None:
        """全部台词的实际总时长。任一句未配音则返回 None。"""
        if not self.dialogue:
            return 0.0
        durations = [line.actual_duration_s for line in self.dialogue]
        if any(d is None for d in durations):
            return None
        return sum(durations)  # type: ignore[arg-type]


def derive_needs_lipsync(shot: Shot) -> bool:
    """判断一个镜头该不该做口型。

    这件事必须用规则算，不能交给大模型判断，它在这上面很不稳。

    四个条件同时成立才做：有出镜台词、景别够近能看清嘴、机位不是顶拍、
    并且说话的角色确实是正脸或侧脸对着镜头。
    """
    if not shot.has_onscreen_dialogue:
        return False
    if shot.shot_size not in LIPSYNC_CAPABLE_SIZES:
        return False
    if shot.camera_angle not in LIPSYNC_CAPABLE_ANGLES:
        return False

    speakers = {line.char_id for line in shot.dialogue if line.char_id is not None}
    poses = {c.face_pose for c in shot.characters if c.char_id in speakers}
    return bool(poses & LIPSYNC_CAPABLE_POSES)


def apply_lipsync_rules(shots: list[Shot]) -> list[Shot]:
    """批量回填 needs_lipsync。分镜生成后、配音之前调用。"""
    for shot in shots:
        shot.needs_lipsync = derive_needs_lipsync(shot)
    return shots
