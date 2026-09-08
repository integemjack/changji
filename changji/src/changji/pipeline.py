"""流水线编排。

把各阶段串成一条线，接上闸门和断点续跑。

两条原则：

每完成一个镜头就落盘。中途断电、显卡崩、用户按了停止，
重新跑的时候从上次的位置继续，不重做已完成的部分。

闸门失败绝不静默放行。要么重试，要么退回上一阶段，要么明确降级并留下记录。
无人值守时人只需要看闸门报告。
"""

from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from .assembly.assemble import Assembler, AssemblyError, build_timeline
from .assembly.ffmpeg import FFmpeg
from .config import Settings
from .gates.checks import (
    GateResult,
    Verdict,
    decide_next,
    gate_audio_sync,
    gate_episode,
    gate_video,
)
from .hardware import HardwareProfile, Tier
from .models.character import AssetLibrary
from .models.project import Episode, Project, ProjectStore
from .models.shot import Shot, ShotStatus
from .stages.audio import AudioStage, build_backend, split_overlong_shots
from .stages.audio import summarize as summarize_audio
from .stages.frames import FrameOutcome, FrameStage
from .stages.frames import build_backend as build_frame_backend
from .stages.render import (
    PromptComposer,
    RenderStage,
    frames_for,
    max_shot_duration_s,
)


class Stage(str, Enum):
    """流水线的阶段。也是断点续跑的锚点。"""

    AUDIO = "audio"
    FRAMES = "frames"
    DRAFT = "draft"
    FINAL = "final"
    ASSEMBLE = "assemble"


@dataclass
class Event:
    """流水线事件。界面和命令行都靠它显示进度。"""

    stage: Stage
    kind: str  # start / progress / shot_done / gate / warn / done / error
    message: str
    shot_id: str | None = None
    current: int = 0
    total: int = 0
    elapsed_s: float = 0.0

    @property
    def fraction(self) -> float:
        return self.current / self.total if self.total else 0.0


Listener = Callable[[Event], None]


@dataclass
class RunReport:
    """一次运行的结果。"""

    episode_id: str
    started_at: float
    finished_at: float = 0.0
    output: Path | None = None
    gate_results: list[GateResult] = field(default_factory=list)
    fallbacks: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

    @property
    def elapsed_s(self) -> float:
        return (self.finished_at or time.monotonic()) - self.started_at

    @property
    def ok(self) -> bool:
        return self.output is not None and not self.errors

    def render(self) -> str:
        lines = [f"剧集 {self.episode_id}  耗时 {self.elapsed_s / 60:.1f} 分钟"]
        failed = [g for g in self.gate_results if not g.ok]
        lines.append(
            f"闸门检查 {len(self.gate_results)} 次，"
            f"未通过 {len(failed)} 次，降级 {len(self.fallbacks)} 个镜头"
        )
        if self.fallbacks:
            lines.append("降级的镜头（重试超限，用静帧顶替）：")
            for sid in self.fallbacks:
                lines.append(f"  {sid}")
        if self.errors:
            lines.append("错误：")
            for e in self.errors[:10]:
                lines.append(f"  {e}")
        if self.output:
            lines.append(f"成片：{self.output}")
        return "\n".join(lines)


def human_time(seconds: float) -> str:
    """把秒说成人话。「还剩 1847 秒」没人愿意在脑子里除一遍。"""
    seconds = max(0.0, seconds)
    if seconds < 60:
        return f"{seconds:.0f} 秒"
    minutes = seconds / 60
    if minutes < 60:
        return f"{minutes:.0f} 分钟"
    return f"{minutes / 60:.1f} 小时"


class Pipeline:
    """一集的完整流水线。"""

    def __init__(
        self,
        store: ProjectStore,
        settings: Settings,
        comfy_client,
        video_workflow,
        listener: Listener | None = None,
        image_workflow=None,
        tts_workflow=None,
    ) -> None:
        self.store = store
        self.settings = settings
        self.client = comfy_client
        self.video_workflow = video_workflow
        self._image_workflow = image_workflow
        self._tts_workflow = tts_workflow
        self.listener = listener or (lambda e: None)
        self.ff = FFmpeg(settings.assembly.ffmpeg_path, settings.assembly.ffprobe_path)
        self.profile = HardwareProfile.detect(settings.vram_gb_override)

    def _emit(self, event: Event) -> None:
        try:
            self.listener(event)
        except Exception:
            pass  # 监听器出错不能影响流水线

    def _save(self, project: Project) -> None:
        """落盘。每个镜头做完都存，断点续跑靠它。"""
        self.store.save_project(project)

    # ---- 各阶段 ----

    async def run_audio(
        self, project: Project, episode: Episode, assets: AssetLibrary,
        force: bool = False,
    ) -> None:
        """配音。在生成任何画面之前跑完。"""
        todo = [
            s for s in episode.sorted_shots()
            if force or s.status == ShotStatus.PLANNED
        ]
        if not todo:
            self._emit(Event(Stage.AUDIO, "done", "配音已完成，跳过"))
            return

        backend = build_backend(
            self.settings.tts, self.client, self._tts_workflow, self.store.paths)
        # 后端名字对用户没有意义，要说清楚这次到底出不出声音
        how = {
            "estimate": "只算时长不出声音，成片会是静音",
            "comfy": "走 ComfyUI 配音节点",
            "http": "走独立配音服务",
        }.get(backend.name, backend.name)
        self._emit(Event(
            Stage.AUDIO, "start",
            f"给 {len(todo)} 个镜头配音，{how}",
            total=len(todo),
        ))
        stage = AudioStage(backend, self.settings.tts, self.store.paths)

        def report(plan, done: int, total: int) -> None:
            self._emit(Event(
                Stage.AUDIO, "shot_done",
                f"{plan.shot_id} 配音 {plan.speech_duration_s} 秒，"
                f"镜头锁到 {plan.locked_duration_s} 秒",
                shot_id=plan.shot_id, current=done, total=total,
            ))

        plans = await stage.run(todo, assets, on_shot=report)
        if stage.unknown_voices:
            self._emit(Event(
                Stage.AUDIO, "warn",
                f"这些音色服务端上没有，已自动换成可用的："
                f"{'、'.join(sorted(stage.unknown_voices))}。"
                f"想指定的话去角色场景页从下拉框里选",
            ))

        for shot in todo:
            shot.status = ShotStatus.AUDIO_DONE

        # 台词太多装不下的镜头，在这里拆成连着的几镜。
        # 拆在配音之后、出首帧之前：音频已经有了，画面还没生成，
        # 拆开不浪费任何一次渲染。
        before = len(episode.shots)
        episode.shots = split_overlong_shots(
            episode.shots, max_shot_duration_s(self.settings.assembly.fps))
        added = len(episode.shots) - before
        if added:
            self._emit(Event(
                Stage.AUDIO, "warn",
                f"有 {added} 处台词一镜装不下，已拆成新的镜头。"
                f"这一集现在是 {len(episode.shots)} 个镜头",
            ))
        self._save(project)
        self._emit(Event(Stage.AUDIO, "done", summarize_audio(plans),
                         current=len(todo), total=len(todo)))

    async def run_frames(
        self, project: Project, episode: Episode, assets: AssetLibrary,
        force: bool = False,
    ) -> None:
        """出逐镜首帧。跨镜头一致性靠这一步锁死。"""
        todo = [
            s for s in episode.sorted_shots()
            if force or s.status == ShotStatus.AUDIO_DONE
        ]
        if not todo:
            self._emit(Event(Stage.FRAMES, "done", "首帧已完成，跳过"))
            return

        backend = build_frame_backend(
            self.client, self.store.paths, self.video_workflow, self.ff,
            self._image_workflow,
        )
        # 传了参考图但走的是视频模型这条路，那些图一张都不会被用上。
        # 不说的话，用户传了图、镜头也退回重跑了，画面却一点没变，
        # 只会以为是模型不听话。
        if backend.name != "image_model":
            with_refs = [c.name for c in assets.characters.values()
                         if c.ref_front or c.ref_three_quarter or c.ref_back]
            if with_refs:
                self._emit(Event(
                    Stage.FRAMES, "warn",
                    f"{'、'.join(with_refs)} 传了参考图，但这一步用的是视频模型，"
                    f"不看参考图。要让它生效，把一个图像工作流存成项目里的 "
                    f"workflows/image.json",
                ))
        self._emit(Event(
            Stage.FRAMES, "start",
            f"给 {len(todo)} 个镜头出首帧（{backend.name}）",
            total=len(todo),
        ))

        stage = FrameStage(backend, PromptComposer(assets), self.store.paths)
        done = 0

        def report(outcome: FrameOutcome) -> None:
            nonlocal done
            done += 1
            if outcome.ok:
                self._emit(Event(Stage.FRAMES, "shot_done",
                                 f"{outcome.shot_id} 首帧完成",
                                 shot_id=outcome.shot_id, current=done,
                                 total=len(todo), elapsed_s=outcome.elapsed_s))
            else:
                self._emit(Event(Stage.FRAMES, "warn",
                                 f"{outcome.shot_id} 首帧失败：{outcome.error}",
                                 shot_id=outcome.shot_id, current=done,
                                 total=len(todo)))

        # 首帧用草稿档的尺寸。它只是给视频模型定调，不需要成片分辨率。
        spec = self.profile.tiers[Tier.DRAFT]
        outcomes = await stage.run(todo, spec, assets.style.aspect_ratio, report)
        self._save(project)

        failed = [o for o in outcomes if not o.ok]
        self._emit(Event(
            Stage.FRAMES, "done",
            f"首帧完成 {len(outcomes) - len(failed)} 个"
            + (f"，失败 {len(failed)} 个（这些镜头会退回纯文生视频）"
               if failed else ""),
            current=len(outcomes), total=len(outcomes),
        ))

    async def run_render(
        self, project: Project, episode: Episode, assets: AssetLibrary,
        tier: Tier, force: bool = False,
    ) -> list[GateResult]:
        """渲染一个档位，并逐个过闸门。"""
        stage_name = Stage.DRAFT if tier is Tier.DRAFT else Stage.FINAL
        want_before = (
            ShotStatus.FRAME_DONE if tier is Tier.DRAFT else ShotStatus.DRAFT_DONE
        )
        want_after = (
            ShotStatus.DRAFT_DONE if tier is Tier.DRAFT else ShotStatus.FINAL_DONE
        )

        # 首帧失败的镜头状态还停在 AUDIO_DONE。它们不该被跳过，
        # 而是退回纯文生视频，画面一致性差一些但整集不会卡在这里。
        extra = (
            {ShotStatus.AUDIO_DONE, ShotStatus.DRAFT_REJECTED}
            if tier is Tier.DRAFT else {ShotStatus.FINAL_REJECTED}
        )
        todo = [
            s for s in episode.sorted_shots()
            if force or s.status == want_before or s.status in extra
        ]
        if not todo:
            self._emit(Event(stage_name, "done", f"{tier.value} 档已完成，跳过"))
            return []

        spec = self.profile.tiers[tier]
        est = self.profile.estimate_episode(len(todo), tier)
        self._emit(Event(
            stage_name, "start",
            f"{tier.value} 档渲染 {len(todo)} 个镜头，"
            f"{spec.width}x{spec.height} {spec.steps} 步"
            + (f"，粗估 {human_time(est)}" if est else ""),
            total=len(todo),
        ))

        composer = PromptComposer(assets)
        render = RenderStage(
            self.client, composer, self.store.paths, self.video_workflow,
            fps=self.settings.assembly.fps,
        )
        aspect = assets.style.aspect_ratio
        results: list[GateResult] = []

        # 开跑前的预计来自一张按显存推的静态表，实测能差一倍。
        # 跑起来之后用真实耗时重算，等的人才知道还要等多久。
        stage_started = time.monotonic()
        for i, shot in enumerate(todo, start=1):
            result = await self._render_one(
                project, shot, render, spec, aspect, stage_name, want_after, i, len(todo)
            )
            if result is not None:
                results.append(result)
            left = len(todo) - i
            if left:
                per = (time.monotonic() - stage_started) / i
                self._emit(Event(
                    stage_name, "eta",
                    f"还剩 {left} 个镜头，按目前速度约 {human_time(per * left)}",
                    current=i, total=len(todo),
                ))
        return results

    async def _render_one(
        self, project: Project, shot: Shot, render: RenderStage,
        spec, aspect: str, stage_name: Stage, want_after: ShotStatus,
        index: int, total: int,
    ) -> GateResult | None:
        """渲染一个镜头并反复过闸门，直到通过或者用尽重试。"""
        gates = self.settings.gates
        last_result: GateResult | None = None
        started = time.monotonic()

        while True:
            try:
                plan = render.plan(shot, spec, aspect)
                path = await render.render_video(
                    shot, plan, start_image=shot.frame_path)
                shot.video_path = self.store.paths.rel(path)
            except Exception as exc:
                shot.attempts += 1
                self._emit(Event(stage_name, "warn",
                                 f"{shot.shot_id} 渲染失败：{exc}",
                                 shot_id=shot.shot_id, current=index, total=total))
                if shot.attempts >= gates.max_attempts_per_shot:
                    return self._fallback(project, shot, stage_name,
                                          f"渲染连续失败：{exc}")
                self._save(project)
                continue

            if not gates.enabled:
                shot.status = want_after
                self._save(project)
                self._emit(Event(stage_name, "shot_done", f"{shot.shot_id} 完成",
                                 shot_id=shot.shot_id, current=index, total=total,
                                 elapsed_s=time.monotonic() - started))
                return None

            expected = frames_for(shot.duration_s, self.settings.assembly.fps) / \
                self.settings.assembly.fps
            last_result = await gate_video(
                shot, path, self.ff, gates,
                expected_duration_s=expected,
                expected_size=(plan.spec.width, plan.spec.height),
                gate_name=f"{spec.tier.value} 档闸门",
            )

            if last_result.ok:
                shot.status = want_after
                shot.gate_notes = []
                self._save(project)
                self._emit(Event(stage_name, "shot_done",
                                 f"{shot.shot_id} 通过闸门",
                                 shot_id=shot.shot_id, current=index, total=total,
                                 elapsed_s=time.monotonic() - started))
                return last_result

            decision = decide_next(last_result, shot, gates)
            shot.gate_notes = last_result.reasons
            self._emit(Event(stage_name, "gate", last_result.describe(),
                             shot_id=shot.shot_id, current=index, total=total))

            if decision is Verdict.RETRY:
                shot.attempts += 1
                self._save(project)
                continue
            if decision is Verdict.FALLBACK:
                return self._fallback(project, shot, stage_name,
                                      "；".join(last_result.reasons))
            # REGRESS：重跑没用，标记后交给人
            shot.status = (
                ShotStatus.DRAFT_REJECTED if spec.tier is Tier.DRAFT
                else ShotStatus.FINAL_REJECTED
            )
            self._save(project)
            return last_result

    def _fallback(
        self, project: Project, shot: Shot, stage_name: Stage, reason: str,
    ) -> GateResult:
        """降级。重试超限时不卡死，用能用的东西顶上。"""
        shot.status = ShotStatus.FALLBACK
        shot.gate_notes = [reason]
        self._save(project)
        self._emit(Event(stage_name, "warn",
                         f"{shot.shot_id} 重试超限，降级处理：{reason}",
                         shot_id=shot.shot_id))
        return GateResult(shot.shot_id, Verdict.FALLBACK, "重试超限", [reason])

    async def run_assemble(
        self, project: Project, episode: Episode, out_name: str | None = None,
    ) -> Path:
        """装配成片。"""
        shots = [
            s for s in episode.sorted_shots()
            if s.video_path and s.status in (
                ShotStatus.FINAL_DONE, ShotStatus.DRAFT_DONE,
                ShotStatus.FALLBACK, ShotStatus.LOCKED,
            )
        ]
        if not shots:
            raise AssemblyError("没有可装配的镜头。先跑渲染阶段")

        self._emit(Event(Stage.ASSEMBLE, "start",
                         f"装配 {len(shots)} 个镜头", total=len(shots)))

        # 装配前先查音画能不能装下，装完再发现就得重做整集
        for shot in shots:
            if shot.dialogue:
                r = await gate_audio_sync(
                    shot, self.store.paths.abs(shot.video_path), self.ff,
                    self.settings.gates)
                if not r.ok:
                    self._emit(Event(Stage.ASSEMBLE, "gate", r.describe(),
                                     shot_id=shot.shot_id))

        timeline = build_timeline(shots, self.store.paths, self.settings.assembly)
        assembler = Assembler(
            self.ff, self.settings.assembly, self.store.paths,
            target_lufs=self.settings.gates.target_lufs,
            max_true_peak_db=self.settings.gates.max_true_peak_db,
        )
        name = out_name or f"{episode.episode_id}.mp4"
        output = await assembler.assemble(timeline, name)

        result = await gate_episode(
            output, self.ff, self.settings.gates,
            expected_duration_s=timeline.total_duration_s)
        if not result.ok:
            for reason in result.reasons:
                self._emit(Event(Stage.ASSEMBLE, "gate", f"成片：{reason}"))

        self._emit(Event(Stage.ASSEMBLE, "done", f"成片已生成：{output}",
                         current=len(shots), total=len(shots)))
        return output

    # ---- 全流程 ----

    async def run_stages(
        self, episode_id: str, stages: list[str], force: bool = False,
    ) -> RunReport:
        """只跑指定的阶段。

        用途是单独重做某一段：改完分镜只重渲染、只重出首帧、
        或者改完参数只重新装配，不必把前面的环节全跑一遍。
        """
        project = self.store.load_project()
        assets = self.store.load_assets()
        episode = project.episode_by_id(episode_id)
        if episode is None:
            raise ValueError(f"项目里没有剧集 {episode_id}")
        if not episode.shots:
            raise ValueError(f"剧集 {episode_id} 还没有分镜表")

        wanted = [s.strip() for s in stages if s.strip()]
        known = {s.value for s in Stage}
        unknown = [s for s in wanted if s not in known]
        if unknown:
            raise ValueError(
                f"不认识的阶段 {'、'.join(unknown)}。"
                f"可选：{'、'.join(sorted(known))}"
            )

        report = RunReport(episode_id=episode_id, started_at=time.monotonic())
        try:
            if Stage.AUDIO.value in wanted:
                await self.run_audio(project, episode, assets, force=force)
            if Stage.FRAMES.value in wanted:
                await self.run_frames(project, episode, assets, force=force)
            if Stage.DRAFT.value in wanted:
                report.gate_results.extend(await self.run_render(
                    project, episode, assets, Tier.DRAFT, force=force))
            if Stage.FINAL.value in wanted:
                report.gate_results.extend(await self.run_render(
                    project, episode, assets, Tier.FINAL, force=force))
            if Stage.ASSEMBLE.value in wanted:
                report.output = await self.run_assemble(project, episode)
            report.fallbacks = [
                s.shot_id for s in episode.shots if s.status == ShotStatus.FALLBACK
            ]
        except Exception as exc:
            report.errors.append(str(exc))
            self._emit(Event(Stage.ASSEMBLE, "error", str(exc)))
        finally:
            report.finished_at = time.monotonic()
            self._save(project)
        return report

    async def run(
        self,
        episode_id: str,
        skip_final: bool = False,
        force: bool = False,
    ) -> RunReport:
        """跑完一集。可以中断，可以续跑。

        skip_final 为真时只跑草稿档就装配，用于快速验证叙事。
        """
        project = self.store.load_project()
        assets = self.store.load_assets()
        episode = project.episode_by_id(episode_id)
        if episode is None:
            raise ValueError(f"项目里没有剧集 {episode_id}")
        if not episode.shots:
            raise ValueError(f"剧集 {episode_id} 还没有分镜表")

        report = RunReport(episode_id=episode_id, started_at=time.monotonic())
        try:
            await self.run_audio(project, episode, assets, force=force)
            await self.run_frames(project, episode, assets, force=force)

            draft = await self.run_render(
                project, episode, assets, Tier.DRAFT, force=force)
            report.gate_results.extend(draft)

            if not skip_final:
                final = await self.run_render(
                    project, episode, assets, Tier.FINAL, force=False)
                report.gate_results.extend(final)

            report.fallbacks = [
                s.shot_id for s in episode.shots if s.status == ShotStatus.FALLBACK
            ]
            report.output = await self.run_assemble(project, episode)
        except Exception as exc:
            report.errors.append(str(exc))
            self._emit(Event(Stage.ASSEMBLE, "error", str(exc)))
        finally:
            report.finished_at = time.monotonic()
            self._save(project)
        return report


def progress_printer(console) -> Listener:
    """把事件打到终端。命令行模式用。"""

    def listen(event: Event) -> None:
        if event.kind == "start":
            console.print(f"[cyan]{event.stage.value}[/cyan]  {event.message}")
        elif event.kind == "shot_done":
            console.print(
                f"  [green]✓[/green] {event.message}"
                f"  ({event.current}/{event.total}, {event.elapsed_s:.0f} 秒)"
            )
        elif event.kind == "gate" or event.kind == "warn":
            console.print(f"  [yellow]![/yellow] {event.message}")
        elif event.kind == "error":
            console.print(f"[red]✗[/red] {event.message}")
        elif event.kind == "done":
            console.print(f"[green]完成[/green]  {event.message}")

    return listen
