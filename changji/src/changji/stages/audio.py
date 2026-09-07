"""配音阶段。

这一步在生成任何画面之前跑完，拿到每句台词的真实时长，反过来锁定镜头时长。
音画对齐从源头解决，而不是等成片后再想办法把声音塞进去。

后端可插拔。除了真实 TTS，还有一个纯估算后端：不出音频只算时长，
用来在没装 TTS 的机器上把整条流水线跑通，也用于排产估算。
"""

from __future__ import annotations

import asyncio
import re
import struct
import wave
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

import httpx

from ..config import TTSConfig
from ..models.character import AssetLibrary, pick_voice
from ..models.project import ProjectPaths
from ..models.shot import DialogueLine, Shot, ShotStatus
from .storyboard import ceil_duration


class AudioError(RuntimeError):
    pass


# 中文普通话正常语速的字符数每秒。用于估算，真实值由 TTS 返回。
# 这个数字来自播音语速的常见区间，短剧对白通常偏快。
CHARS_PER_SECOND = 4.6
# 每句话前后的呼吸留白
LEAD_IN_S = 0.15
TAIL_S = 0.25


def estimate_speech_duration(text: str) -> float:
    """估算一句中文台词的时长。

    标点会带来停顿，逗号短一点，句号问号感叹号长一点。
    """
    stripped = re.sub(r"[\s]", "", text)
    if not stripped:
        return 0.0
    # 标点不发音但产生停顿，先摘出来单独计
    pauses = {
        "，": 0.18, "、": 0.12, "；": 0.22, "：": 0.18,
        "。": 0.32, "？": 0.35, "！": 0.35, "…": 0.40, "—": 0.25,
    }
    pause_total = sum(pauses.get(ch, 0.0) for ch in stripped)
    spoken = [ch for ch in stripped if ch not in pauses]
    return len(spoken) / CHARS_PER_SECOND + pause_total + LEAD_IN_S + TAIL_S


@dataclass
class SynthesisResult:
    """一句台词的配音结果。"""

    duration_s: float
    audio_path: Path | None = None

    @property
    def is_real_audio(self) -> bool:
        return self.audio_path is not None


class TTSBackend(Protocol):
    """配音后端。"""

    name: str

    async def synthesize(
        self, text: str, out_path: Path, voice_id: str | None,
        emotion: str, intensity: float,
    ) -> SynthesisResult:
        ...

    async def available(self) -> bool:
        ...


class EstimateBackend:
    """只算时长不出音频。

    用途有两个：在没装 TTS 的机器上把整条流水线跑通，以及排产时估算总时长。
    产出的是等长静音 wav，所以后面的装配环节不用为它写特例。
    """

    name = "estimate"

    def __init__(self, sample_rate: int = 24000) -> None:
        self.sample_rate = sample_rate

    async def available(self) -> bool:
        return True

    async def synthesize(
        self, text: str, out_path: Path, voice_id: str | None = None,
        emotion: str = "neutral", intensity: float = 0.5,
    ) -> SynthesisResult:
        duration = estimate_speech_duration(text)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        _write_silence(out_path, duration, self.sample_rate)
        return SynthesisResult(duration_s=duration, audio_path=out_path)


def _write_silence(path: Path, seconds: float, sample_rate: int) -> None:
    """写一段静音 wav。"""
    frames = max(1, int(seconds * sample_rate))
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(b"\x00\x00" * frames)


class HttpTTSBackend:
    """指向一个独立的 HTTP 配音服务。

    很多 TTS 项目自带 api 服务，比 ComfyUI 节点更省事，
    而且可以跑在另一台机器上。
    """

    name = "http"

    def __init__(self, base_url: str, timeout_s: float = 300.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout_s = timeout_s

    async def available(self) -> bool:
        try:
            async with httpx.AsyncClient(timeout=10) as http:
                r = await http.get(f"{self.base_url}/health")
                return r.status_code < 500
        except httpx.RequestError:
            return False

    async def synthesize(
        self, text: str, out_path: Path, voice_id: str | None = None,
        emotion: str = "neutral", intensity: float = 0.5,
    ) -> SynthesisResult:
        payload = {
            "text": text, "voice_id": voice_id,
            "emotion": emotion, "emotion_intensity": intensity,
        }
        async with httpx.AsyncClient(timeout=self.timeout_s) as http:
            try:
                r = await http.post(f"{self.base_url}/tts", json=payload)
            except httpx.RequestError as exc:
                raise AudioError(
                    f"连不上配音服务（{self.base_url}）。\n{exc}"
                ) from exc
            r.raise_for_status()
            out_path.parent.mkdir(parents=True, exist_ok=True)
            out_path.write_bytes(r.content)
        return SynthesisResult(duration_s=probe_wav_duration(out_path), audio_path=out_path)


def probe_wav_duration(path: Path) -> float:
    """读 wav 的时长。不依赖 ffprobe，避免多一个外部依赖。"""
    try:
        with wave.open(str(path), "rb") as wf:
            return wf.getnframes() / float(wf.getframerate())
    except (wave.Error, EOFError, struct.error) as exc:
        raise AudioError(f"音频文件读不出时长：{path}\n{exc}") from exc


# 一句台词能占的最长时间。超过单镜上限的台词，配出来的音频装不进
# 任何一个镜头，混音时会盖到下一镜上去，成片里两个人同时说话。
# 留出尾巴的余量。
def max_line_seconds(fps: int = 24) -> float:
    from .render import max_shot_duration_s
    return max_shot_duration_s(fps) - TAIL_S


# 一句台词最多重切几次。估算不准是常态，但切三次还装不下的话
# 多半是引擎那边出了别的问题，再切下去只是把话剁碎。
_MAX_RESPLITS = 3

_SENTENCE_ENDS = "。！？…"
_CLAUSE_ENDS = "；，、"


def split_long_text(text: str, max_seconds: float) -> list[str]:
    """把过长的台词按标点切成几句，每句都装得进一个镜头。

    先在句末标点处切，切不够再退到逗号分号。硬切字数是最后手段，
    那样会把词切断，听起来很别扭，但总好过整句被下一镜的声音盖住。
    """
    if estimate_speech_duration(text) <= max_seconds:
        return [text]

    def chunks_by(marks: str, src: list[str]) -> list[str]:
        out: list[str] = []
        for part in src:
            if estimate_speech_duration(part) <= max_seconds:
                out.append(part)
                continue
            buf = ""
            for ch in part:
                buf += ch
                if ch in marks and estimate_speech_duration(buf) >= max_seconds * 0.6:
                    out.append(buf)
                    buf = ""
            if buf:
                out.append(buf)
        return out

    pieces = chunks_by(_SENTENCE_ENDS, [text])
    pieces = chunks_by(_CLAUSE_ENDS, pieces)

    # 还有过长的就只能按字数硬切。每一段都要自带头尾的呼吸留白，
    # 算字数时先把这部分扣掉，否则切出来的每一段都刚好超一点。
    speakable = max(0.5, max_seconds - LEAD_IN_S - TAIL_S)
    take = max(1, int(speakable * CHARS_PER_SECOND))
    final: list[str] = []
    for piece in pieces:
        while estimate_speech_duration(piece) > max_seconds:
            final.append(piece[:take])
            piece = piece[take:]
        if piece:
            final.append(piece)
    return [p.strip() for p in final if p.strip()]


@dataclass
class ShotAudioPlan:
    """一个镜头的配音结果与时长决策。"""

    shot_id: str
    speech_duration_s: float
    locked_duration_s: float
    slack_s: float
    lines: int

    @property
    def is_tight(self) -> bool:
        """留白小于半秒。这类镜头在装配时不能再压缩。"""
        return self.slack_s < 0.5


def _free_shot_id(base: str, used: set[str]) -> str:
    """给拆出来的新镜取一个全集没用过的编号。"""
    for suffix in "bcdefghijklmnopqrstuvwxyz":
        candidate = f"{base}_{suffix}"
        if candidate not in used:
            return candidate
    n = 2
    while f"{base}_{n}" in used:
        n += 1
    return f"{base}_{n}"


def split_overlong_shots(shots: list[Shot], max_seconds: float) -> list[Shot]:
    """把装不下自己台词的镜头拆成连着的几镜。

    单镜时长有硬上限，来自视频模型能生成的最大帧数。一个镜头里塞了
    两三句台词，配出来七八秒，镜头只有五秒，混音时后面的声音就盖到
    下一镜上去了，成片里两个人同时说话。

    在配音之后、出首帧之前拆。这时候音频已经有了，画面还没生成，
    拆开不浪费任何一次渲染。原镜留着第一组台词和已有的音频，
    后面几组各起一个新镜从头生成。
    """
    # 编号要跟全集比对着发。同一集重跑一次配音会再拆一次，
    # 只按本次的序号取名的话，第二次又会取出一个 sh001_b，
    # 于是一集里出现两个同名镜头：按 id 找镜头只能找到头一个，
    # 音频和首帧的文件名也会互相覆盖。
    used = {s.shot_id for s in shots}
    out: list[Shot] = []
    for shot in sorted(shots, key=lambda x: x.order):
        groups = _group_lines(shot, max_seconds)
        if len(groups) <= 1:
            out.append(shot)
            continue
        shot.dialogue = groups[0]
        out.append(shot)
        for group in groups[1:]:
            extra = shot.model_copy(deep=True)
            extra.shot_id = _free_shot_id(shot.shot_id, used)
            used.add(extra.shot_id)
            extra.dialogue = group
            # 新镜是全新的画面，之前那一镜的产物一概不能继承
            extra.frame_path = None
            extra.video_path = None
            extra.attempts = 0
            extra.gate_notes = []
            extra.status = ShotStatus.PLANNED
            extra.duration_locked = False
            out.append(extra)
    # order 是整数，拆完统一重排一遍
    for i, shot in enumerate(out):
        shot.order = i
    return out


def _group_lines(shot: Shot, max_seconds: float) -> list[list[DialogueLine]]:
    """把一个镜头的台词按时长打包，每包都装得进一个镜头。"""
    if len(shot.dialogue) <= 1:
        return [list(shot.dialogue)]

    def dur(line: DialogueLine) -> float:
        if line.actual_duration_s is not None:
            return line.actual_duration_s
        return estimate_speech_duration(line.text)

    budget = max(0.5, max_seconds - TAIL_S)
    groups: list[list[DialogueLine]] = []
    current: list[DialogueLine] = []
    total = 0.0
    for line in shot.dialogue:
        d = dur(line)
        if current and total + d > budget:
            groups.append(current)
            current, total = [], 0.0
        current.append(line)
        total += d
    if current:
        groups.append(current)
    return groups


class AudioStage:
    """跑配音并锁定镜头时长。"""

    def __init__(self, backend: TTSBackend, config: TTSConfig, paths: ProjectPaths) -> None:
        self.backend = backend
        self.config = config
        self.paths = paths
        self._voices: list[str] | None = None
        # 服务端不认的音色。收集起来报给用户，不然自动换了声音他不知道。
        self.unknown_voices: set[str] = set()

    async def run(
        self, shots: list[Shot], assets: AssetLibrary,
        concurrency: int = 2,
        on_shot: Callable[[ShotAudioPlan, int, int], None] | None = None,
    ) -> list[ShotAudioPlan]:
        """给所有镜头配音，然后反推锁定时长。

        on_shot 每配完一个镜头回调一次。整段配音要跑好几分钟，
        不报进度的话界面就是一根不动的进度条，看着像是卡死了。
        """
        sem = asyncio.Semaphore(max(1, concurrency))
        done = 0
        total = len(shots)

        async def one(shot: Shot) -> ShotAudioPlan:
            nonlocal done
            async with sem:
                plan = await self._process_shot(shot, assets)
            done += 1
            if on_shot:
                on_shot(plan, done, total)
            return plan

        return list(await asyncio.gather(*(one(s) for s in shots)))

    async def _process_shot(self, shot: Shot, assets: AssetLibrary) -> ShotAudioPlan:
        # 过长的台词先按字数估一遍切开。估算只是第一道，
        # 合成出来的真实时长常常比估的长两成，所以合成之后还要再验一次。
        self._split_long_lines(shot)
        limit = max_line_seconds()

        done: list[DialogueLine] = []
        queue = list(shot.dialogue)
        idx = 0
        splits = 0
        while queue:
            line = queue.pop(0)
            voice = await self._voice_for(line, assets)
            out = self.paths.audio / f"{shot.shot_id}_{idx:02d}.wav"
            result = await self.backend.synthesize(
                line.text, out, voice, line.emotion, line.emotion_intensity,
            )

            # 合成出来还是装不下，按实测语速重切一次再合成。
            # 光靠估算不行：估的是每秒 4.6 个字，不同引擎不同音色差得很远，
            # 实测有过估 4.8 秒、出来 5.8 秒的，那 1 秒就盖到下一镜上了。
            if (result.duration_s > limit and splits < _MAX_RESPLITS
                    and len(line.text) > 6):
                # 估算偏了多少就把预算缩多少。估 E 秒实际 D 秒，
                # 想让实际落到 limit，就得按 limit * E / D 去估。
                # 再留一成余量，免得来回重切。
                scaled = limit * estimate_speech_duration(line.text) \
                    / result.duration_s * 0.9
                pieces = split_long_text(line.text, max(0.8, scaled))
                if len(pieces) > 1:
                    splits += 1
                    for piece in reversed(pieces):
                        clone = line.model_copy(deep=True)
                        clone.text = piece
                        clone.audio_path = None
                        clone.actual_duration_s = None
                        queue.insert(0, clone)
                    continue

            line.actual_duration_s = round(result.duration_s, 3)
            if result.audio_path is not None:
                line.audio_path = self.paths.rel(result.audio_path)
            line.voice_id = voice
            done.append(line)
            idx += 1

        shot.dialogue = done
        total = sum(x.actual_duration_s or 0.0 for x in done)
        locked = self._lock_duration(shot, total)
        return ShotAudioPlan(
            shot_id=shot.shot_id,
            speech_duration_s=round(total, 3),
            locked_duration_s=locked,
            slack_s=round(locked - total, 3),
            lines=len(shot.dialogue),
        )

    @staticmethod
    def _split_long_lines(shot: Shot) -> None:
        limit = max_line_seconds()
        out: list[DialogueLine] = []
        for line in shot.dialogue:
            pieces = split_long_text(line.text, limit)
            if len(pieces) == 1:
                out.append(line)
                continue
            for piece in pieces:
                clone = line.model_copy(deep=True)
                clone.text = piece
                # 切开之后原来那份音频对不上了，清掉重新合成
                clone.audio_path = None
                clone.actual_duration_s = None
                out.append(clone)
        shot.dialogue = out

    async def _voice_for(
        self, line: DialogueLine, assets: AssetLibrary,
    ) -> str | None:
        """台词的音色由角色资产决定，不由分镜决定。

        角色上填了就用填的，那是人工指定的，不该被自动挑选覆盖。
        没填就按性别从服务端实际提供的列表里挑一条。
        """
        char = assets.characters.get(line.char_id or "")
        available = await self._available_voices()
        wanted = char.voice_id if char is not None else line.voice_id

        if not available:
            # 问不到列表就别自作主张，照填的来
            return wanted

        if wanted:
            if wanted in available:
                return wanted
            # 老项目里存的可能是 v_角色名 这种早年自造的 id，服务端不认。
            # 原样提交上去节点会拒绝，整条流水线断在配音这一步。
            # 这种情况退回自动挑选，让老项目还能跑，而不是让人先去
            # 挨个改一遍音色。
            self.unknown_voices.add(wanted)

        if char is None:
            return pick_voice(available, "", 0)  # 旁白
        return pick_voice(available, char.voice_gender, char.voice_order)

    async def _available_voices(self) -> list[str]:
        """服务端这台机器上到底有哪些参考音色。查一次就够，缓存住。"""
        if self._voices is None:
            lister = getattr(self.backend, "list_voices", None)
            self._voices = list(await lister()) if lister else []
        return self._voices

    def _lock_duration(self, shot: Shot, speech_s: float) -> float:
        """由配音时长反推镜头时长。

        没有台词的镜头保持分镜给的时长不动，它们是节奏调节的余量。
        有台词的镜头向上吸附到可生成档位并锁定，宁长勿短，
        短了会截断台词，长了尾巴上留一点表演余韵反而自然。
        """
        if not shot.dialogue:
            return shot.duration_s
        needed = speech_s + TAIL_S
        locked = ceil_duration(needed)
        shot.duration_s = locked
        shot.duration_locked = True
        return locked


class ComfyTTSBackend:
    """通过 ComfyUI 的 TTS 节点配音。

    好处是推理都集中在装了显卡和 PyTorch 的那台机器上，编排引擎
    不必背推理框架的依赖。工作流由用户提供，放在项目的
    workflows/tts.json，这样换引擎只换工作流不改代码。
    """

    name = "comfy"

    def __init__(self, client, workflow, paths: ProjectPaths) -> None:
        self.client = client
        self.workflow = workflow
        self.paths = paths

    async def available(self) -> bool:
        return await self.client.ping()

    async def list_voices(self) -> list[str]:
        """服务端这台机器上有哪些参考音色。

        音色在节点里是个下拉框，装了哪些插件就有哪些选项。
        提交一条不在列表里的路径，节点会直接拒绝，整条流水线断在这。
        """
        for node in self.workflow.prompt.values():
            cls = node.get("class_type")
            if not cls:
                continue
            for key in _VOICE_KEYS:
                if key in node.get("inputs", {}):
                    try:
                        return await self.client.available_models(cls, key)
                    except Exception:
                        return []
        return []

    async def synthesize(
        self, text: str, out_path: Path, voice_id: str | None = None,
        emotion: str = "neutral", intensity: float = 0.5,
    ) -> SynthesisResult:
        from ..comfy.client import PromptValidationError
        from ..comfy.workflow import WorkflowError

        wf = self.workflow.copy()
        applied = _apply_text(wf, text, voice_id, emotion)
        if not applied:
            raise AudioError(
                "配音工作流里找不到可以填文本的节点。"
                "请确认 workflows/tts.json 里有一个文本输入节点"
            )
        try:
            wf.set_by_class("SaveAudio", filename_prefix=f"changji/{out_path.stem}")
        except WorkflowError:
            pass  # 有些工作流用别的保存节点

        try:
            result = await self.client.run(wf)
        except PromptValidationError as exc:
            raise AudioError(f"配音工作流被拒绝：\n{exc.human_summary()}") from exc

        ref = None
        for kind in ("audio", "audios", "images"):
            files = result.files(kind)
            if files:
                ref = files[0]
                break
        if ref is None:
            raise AudioError("配音完成但没有产出音频文件")

        out_path.parent.mkdir(parents=True, exist_ok=True)
        await self.client.download(ref, out_path)

        duration = probe_audio_duration(out_path)
        _reject_silent_audio(out_path, duration, text)
        return SynthesisResult(duration_s=duration, audio_path=out_path)


# 常见 TTS 节点里放文本的键名。不同引擎叫法不同，逐个试。
_TEXT_KEYS = ("text", "prompt", "input_text", "tts_text", "content")
_VOICE_KEYS = ("narrator_voice", "voice", "voice_id", "speaker",
               "reference_audio", "speaker_id")
_EMOTION_KEYS = ("emotion", "emo", "style", "instruct")


def _apply_text(wf, text: str, voice_id: str | None, emotion: str) -> bool:
    """把台词填进工作流。

    节点类型和参数名因引擎而异，所以按键名匹配而不是按节点类型。
    """
    filled = False
    for node in wf.prompt.values():
        inputs = node.setdefault("inputs", {})
        for key in _TEXT_KEYS:
            if key in inputs and isinstance(inputs[key], str):
                inputs[key] = text
                filled = True
                break
        if voice_id:
            for key in _VOICE_KEYS:
                if key in inputs and isinstance(inputs[key], str):
                    inputs[key] = voice_id
                    break
        if emotion and emotion != "neutral":
            for key in _EMOTION_KEYS:
                if key in inputs and isinstance(inputs[key], str):
                    inputs[key] = emotion
                    break
    return filled


def probe_audio_duration(path: Path) -> float:
    """读音频时长。wav 走标准库，其它格式退回 ffprobe。"""
    if path.suffix.lower() == ".wav":
        try:
            return probe_wav_duration(path)
        except AudioError:
            pass
    import json
    import subprocess

    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-print_format", "json", str(path)],
            capture_output=True, text=True, timeout=60, check=True,
        ).stdout
        return float(json.loads(out)["format"]["duration"])
    except (subprocess.SubprocessError, OSError, KeyError, ValueError) as exc:
        raise AudioError(f"读不出音频时长：{path}\n{exc}") from exc


# 一秒以内的音频几乎不可能是一句正常台词，多半是节点失败后的占位输出
_MIN_PLAUSIBLE_DURATION_S = 1.05


def _reject_silent_audio(path: Path, duration_s: float, text: str) -> None:
    """检查产出的音频是不是真的有声音。

    这道检查存在的理由：ComfyUI 的 TTS 节点内部捕获异常后，会输出一个
    一秒的空音频然后正常返回，服务端的执行状态报的是 success。
    只信状态码的客户端会被完全骗过，拿到一堆静音文件还以为配音成功了。
    实测就撞上过这个：节点缺 librosa 报错，任务状态却是成功。

    所以不能只信状态，必须验证产出物本身。
    """
    expected = estimate_speech_duration(text)
    if duration_s >= _MIN_PLAUSIBLE_DURATION_S and duration_s >= expected * 0.35:
        return

    size = path.stat().st_size if path.is_file() else 0
    raise AudioError(
        f"配音节点返回了一个疑似空音频：时长 {duration_s:.2f} 秒、"
        f"{size} 字节，而这句台词按语速估算应有 {expected:.1f} 秒。\n"
        f"台词：{text[:30]}\n"
        f"ComfyUI 报的任务状态是成功，但节点内部很可能失败了。"
        f"去看 ComfyUI 的日志，常见原因是缺少依赖或模型没下完。"
    )


def build_backend(
    config: TTSConfig, comfy_client=None, workflow=None,
    paths: ProjectPaths | None = None,
) -> TTSBackend:
    """按配置和可用资源造后端。

    优先级：显式配的 http、项目里放了配音工作流就走 ComfyUI、
    都没有则退回估算后端让流水线能跑通。
    """
    if config.backend == "http":
        if not config.base_url:
            raise AudioError("配音后端设成了 http，但没填 tts.base_url")
        return HttpTTSBackend(config.base_url)
    if config.backend == "comfy" and comfy_client is not None \
            and workflow is not None and paths is not None:
        return ComfyTTSBackend(comfy_client, workflow, paths)
    return EstimateBackend()


def summarize(plans: list[ShotAudioPlan]) -> str:
    """配音结果概览。"""
    if not plans:
        return "没有需要配音的镜头"
    total_speech = sum(p.speech_duration_s for p in plans)
    total_locked = sum(p.locked_duration_s for p in plans)
    tight = [p for p in plans if p.is_tight and p.lines]
    lines = [
        f"配音完成 {sum(p.lines for p in plans)} 句，覆盖 {len(plans)} 个镜头",
        f"语音总长 {total_speech:.1f} 秒，锁定后镜头总长 {total_locked:.1f} 秒",
    ]
    if tight:
        lines.append(
            f"其中 {len(tight)} 个镜头留白不足半秒，装配时不能再压缩："
            + "、".join(p.shot_id for p in tight[:5])
        )
    return "\n".join(lines)
