"""渲染阶段：逐镜首帧与图生视频。

提示词的拼接在这里。分层顺序是固定的，而且不能改：

    身份层（角色外观，逐字节不变）
    场景层（空间与光线，每场固定）
    镜头层（景别、机位、动作，每镜可变）
    风格层（全剧统一）

顺序不能改是因为提示词里靠前的词权重更高。顺序一变，画面重心就跟着变，
同一个角色在不同镜头里会显得不是同一个人。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from ..comfy.client import ComfyClient, JobResult, PromptValidationError
from ..comfy.workflow import ApiWorkflow
from ..hardware import Tier, TierSpec
from ..models.character import AssetLibrary, StyleLine
from ..models.project import ProjectPaths
from ..models.shot import Shot, ShotStatus


class RenderError(RuntimeError):
    pass


# 景别的中文说法。写实线用自然语言，模型对中文景别词有反应。
_SHOT_SIZE_ZH = {
    "ECU": "大特写", "CU": "特写", "MCU": "近景",
    "MS": "中景", "MLS": "中远景", "LS": "远景", "ELS": "大远景",
}
_ANGLE_ZH = {
    "low": "仰拍", "eye_level": "平视", "high": "俯拍",
    "overhead": "顶拍", "dutch": "斜角构图",
}
_MOVE_ZH = {
    "static": "固定镜头", "pan_left": "向左横摇", "pan_right": "向右横摇",
    "tilt_up": "上摇", "tilt_down": "下摇", "push_in": "镜头缓慢推近",
    "pull_out": "镜头缓慢拉远", "handheld": "手持轻微晃动", "orbit": "环绕运镜",
}


@dataclass
class PromptBundle:
    """一个镜头拼好的正负提示词。"""

    positive: str
    negative: str
    reference_images: list[str] = field(default_factory=list)


class PromptComposer:
    """把分镜表和资产库拼成提示词。

    这是一致性真正生效的地方。同一个角色在几十个镜头里拿到的身份层
    完全相同，因为它是从资产库读出来的同一个字符串。
    """

    def __init__(self, assets: AssetLibrary) -> None:
        self.assets = assets
        self.style_line = assets.style.style_line

    @property
    def _sep(self) -> str:
        return ", " if self.style_line is StyleLine.ANIME else "，"

    def compose(self, shot: Shot) -> PromptBundle:
        layers: list[str] = []
        refs: list[str] = []

        # 身份层。逐字节从资产库拼出来，模型碰不到。
        for in_shot in shot.characters:
            char = self.assets.characters.get(in_shot.char_id)
            if char is None:
                raise RenderError(
                    f"镜头 {shot.shot_id} 引用了未注册角色 {in_shot.char_id}"
                )
            desc = char.render_prompt(self.style_line, in_shot.wardrobe_state)
            beats = [desc]
            if in_shot.expression:
                beats.append(in_shot.expression)
            if in_shot.action:
                beats.append(in_shot.action)
            layers.append(self._sep.join(beats))
            ref = char.ref_for_pose(in_shot.face_pose.value)
            if ref:
                refs.append(ref)

        # 场景层
        if shot.location_id:
            loc = self.assets.locations.get(shot.location_id)
            if loc is None:
                raise RenderError(
                    f"镜头 {shot.shot_id} 引用了未注册场景 {shot.location_id}"
                )
            layers.append(loc.render_prompt(self.style_line))
            if loc.ref_empty:
                refs.append(loc.ref_empty)

        # 镜头层
        camera = [
            _SHOT_SIZE_ZH.get(shot.shot_size.value, ""),
            _ANGLE_ZH.get(shot.camera_angle.value, ""),
        ]
        layers.append(self._sep.join(c for c in camera if c))
        if shot.first_frame_prompt:
            layers.append(shot.first_frame_prompt)

        # 风格层
        if self.assets.style.global_style:
            layers.append(self.assets.style.global_style)

        negative = self._sep.join(
            p for p in (shot.negative_prompt, self.assets.style.negative_prompt) if p
        )
        return PromptBundle(
            positive=self._sep.join(p for p in layers if p.strip()),
            negative=negative,
            reference_images=refs,
        )

    def motion_prompt(self, shot: Shot) -> str:
        """给视频模型的运动描述。它只管动作，不重复外观。"""
        parts = [_MOVE_ZH.get(shot.camera_move.value, "")]
        if shot.motion_prompt:
            parts.append(shot.motion_prompt)
        for in_shot in shot.characters:
            if in_shot.action:
                parts.append(in_shot.action)
        return self._sep.join(p for p in parts if p)


@dataclass
class RenderPlan:
    """一个镜头的渲染计划。"""

    shot_id: str
    tier: Tier
    spec: TierSpec
    frames: int
    prompts: PromptBundle
    motion: str

    @property
    def duration_s(self) -> float:
        return self.frames / 24.0


# 单段帧数上限。超过这个数会在约 100 帧处到达末帧然后往回跑，
# 出现乒乓现象。这是模型本身的限制，不是可调参数。
MAX_FRAMES = 121


def max_shot_duration_s(fps: int = 24) -> float:
    """单个镜头能生成的最长时长。

    分镜的时长档位必须由它推导，不能各写一份。
    早先档位表里有 8 秒和 10 秒，而实际上限是 5 秒，
    多出来的部分被静默截断，成片比计划短了一大截且没人发现。
    """
    return MAX_FRAMES / fps


def frames_for(duration_s: float, fps: int = 24) -> int:
    """时长换算帧数。

    Wan 要求帧数满足 4n+1。超过上限的时长会被截断，
    调用方应该先用 max_shot_duration_s 把时长限住。
    """
    raw = int(round(duration_s * fps))
    n = max(1, round((raw - 1) / 4))
    frames = 4 * n + 1
    return min(frames, MAX_FRAMES)


class RenderStage:
    """驱动 ComfyUI 出图和出视频。"""

    def __init__(
        self,
        client: ComfyClient,
        composer: PromptComposer,
        paths: ProjectPaths,
        video_workflow: ApiWorkflow,
        fps: int = 24,
    ) -> None:
        self.client = client
        self.composer = composer
        self.paths = paths
        self.video_workflow = video_workflow
        self.fps = fps

    def plan(self, shot: Shot, spec: TierSpec, aspect_ratio: str = "9:16") -> RenderPlan:
        return RenderPlan(
            shot_id=shot.shot_id,
            tier=spec.tier,
            spec=spec.scaled_to(aspect_ratio),
            frames=frames_for(shot.duration_s, self.fps),
            prompts=self.composer.compose(shot),
            motion=self.composer.motion_prompt(shot),
        )

    async def render_video(
        self,
        shot: Shot,
        plan: RenderPlan,
        start_image: str | None = None,
        on_progress: Callable[[float], None] | None = None,
    ) -> Path:
        """出一个镜头的视频，下载到项目目录，返回相对路径对应的绝对路径。"""
        wf = self.video_workflow.copy()

        # 尺寸与帧数
        wf.set_by_class(
            "Wan22ImageToVideoLatent",
            width=plan.spec.width, height=plan.spec.height, length=plan.frames,
        )
        wf.set_by_class("KSampler", steps=plan.spec.steps, seed=_seed_for(shot))

        # 正负提示词。两个 CLIPTextEncode 靠连线区分，不能靠猜。
        pos_id, neg_id = self._text_node_ids(wf)
        wf.set_input(pos_id, "text", self._video_positive(plan))
        wf.set_input(neg_id, "text", plan.prompts.negative)

        if start_image:
            uploaded = await self.client.upload_image(self.paths.abs(start_image))
            wf.set_by_class("LoadImage", image=uploaded)

        prefix = f"changji/{shot.shot_id}_{plan.tier.value}"
        wf.set_by_class("SaveVideo", filename_prefix=prefix)

        def _progress(p):
            if on_progress and p.total:
                on_progress(p.fraction)

        try:
            result = await self.client.run(wf, on_progress=_progress)
        except PromptValidationError as exc:
            raise RenderError(
                f"镜头 {shot.shot_id} 的工作流被服务端拒绝：\n{exc.human_summary()}"
            ) from exc

        return await self._download(result, shot, plan.tier)

    def _video_positive(self, plan: RenderPlan) -> str:
        """视频模型的正向提示词：画面描述加运动描述。"""
        parts = [plan.prompts.positive]
        if plan.motion:
            parts.append(plan.motion)
        return self.composer._sep.join(p for p in parts if p)

    @staticmethod
    def _text_node_ids(wf: ApiWorkflow) -> tuple[str, str]:
        """找出正负提示词节点。

        工作流里有两个 CLIPTextEncode，靠它们连到 KSampler 的哪个输入槽区分。
        按节点 id 大小猜是错的，那只是画布上的创建顺序。
        """
        ks = wf.one_by_class("KSampler")
        inputs = wf.prompt[ks]["inputs"]
        pos, neg = inputs.get("positive"), inputs.get("negative")
        if not (isinstance(pos, list) and isinstance(neg, list)):
            raise RenderError(
                "工作流的 KSampler 没有接正负提示词，无法确定改哪个节点"
            )
        return str(pos[0]), str(neg[0])

    async def _download(self, result: JobResult, shot: Shot, tier: Tier) -> Path:
        ref = result.first_file()
        if ref is None:
            raise RenderError(f"镜头 {shot.shot_id} 生成完成但没有产出文件")
        target = self.paths.shots(tier.value) / f"{shot.shot_id}.mp4"
        return await self.client.download(ref, target)


def _seed_for(shot: Shot) -> int:
    """由 shot_id 和重试次数推导种子。

    同一个镜头重跑时种子必须变，否则会拿到一模一样的废片；
    但整体又要可复现，所以不能用随机数。
    """
    base = abs(hash(shot.shot_id)) % (2**31)
    return (base + shot.attempts * 7919) % (2**31)


@dataclass
class RenderOutcome:
    shot_id: str
    ok: bool
    path: Path | None = None
    error: str | None = None
    elapsed_s: float = 0.0


async def render_batch(
    stage: RenderStage,
    shots: list[Shot],
    spec: TierSpec,
    aspect_ratio: str = "9:16",
    on_shot_done: Callable[[RenderOutcome], None] | None = None,
) -> list[RenderOutcome]:
    """顺序渲染一批镜头。

    刻意不并发。显卡就一张，并发只会让每个任务都变慢并增加显存不足的风险，
    ComfyUI 自己的队列也是串行执行的。
    """
    import time

    outcomes: list[RenderOutcome] = []
    for shot in shots:
        started = time.monotonic()
        try:
            plan = stage.plan(shot, spec, aspect_ratio)
            path = await stage.render_video(shot, plan, start_image=shot.frame_path)
            shot.video_path = stage.paths.rel(path)
            shot.status = (
                ShotStatus.DRAFT_DONE if spec.tier is Tier.DRAFT else ShotStatus.FINAL_DONE
            )
            outcome = RenderOutcome(shot.shot_id, True, path,
                                    elapsed_s=time.monotonic() - started)
        except Exception as exc:
            shot.attempts += 1
            outcome = RenderOutcome(shot.shot_id, False, error=str(exc),
                                    elapsed_s=time.monotonic() - started)
        outcomes.append(outcome)
        if on_shot_done:
            on_shot_done(outcome)
    return outcomes
