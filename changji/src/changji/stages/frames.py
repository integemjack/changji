"""逐镜首帧生成。

这是角色一致性真正落地的一环。视频模型只负责三到五秒的运动，
跨镜头的长相、服装、场景全靠首帧锁死。

后端可插拔。装了图像编辑模型就走它，能吃角色定妆图和场景空景图做参考，
一致性最好。没装就退回用视频模型生成单帧，本机实测 3.5 秒一张，
质量不如前者但能让整条流水线先跑起来。
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Protocol

from ..assembly.ffmpeg import FFmpeg, FFmpegError
from ..comfy.client import ComfyClient, PromptValidationError
from ..comfy.workflow import ApiWorkflow, WorkflowError
from ..hardware import TierSpec
from ..models.project import ProjectPaths
from ..models.shot import Shot, ShotStatus
from .render import PromptComposer, RenderError


class FrameError(RuntimeError):
    pass


class FrameBackend(Protocol):
    """首帧后端。"""

    name: str

    async def generate(
        self, shot: Shot, prompts, spec: TierSpec, dest: Path,
    ) -> Path:
        ...


class VideoModelFrameBackend:
    """用视频模型生成单帧。

    把长度设成 1 帧、断开首帧输入，视频模型就退化成文生图。
    质量不如专门的图像模型，但不需要额外装东西，
    并且和后续的视频生成用同一个模型，画风天然一致。
    """

    name = "video_model"

    def __init__(
        self, client: ComfyClient, workflow: ApiWorkflow, ff: FFmpeg,
        steps: int = 8,
    ) -> None:
        self.client = client
        self.workflow = workflow
        self.ff = ff
        self.steps = steps

    async def generate(
        self, shot: Shot, prompts, spec: TierSpec, dest: Path,
    ) -> Path:
        wf = self.workflow.copy()

        # 断开首帧输入，让它变成纯文生
        try:
            latent_id = wf.one_by_class("Wan22ImageToVideoLatent")
        except WorkflowError as exc:
            raise FrameError(
                "工作流里没有找到潜空间节点，无法用视频模型出首帧。"
                "请改用图像工作流。"
            ) from exc
        wf.prompt[latent_id]["inputs"].pop("start_image", None)
        wf.set_input(latent_id, "width", spec.width)
        wf.set_input(latent_id, "height", spec.height)
        wf.set_input(latent_id, "length", 1)

        wf.set_by_class("KSampler", steps=self.steps, seed=_seed_for(shot))
        pos_id, neg_id = _text_nodes(wf)
        wf.set_input(pos_id, "text", prompts.positive)
        wf.set_input(neg_id, "text", prompts.negative)
        wf.set_by_class("SaveVideo",
                        filename_prefix=f"changji/frame_{shot.shot_id}")

        try:
            result = await self.client.run(wf)
        except PromptValidationError as exc:
            raise FrameError(
                f"镜头 {shot.shot_id} 的首帧工作流被拒绝：\n{exc.human_summary()}"
            ) from exc

        ref = result.first_file()
        if ref is None:
            raise FrameError(f"镜头 {shot.shot_id} 首帧生成完成但没有产出")

        # 产出是单帧视频，抽成图片
        tmp = dest.with_suffix(".mp4")
        await self.client.download(ref, tmp)
        try:
            await self.ff.extract_frame(tmp, dest, at_second=0.0)
        except FFmpegError as exc:
            raise FrameError(f"从单帧视频里抽图失败：{exc}") from exc
        finally:
            tmp.unlink(missing_ok=True)
        return dest


class ImageModelFrameBackend:
    """用专门的图像工作流生成首帧。

    这条路能吃角色定妆图和场景空景图做参考，跨镜头一致性远好于视频模型。
    工作流由用户提供，放在项目的 workflows/image.json。
    """

    name = "image_model"

    def __init__(
        self, client: ComfyClient, workflow: ApiWorkflow, paths: ProjectPaths,
    ) -> None:
        self.client = client
        self.workflow = workflow
        self.paths = paths

    async def generate(
        self, shot: Shot, prompts, spec: TierSpec, dest: Path,
    ) -> Path:
        wf = self.workflow.copy()

        # 尺寸节点的类型因模型而异，逐个尝试
        for cls in ("EmptySD3LatentImage", "EmptyLatentImage",
                    "EmptySD3LatentImage", "ModelSamplingSD3"):
            try:
                wf.set_by_class(cls, width=spec.width, height=spec.height)
                break
            except WorkflowError:
                continue

        try:
            wf.set_by_class("KSampler", seed=_seed_for(shot))
            pos_id, neg_id = _text_nodes(wf)
            wf.set_input(pos_id, "text", prompts.positive)
            wf.set_input(neg_id, "text", prompts.negative)
        except WorkflowError as exc:
            raise FrameError(f"图像工作流结构不认识：{exc}") from exc

        # 上传参考图。有几个参考图输入就填几个。
        uploaded: list[str] = []
        for ref_rel in prompts.reference_images:
            ref_path = self.paths.abs(ref_rel)
            if ref_path.is_file():
                uploaded.append(await self.client.upload_image(ref_path))
        for i, node_id in enumerate(wf.find_by_class("LoadImage")):
            if i < len(uploaded):
                wf.set_input(node_id, "image", uploaded[i])

        try:
            wf.set_by_class("SaveImage",
                            filename_prefix=f"changji/frame_{shot.shot_id}")
        except WorkflowError:
            pass  # 有些工作流用别的保存节点，用默认前缀也能取到

        try:
            result = await self.client.run(wf)
        except PromptValidationError as exc:
            raise FrameError(
                f"镜头 {shot.shot_id} 的图像工作流被拒绝：\n{exc.human_summary()}"
            ) from exc

        ref = result.first_file()
        if ref is None:
            raise FrameError(f"镜头 {shot.shot_id} 首帧生成完成但没有产出")
        return await self.client.download(ref, dest)


def _seed_for(shot: Shot) -> int:
    """首帧的种子。和视频用不同的偏移，避免两者撞上同一个坏种子。"""
    base = abs(hash("frame:" + shot.shot_id)) % (2**31)
    return (base + shot.attempts * 6271) % (2**31)


def _text_nodes(wf: ApiWorkflow) -> tuple[str, str]:
    """找正负提示词节点。靠它们连到采样器的哪个输入槽区分，不能按 id 猜。"""
    ks = wf.one_by_class("KSampler")
    inputs = wf.prompt[ks]["inputs"]
    pos, neg = inputs.get("positive"), inputs.get("negative")
    if not (isinstance(pos, list) and isinstance(neg, list)):
        raise WorkflowError("采样器没有接正负提示词，无法确定改哪个节点")
    return str(pos[0]), str(neg[0])


@dataclass
class FrameOutcome:
    shot_id: str
    ok: bool
    path: Path | None = None
    error: str | None = None
    elapsed_s: float = 0.0


class FrameStage:
    """给一批镜头出首帧。"""

    def __init__(
        self, backend: FrameBackend, composer: PromptComposer, paths: ProjectPaths,
    ) -> None:
        self.backend = backend
        self.composer = composer
        self.paths = paths

    async def run(
        self,
        shots: list[Shot],
        spec: TierSpec,
        aspect_ratio: str = "9:16",
        on_done: Callable[[FrameOutcome], None] | None = None,
    ) -> list[FrameOutcome]:
        """顺序生成。显卡只有一张，并发只会更慢。"""
        scaled = spec.scaled_to(aspect_ratio)
        outcomes: list[FrameOutcome] = []
        for shot in shots:
            started = time.monotonic()
            try:
                prompts = self.composer.compose(shot)
                dest = self.paths.frames / f"{shot.shot_id}.png"
                path = await self.backend.generate(shot, prompts, scaled, dest)
                shot.frame_path = self.paths.rel(path)
                shot.status = ShotStatus.FRAME_DONE
                outcome = FrameOutcome(shot.shot_id, True, path,
                                       elapsed_s=time.monotonic() - started)
            except (FrameError, RenderError) as exc:
                shot.attempts += 1
                outcome = FrameOutcome(shot.shot_id, False, error=str(exc),
                                       elapsed_s=time.monotonic() - started)
            outcomes.append(outcome)
            if on_done:
                on_done(outcome)
        return outcomes


def build_backend(
    client: ComfyClient,
    paths: ProjectPaths,
    video_workflow: ApiWorkflow,
    ff: FFmpeg,
    image_workflow: ApiWorkflow | None = None,
) -> FrameBackend:
    """按可用资源选后端。

    项目里放了 workflows/image.json 就用图像模型，一致性更好。
    没放就退回视频模型出单帧，不需要额外装东西。
    """
    if image_workflow is not None:
        return ImageModelFrameBackend(client, image_workflow, paths)
    return VideoModelFrameBackend(client, video_workflow, ff)
