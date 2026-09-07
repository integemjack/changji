"""角色与场景资产。

外观描述只存在于这里。分镜表里只有 id。渲染提示词时由程序把不可变外观块
和本镜可变项机械拼接，保证同一角色在几十个镜头里拿到逐字节相同的描述。
"""

from __future__ import annotations

from enum import Enum

from pydantic import BaseModel, Field


class StyleLine(str, Enum):
    """风格线。决定用哪套出图基座和提示词范式。"""

    REALISTIC = "realistic"
    ANIME = "anime"


class AppearanceBlock(BaseModel):
    """不可变外观块。一旦定稿就不再改，改了等于换角色。

    五段式结构。拼提示词时按固定顺序连接，顺序也不能变，
    因为提示词里靠前的词权重更高，顺序变了画面就会漂。
    """

    model_config = {"extra": "forbid"}

    identity: str = Field(description="身份：性别、年龄段、气质")
    body: str = Field(default="", description="体型、身高感")
    face: str = Field(description="五官、发型、发色、瞳色")
    attire: str = Field(description="默认服装")
    style: str = Field(default="", description="该角色特有的画风修饰")

    def render(self, style_line: StyleLine) -> str:
        """拼成提示词片段。写实线用自然语言，动漫线用标签串。

        各段的尾部标点要去掉。手写的设定里常带句号，拼接后会变成
        「冷静克制。，身姿笔挺。，」这样标点重复的串，
        而这个串会出现在每一个镜头的提示词里。
        """
        raw = (self.identity, self.body, self.face, self.attire, self.style)
        parts = [p.strip().rstrip("。.；;，,、 ") for p in raw]
        parts = [p for p in parts if p]
        sep = ", " if style_line is StyleLine.ANIME else "，"
        return sep.join(parts)


class WardrobeVariant(BaseModel):
    """服装变体。剧情里换装、衣服破损这些状态。"""

    model_config = {"extra": "forbid"}

    wardrobe_id: str
    description: str = Field(description="覆盖 AppearanceBlock.attire")


class Character(BaseModel):
    """一个角色。"""

    model_config = {"extra": "forbid"}

    char_id: str = Field(pattern=r"^c_[a-z0-9_]+$", description="必须以 c_ 开头")
    name: str = Field(description="剧本里的称呼")
    appearance: AppearanceBlock
    wardrobe: list[WardrobeVariant] = Field(default_factory=list)

    # 参考图，相对项目根的路径
    ref_front: str | None = None
    ref_three_quarter: str | None = None
    ref_back: str | None = None

    # 训练出来的角色 LoRA
    lora_path: str | None = None
    lora_trigger: str | None = None
    lora_strength: float = Field(default=1.0, ge=0.0, le=2.0)

    # 配音
    voice_id: str | None = None
    voice_ref_audio: str | None = None
    # 猜出来的性别和角色序号。配音时拿它们从服务端的音色列表里挑。
    voice_gender: str = ""
    voice_order: int = 0

    def wardrobe_desc(self, wardrobe_state: str) -> str:
        """取指定服装状态的描述，找不到就退回默认。"""
        if wardrobe_state and wardrobe_state != "default":
            for variant in self.wardrobe:
                if variant.wardrobe_id == wardrobe_state:
                    return variant.description
        return self.appearance.attire

    def render_prompt(self, style_line: StyleLine, wardrobe_state: str = "default") -> str:
        """渲染该角色的完整外观提示词片段。

        换装时只替换 attire 那一段，其余逐字节不变。
        """
        block = self.appearance
        if wardrobe_state != "default":
            block = block.model_copy(update={"attire": self.wardrobe_desc(wardrobe_state)})
        rendered = block.render(style_line)
        if self.lora_trigger:
            sep = ", " if style_line is StyleLine.ANIME else "，"
            rendered = f"{self.lora_trigger}{sep}{rendered}"
        return rendered

    def ref_for_pose(self, face_pose: str) -> str | None:
        """按面部朝向挑参考图。"""
        return {
            "front": self.ref_front,
            "three_quarter": self.ref_three_quarter or self.ref_front,
            "profile": self.ref_three_quarter or self.ref_front,
            "back": self.ref_back or self.ref_three_quarter,
        }.get(face_pose, self.ref_front)


class Location(BaseModel):
    """一个场景。空景图是场景一致性的锚点。"""

    model_config = {"extra": "forbid"}

    location_id: str = Field(pattern=r"^loc_[a-z0-9_]+$")
    name: str
    space: str = Field(description="空间结构")
    lighting: str = Field(description="光线基调，如 冷调顶光、暖调侧逆光")
    palette: str = Field(default="", description="色彩方案")
    ref_empty: str | None = Field(default=None, description="空景图路径，无人物")

    def render_prompt(self, style_line: StyleLine) -> str:
        parts = [p.strip() for p in (self.space, self.lighting, self.palette) if p.strip()]
        sep = ", " if style_line is StyleLine.ANIME else "，"
        return sep.join(parts)


class StyleProfile(BaseModel):
    """全剧统一的风格层。所有镜头共用，保证整体调性不漂。"""

    model_config = {"extra": "forbid"}

    style_line: StyleLine = StyleLine.REALISTIC
    global_style: str = Field(default="", description="全剧画风、色温、质感")
    negative_prompt: str = Field(default="")
    aspect_ratio: str = Field(default="9:16", description="9:16 竖屏 或 16:9 横屏")


class AssetLibrary(BaseModel):
    """角色和场景的资产库。分镜表里的每个 id 都必须能在这里查到。"""

    model_config = {"extra": "forbid"}

    characters: dict[str, Character] = Field(default_factory=dict)
    locations: dict[str, Location] = Field(default_factory=dict)
    style: StyleProfile = Field(default_factory=StyleProfile)

    def character_ids(self) -> list[str]:
        """给大模型做约束解码用的枚举。有了它模型就编不出新角色。"""
        return sorted(self.characters)

    def location_ids(self) -> list[str]:
        return sorted(self.locations)

    def validate_references(self, char_ids: set[str], loc_ids: set[str]) -> list[str]:
        """检查分镜表引用的 id 是否都已注册。返回问题列表。"""
        problems = []
        for cid in sorted(char_ids - set(self.characters)):
            problems.append(f"角色 {cid} 未在资产库注册")
        for lid in sorted(loc_ids - set(self.locations)):
            problems.append(f"场景 {lid} 未在资产库注册")
        return problems


# 参考音色的偏好顺序。真正能用哪些由服务端说了算，
# 这里只是没得选时的默认倾向：中文样本优先，其次按性别分。
#
# 踩过的坑：直接写死路径提交上去，节点校验不过整条流水线就断了。
# 参考音色在 ComfyUI 那边是个下拉框，装了哪些插件就有哪些选项，
# 换一台机器列表就不一样，所以不能假设任何一条路径一定存在。
# 参考音色是 ComfyUI 那边的一个下拉框，装了哪些插件就有哪些选项，
# 换一台机器列表就不一样。所以不能写死路径，只能按名字里的线索猜。
#
# 踩过的坑：写死 vibevoice 的中文样本提交上去，节点校验直接拒绝，
# 整条流水线断在配音这一步。文件在磁盘上，但不在这个节点的列表里。
_FEMALE_MARKS = ("female", "woman", "_f_", "belinda", "mabel", "sophie")
_MALE_MARKS = ("male", "man", "_m_", "chadwick", "eastwood", "freeman",
               "attenborough")

_FEMALE_WORDS = ("女", "妈", "母", "姐", "妹", "婆", "娘", "妻", "太太", "阿姨")
_MALE_WORDS = ("男", "爸", "父", "哥", "弟", "爷", "叔", "夫", "先生", "伯")


def guess_gender(identity: str) -> str:
    """从身份描述里猜性别。猜不出来返回空串。

    身份描述里通常会写「一位年轻的女性」这类话，够用了。
    """
    text = identity or ""
    female = any(w in text for w in _FEMALE_WORDS)
    male = any(w in text for w in _MALE_WORDS)
    if female and not male:
        return "female"
    if male and not female:
        return "male"
    return ""


def _voice_gender(name: str) -> str:
    low = name.lower()
    # female 里含 male，先判 female
    if any(m in low for m in _FEMALE_MARKS):
        return "female"
    if any(m in low for m in _MALE_MARKS):
        return "male"
    return ""


def pick_voice(available: list[str], gender: str, index: int = 0) -> str | None:
    """从服务端给的音色列表里挑一条参考音频。

    先按性别分，再在同性别里优先中文样本。顺序不能反过来：
    唯一那条中文样本是男声，反过来的话女角色会被配上男声，
    这比带一点口音难听得多。
    """
    pool = [v for v in available if v and v != "none"]
    if not pool:
        return None

    def rank(v: str) -> tuple[int, int, str]:
        vg = _voice_gender(v)
        if not gender or not vg:
            gender_rank = 1          # 分不出来的排中间
        elif vg == gender:
            gender_rank = 0
        else:
            gender_rank = 2          # 性别相反的排最后
        low = v.lower()
        zh_rank = 0 if ("zh_" in low or "zh-" in low) else 1
        return (gender_rank, zh_rank, v)

    ranked = sorted(pool, key=rank)
    best = rank(ranked[0])[:2]
    tier = [v for v in ranked if rank(v)[:2] == best]
    # 同一档里按角色序号轮着分，不同角色至少声音不同
    return tier[index % len(tier)]
