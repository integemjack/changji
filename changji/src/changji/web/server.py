"""Web 服务。

界面的作用是审片和改分镜，这两件事命令行干不了。
后台跑流水线，前端用轮询看进度。不用 WebSocket，因为轮询在这个场景
足够了，而且少一层连接管理就少一类掉线问题。
"""

from __future__ import annotations

import asyncio
import re
import time
from collections import deque
from pathlib import Path
from typing import Any

from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, HTMLResponse
from pydantic import BaseModel, Field, ValidationError

from ..comfy.client import ComfyClient
from ..config import (
    ComfyConfig,
    LLMConfig,
    Settings,
    TTSConfig,
    env_overridden,
    save_user_config,
    user_config_path,
)
from ..doctor import run_checks
from ..hardware import HardwareProfile, Tier
from ..models.project import ProjectStore
from ..pipeline import Event, Pipeline, human_time
from .page import render_page


# 常见大模型平台的接入地址。
#
# 这些平台都提供 OpenAI 兼容接口，所以不用为每一家写适配器。列在这里只是
# 免得用户去各家文档里翻那一行 base_url——选完照样能手改。
#
# local 为真的是本机跑的服务，通常不校验密钥；其余都要去对应平台申请。
# 地址会变，各家也会加新的，填不通时以官方文档为准。
LLM_PROVIDERS: list[dict[str, Any]] = [
    {"id": "ollama", "name": "Ollama（本机）",
     "base_url": "http://127.0.0.1:11434/v1", "local": True,
     "note": "本机跑模型，不要密钥。先 ollama pull 一个模型"},
    {"id": "lmstudio", "name": "LM Studio（本机）",
     "base_url": "http://127.0.0.1:1234/v1", "local": True,
     "note": "在 LM Studio 里开启本地服务器"},
    {"id": "vllm", "name": "vLLM（本机或局域网）",
     "base_url": "http://127.0.0.1:8000/v1", "local": True,
     "note": "自建推理服务，地址按实际部署改"},
    {"id": "deepseek", "name": "DeepSeek 深度求索",
     "base_url": "https://api.deepseek.com/v1", "local": False,
     "note": "platform.deepseek.com 申请密钥"},
    {"id": "siliconflow", "name": "硅基流动 SiliconFlow",
     "base_url": "https://api.siliconflow.cn/v1", "local": False,
     "note": "聚合了很多开源模型，一个密钥都能用"},
    {"id": "dashscope", "name": "阿里云百炼（通义千问）",
     "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
     "local": False, "note": "注意是 compatible-mode 那条地址"},
    {"id": "moonshot", "name": "月之暗面 Kimi",
     "base_url": "https://api.moonshot.cn/v1", "local": False,
     "note": "platform.moonshot.cn 申请密钥"},
    {"id": "zhipu", "name": "智谱 GLM",
     "base_url": "https://open.bigmodel.cn/api/paas/v4", "local": False,
     "note": "bigmodel.cn 申请密钥"},
    {"id": "ark", "name": "火山方舟（豆包）",
     "base_url": "https://ark.cn-beijing.volces.com/api/v3", "local": False,
     "note": "模型名填推理接入点 ID，不是模型名字"},
    {"id": "hunyuan", "name": "腾讯混元",
     "base_url": "https://api.hunyuan.cloud.tencent.com/v1", "local": False,
     "note": "腾讯云控制台申请密钥"},
    {"id": "minimax", "name": "MiniMax",
     "base_url": "https://api.minimax.chat/v1", "local": False,
     "note": "platform.minimaxi.com 申请密钥"},
    {"id": "stepfun", "name": "阶跃星辰 StepFun",
     "base_url": "https://api.stepfun.com/v1", "local": False,
     "note": "platform.stepfun.com 申请密钥"},
    {"id": "lingyi", "name": "零一万物 Yi",
     "base_url": "https://api.lingyiwanwu.com/v1", "local": False,
     "note": "platform.lingyiwanwu.com 申请密钥"},
    {"id": "openai", "name": "OpenAI",
     "base_url": "https://api.openai.com/v1", "local": False,
     "note": "国内直连多半要自备网络"},
    {"id": "openrouter", "name": "OpenRouter",
     "base_url": "https://openrouter.ai/api/v1", "local": False,
     "note": "一个密钥转发到几百个模型"},
]


# 预告片挂在这个固定的集号上。写死是有意的：预告片只有一条，
# 重剪要覆盖上一条而不是攒出一堆 trailer2、trailer3。
# 它也不参与「接着前几集写」的上下文，否则正片会开始抄自己的预告。
TRAILER_EPISODE_ID = "trailer"


class RunState:
    """一次运行的实时状态。界面轮询这个。"""

    def __init__(self) -> None:
        self.running = False
        self.episode_id: str | None = None
        self.started_at: float = 0.0
        self.events: deque[dict[str, Any]] = deque(maxlen=500)
        self.stage: str = ""
        self.current = 0
        self.total = 0
        self.message = ""
        self.output: str | None = None
        self.outputs: list[str] = []
        self.error: str | None = None
        self.task: asyncio.Task | None = None
        # 一次跑多集时的队列进度。只跑一集时是 1/1。
        self.queue_done = 0
        self.queue_total = 1

    def record(self, event: Event) -> None:
        self.stage = event.stage.value
        if event.total:
            self.current, self.total = event.current, event.total
        self.message = event.message
        self.events.append({
            "at": round(time.time(), 3),
            "stage": event.stage.value,
            "kind": event.kind,
            "message": event.message,
            "shot_id": event.shot_id,
            "current": event.current,
            "total": event.total,
        })

    def snapshot(self) -> dict[str, Any]:
        return {
            "running": self.running,
            "episode_id": self.episode_id,
            "stage": self.stage,
            "current": self.current,
            "total": self.total,
            "message": self.message,
            "elapsed_s": round(time.monotonic() - self.started_at, 1)
            if self.started_at else 0,
            "output": self.output,
            "outputs": list(self.outputs),
            "queue_done": self.queue_done,
            "queue_total": self.queue_total,
            "error": self.error,
            "events": list(self.events)[-80:],
        }


class WriteState:
    """写整季时的进度。跟跑流水线分开，两件事可以同时进行。"""

    def __init__(self) -> None:
        self.running = False
        self.done = 0
        self.total = 0
        self.message = ""
        self.episodes: list[dict[str, Any]] = []
        self.error: str | None = None
        self.task: asyncio.Task | None = None

    def snapshot(self) -> dict[str, Any]:
        return {
            "running": self.running,
            "done": self.done,
            "total": self.total,
            "message": self.message,
            "episodes": list(self.episodes),
            "error": self.error,
        }


class RunRequest(BaseModel):
    """跑流水线。

    stages 为空表示跑全流程。给了就只跑指定的阶段，这样可以单独重做
    某一段，比如只重出首帧而不重跑配音，或者改完分镜只重渲染。
    """

    project: str
    episode_id: str
    skip_final: bool = False
    force: bool = False
    stages: list[str] = Field(default_factory=list)
    # 一次排完整个项目。做的就是量产，一集一集手点没有意义。
    # 给了这个就忽略 episode_id。
    all_episodes: bool = False


class ShotPatch(BaseModel):
    """改一个镜头。只列可以人工改的字段。

    角色外观依然不在这里，一致性靠资产库统一管理这一点不因为
    加了编辑功能就松动。
    """

    model_config = {"extra": "forbid"}

    first_frame_prompt: str | None = None
    motion_prompt: str | None = None
    negative_prompt: str | None = None
    visual_desc: str | None = None
    shot_size: str | None = None
    camera_angle: str | None = None
    camera_move: str | None = None
    duration_s: float | None = None
    transition_in: str | None = None
    transition_dur_s: float | None = None
    subtitle_text: str | None = None
    beat: str | None = None
    needs_lipsync: bool | None = None
    status: str | None = None
    dialogue_texts: list[str] | None = None


class ShotUpdateRequest(BaseModel):
    project: str
    episode_id: str
    shot_id: str
    patch: ShotPatch


class CharacterPatch(BaseModel):
    """改角色。外观五段是一致性的锚点，改了会影响所有引用它的镜头。"""

    model_config = {"extra": "forbid"}

    name: str | None = None
    identity: str | None = None
    body: str | None = None
    face: str | None = None
    attire: str | None = None
    style: str | None = None
    voice_id: str | None = None
    lora_trigger: str | None = None
    lora_strength: float | None = None


class CharacterUpdateRequest(BaseModel):
    project: str
    char_id: str
    patch: CharacterPatch
    reset_shots: bool = True


class ClearReferenceRequest(BaseModel):
    model_config = {"extra": "forbid"}

    project: str
    char_id: str
    slot: str


class ReorderShotsRequest(BaseModel):
    """重排镜头顺序。

    分镜出来之后想调节奏，最常做的事就是把某一镜往前挪。以前只能改
    order 字段——而那个字段在编辑器里没有露出来，等于做不了。
    """

    model_config = {"extra": "forbid"}

    project: str
    episode_id: str
    shot_ids: list[str] = Field(description="按新顺序排的完整镜头 id 列表")


class LinkLocationsRequest(BaseModel):
    """把 location_id 空着的镜头接回场景。留空集号表示整个项目都补。"""

    model_config = {"extra": "forbid"}

    project: str
    episode_id: str = ""


class ClearLocationReferenceRequest(BaseModel):
    """撤掉一个场景的空景图。"""

    model_config = {"extra": "forbid"}

    project: str
    location_id: str


class LocationPatch(BaseModel):
    model_config = {"extra": "forbid"}

    name: str | None = None
    space: str | None = None
    lighting: str | None = None
    palette: str | None = None


class LocationUpdateRequest(BaseModel):
    project: str
    location_id: str
    patch: LocationPatch
    reset_shots: bool = True


class StylePatch(BaseModel):
    """全剧风格层。所有镜头都会带上它。"""

    model_config = {"extra": "forbid"}

    global_style: str | None = None
    negative_prompt: str | None = None
    aspect_ratio: str | None = None


class StyleUpdateRequest(BaseModel):
    project: str
    patch: StylePatch
    reset_shots: bool = False


class NewEpisodeRequest(BaseModel):
    """新建一集。量产时一个项目会有很多集，共用同一套角色和场景设定。"""

    project: str
    episode_id: str = ""
    title: str = ""
    target_duration_s: float = 60.0


class EpisodeActionRequest(BaseModel):
    project: str
    episode_id: str
    action: str  # delete / duplicate / rename
    new_title: str = ""


class BatchShotRequest(BaseModel):
    """批量改镜头状态。

    量产时一集几十个镜头，一个个点太慢。常见操作是把一批不满意的
    重置去重跑，或者把满意的锁住不再动。
    """

    project: str
    episode_id: str
    shot_ids: list[str] = Field(default_factory=list)
    action: str  # reset / lock / unlock / clear_notes


class ScriptUpdateRequest(BaseModel):
    """改剧本。可以选择是否重出分镜。"""

    project: str
    episode_id: str
    script: str
    regenerate: bool = False
    duration_s: float | None = None


class SettingsPatch(BaseModel):
    """运行参数。改完只影响本次运行，不写回配置文件。"""

    model_config = {"extra": "forbid"}

    draft_width: int | None = None
    draft_height: int | None = None
    draft_steps: int | None = None
    final_width: int | None = None
    final_height: int | None = None
    final_steps: int | None = None
    fps: int | None = None
    crf: int | None = None
    subtitle_font: str | None = None
    subtitle_max_chars_per_line: int | None = None
    scene_transition_s: float | None = None
    subtitle_max_lines: int | None = None
    max_attempts_per_shot: int | None = None
    gates_enabled: bool | None = None
    min_pixel_std: float | None = None
    min_frame_similarity: float | None = None
    max_audio_drift_s: float | None = None
    target_lufs: float | None = None
    fallback_on_exhausted: bool | None = None
    tts_tolerance_s: float | None = None
    tts_max_tempo_shift: float | None = None


class SettingsRequest(BaseModel):
    """改运行参数，可以选择写不写回配置文件。"""

    model_config = {"extra": "forbid"}

    patch: SettingsPatch
    persist: bool = Field(
        default=False,
        description="写回用户全局配置。不写的话重启就退回默认值",
    )


class ConnectionsPatch(BaseModel):
    """连接设置。改这些等于换一台干活的机器。"""

    model_config = {"extra": "forbid"}

    comfy_base_url: str | None = None
    comfy_job_timeout_s: float | None = None
    comfy_max_retries: int | None = None
    llm_base_url: str | None = None
    llm_model: str | None = None
    llm_api_key: str | None = None
    llm_temperature: float | None = None
    tts_backend: str | None = None
    tts_base_url: str | None = None
    tts_engine: str | None = None
    vram_gb_override: float | None = None


class ConnectionsRequest(BaseModel):
    model_config = {"extra": "forbid"}

    patch: ConnectionsPatch
    persist: bool = Field(
        default=True,
        description="写回用户全局配置。不写的话重启就没了",
    )


class DeleteProjectRequest(BaseModel):
    """删项目。整个目录连同素材成片一起没，所以门槛设得高一点。"""

    model_config = {"extra": "forbid"}

    path: str
    confirm_name: str = Field(
        description="必须和项目目录名一字不差，防手滑",
    )


class PremiseRequest(BaseModel):
    """只改项目的梗概。

    以前界面上要存一句梗概，得借道 /api/script/write——那个接口会顺手
    把梗概写进项目，但代价是让大模型完整写一集，几十秒起步，大模型没起来
    时还直接失败。存一行字不该是这个价钱。
    """

    model_config = {"extra": "forbid"}

    project: str
    premise: str = Field(default="", max_length=2000)


class NewProjectRequest(BaseModel):
    path: str
    title: str = ""
    style_line: str = "realistic"


class WriteScriptRequest(BaseModel):
    """让 AI 写一集剧本。

    写完不落库，回给界面让人先看先改。剧本是整条流水线的源头，
    源头没审过就往下跑，后面几十分钟的渲染全是白跑。
    """

    model_config = {"extra": "forbid"}

    project: str
    episode_id: str = ""
    premise: str
    duration_s: float = Field(default=60.0, gt=0, le=1800)
    continue_from_previous: bool = Field(
        default=True, description="把前面几集当上下文，接着往下写")
    reuse_characters: bool = Field(
        default=True, description="沿用项目里已有的角色，名字不变")


class PremiseIdeaRequest(BaseModel):
    """让大模型想几个选题。

    「这部剧讲什么」是整条流水线的源头，也是最难从零开始的一步。
    别处每一步都能让 AI 代劳，唯独这里要人对着空白框发呆，说不过去。
    """

    model_config = {"extra": "forbid"}

    project: str
    keywords: str = Field(default="", max_length=200,
                          description="想往哪个方向。留空就让它自由发挥")
    count: int = Field(default=3, ge=3, le=5)


class TrailerRequest(BaseModel):
    """剪一条预告片。

    对流水线来说预告片就是特别短的一集：出分镜、配音、装配全走原来那条路。
    区别只在写的时候——预告片要的是钩子不是完整故事，所以另一套提示词。
    """

    model_config = {"extra": "forbid"}

    project: str
    duration_s: float = Field(default=20.0, gt=0, le=120)
    episode_ids: list[str] = Field(
        default_factory=list,
        description="从哪几集里挑素材。留空表示所有已写好的集",
    )
    reuse_characters: bool = True


class WriteSeriesRequest(BaseModel):
    """一口气写好几集。

    量产的前半段。一集一集手点写、手点新建，写到第五集人就放弃了。
    """

    model_config = {"extra": "forbid"}

    project: str
    premise: str
    episodes: int = Field(default=3, ge=1, le=20)
    duration_s: float = Field(default=60.0, gt=0, le=1800)
    reuse_characters: bool = True


class PlanAllRequest(BaseModel):
    """给还没分镜的剧集批量出分镜。

    连着写了五集之后，每一集都还得单独点一次「重出分镜」。
    五次里漏掉一次，跑整个项目时那一集就被跳过去了。
    """

    model_config = {"extra": "forbid"}

    project: str
    overwrite: bool = Field(
        default=False,
        description="连已经有分镜的也重出。默认只补空的，"
                    "免得把人工改过的分镜冲掉",
    )


class PlanRequest(BaseModel):
    """从剧本出角色设定和分镜表。"""

    project: str
    script: str
    episode_id: str = "ep01"
    duration_s: float = 60.0
    regenerate_bible: bool = False


class BibleRequest(BaseModel):
    """只出角色与场景设定，不碰分镜表。

    引导式界面把角色、场景、分镜拆成了三步，每一步都要能单独重来。
    只有 /api/plan 一个入口的话，想重出一次角色设定就会把人工改过的
    整张分镜表一起冲掉。
    """

    model_config = {"extra": "forbid"}

    project: str
    episode_id: str = ""
    script: str = Field(default="", description="留空则用这一集已存的剧本")
    overwrite: bool = Field(
        default=False,
        description="同名的角色和场景用新出的顶掉旧的。默认只补新的，"
                    "手改过的设定和传过的参考图都保住",
    )


def create_app(settings: Settings, default_project: Path | None = None) -> FastAPI:
    app = FastAPI(title="场记", docs_url=None, redoc_url=None)
    state = RunState()
    writing = WriteState()
    app.state.run = state  # 测试要能翻这个状态位
    app.state.write = writing

    @app.get("/", response_class=HTMLResponse)
    async def index() -> str:
        return render_page(
            comfy_url=settings.comfy.base_url,
            default_project=str(default_project) if default_project else "",
        )

    @app.get("/api/health")
    async def health() -> dict[str, Any]:
        return {"ok": True, "service": "changji"}

    @app.get("/api/doctor")
    async def doctor() -> dict[str, Any]:
        report = await run_checks(settings)
        return {
            "can_run": report.can_run,
            "checks": [
                {"name": c.name, "level": c.level.value,
                 "detail": c.detail, "fix": c.fix}
                for c in report.checks
            ],
        }

    @app.get("/api/connections")
    async def get_connections() -> dict[str, Any]:
        """当前连的是哪几台机器。api_key 不回传明文。"""
        key = settings.llm.api_key
        return {
            "comfy_base_url": settings.comfy.base_url,
            "comfy_job_timeout_s": settings.comfy.job_timeout_s,
            "comfy_max_retries": settings.comfy.max_retries,
            "llm_base_url": settings.llm.base_url,
            "llm_model": settings.llm.model,
            "llm_api_key_set": bool(key),
            "llm_api_key_hint": (key[:2] + "***" + key[-2:]) if len(key) > 6 else "***",
            "llm_temperature": settings.llm.temperature,
            "tts_backend": settings.tts.backend,
            "tts_base_url": settings.tts.base_url or "",
            "tts_engine": settings.tts.engine,
            "vram_gb_override": settings.vram_gb_override,
            "config_file": str(user_config_path()),
            # 被环境变量顶住的字段，改了也是白改，界面要说出来
            "env_locked": env_overridden(),
        }

    @app.post("/api/connections")
    async def update_connections(req: ConnectionsRequest) -> dict[str, Any]:
        """改连接设置，然后立刻重新体检。

        默认写回用户全局配置。连接是机器级别的事，不是某一集的事，
        改完重启还得再改一遍才是真的难用。
        """
        if state.running:
            raise HTTPException(409, "正在跑，这时候换机器会把这一集跑坏")

        data = req.patch.model_dump(exclude_none=True)
        if not data:
            raise HTTPException(400, "没有要改的项")

        comfy = settings.comfy.model_dump()
        llm = settings.llm.model_dump()
        tts = settings.tts.model_dump()
        changed: list[str] = []
        for key, value in data.items():
            if key == "vram_gb_override":
                continue
            section, _, field = key.partition("_")
            target = {"comfy": comfy, "llm": llm, "tts": tts}[section]
            if target.get(field) != value:
                changed.append(key)
            target[field] = value
        if "tts_base_url" in data and not data["tts_base_url"]:
            tts["base_url"] = None

        try:
            new_comfy = ComfyConfig.model_validate(comfy)
            new_llm = LLMConfig.model_validate(llm)
            new_tts = TTSConfig.model_validate(tts)
        except ValidationError as exc:
            raise HTTPException(400, _readable(exc)) from exc

        if new_tts.backend not in ("comfy", "http"):
            raise HTTPException(400, "配音后端只能是 comfy 或 http")
        if new_tts.backend == "http" and not new_tts.base_url:
            raise HTTPException(400, "配音后端选 http 就必须填地址")

        settings.comfy = new_comfy
        settings.llm = new_llm
        settings.tts = new_tts
        if "vram_gb_override" in data:
            if settings.vram_gb_override != data["vram_gb_override"]:
                changed.append("vram_gb_override")
            settings.vram_gb_override = data["vram_gb_override"] or None

        saved_to = None
        if req.persist and changed:
            payload: dict[str, Any] = {}
            for key in changed:
                if key == "vram_gb_override":
                    payload["vram_gb_override"] = settings.vram_gb_override
                    continue
                section, _, field = key.partition("_")
                value = {"comfy": new_comfy, "llm": new_llm,
                         "tts": new_tts}[section].model_dump()[field]
                payload.setdefault(section, {})[field] = "" if value is None else value
            try:
                saved_to = str(save_user_config(payload))
            except OSError as exc:
                raise HTTPException(500, f"配置写不进去：{exc}") from exc

        report = await run_checks(settings)
        locked = env_overridden()
        return {
            "changed": changed,
            "labels": _labels(changed),
            "saved_to": saved_to,
            # 写进文件了，但下次启动环境变量还是会把它顶回去
            "env_locked": sorted(k for k in changed if k in locked),
            "can_run": report.can_run,
            "checks": [
                {"name": c.name, "level": c.level.value,
                 "detail": c.detail, "fix": c.fix}
                for c in report.checks
            ],
        }

    @app.get("/api/llm/providers")
    async def llm_providers() -> dict[str, Any]:
        """常见大模型平台的接入地址。

        这些平台都提供 OpenAI 兼容接口，所以场记不用为每一家写一个适配器
        ——填对地址和密钥就能用。列出来只是免得用户去翻各家文档找那一行
        base_url，选完仍然可以手改，这里不锁死任何东西。

        地址会变，各家也会加新的。当成一份「省得查文档」的清单看，
        真填不通的时候以官方文档为准。
        """
        return {"providers": LLM_PROVIDERS}

    @app.get("/api/llm/models")
    async def llm_models() -> dict[str, Any]:
        """那台大模型服务上都有哪些模型。

        模型名以前只能手打。打错了要跑到写剧本那一步才报错，而报出来的
        是一个 404——用户看不出是地址错了还是名字错了。列表拉过来给人选，
        这类错就没机会发生。

        OpenAI 兼容接口一律有 GET /models，Ollama 也有。问不到就返回空
        列表并说明原因，界面退回手打，不至于因为列不出来就没法填。
        """
        import httpx

        url = f"{settings.llm.base_url}/models"
        try:
            async with httpx.AsyncClient(timeout=10.0) as http:
                r = await http.get(
                    url,
                    headers={"Authorization": f"Bearer {settings.llm.api_key}"},
                )
        except httpx.RequestError as exc:
            return {"models": [], "error": f"连不上 {url}：{exc}"}
        if r.status_code >= 400:
            return {"models": [], "error": f"{url} 返回 {r.status_code}"}
        try:
            body = r.json()
        except Exception:
            return {"models": [], "error": f"{url} 返回的不是 JSON"}

        items = body.get("data") if isinstance(body, dict) else body
        names = []
        for item in items or []:
            name = item.get("id") if isinstance(item, dict) else str(item)
            if name:
                names.append(str(name))
        return {"models": sorted(set(names)), "current": settings.llm.model}

    @app.get("/api/voices")
    async def voices(path: str) -> dict[str, Any]:
        """服务端有哪些参考音色。

        音色在 ComfyUI 那边是个下拉框，装了哪些插件就有哪些选项。
        界面上得让人从这个列表里选，而不是手打一条路径然后在
        跑到配音那一步才发现填错了。
        """
        from ..stages.audio import ComfyTTSBackend
        from ..workflows_loader import load_workflow

        client = ComfyClient(settings.comfy)
        # 项目里放了自己的 tts.json 就用项目的，音色列表跟着它走
        try:
            wf = await load_workflow(client, _store(path), "tts")
        except Exception as exc:
            return {"voices": [], "error": f"读不到配音工作流：{exc}"}
        backend = ComfyTTSBackend(client, wf, None)
        try:
            names = await backend.list_voices()
        except Exception as exc:
            return {"voices": [], "error": f"问不到服务端：{exc}"}
        return {"voices": [v for v in names if v and v != "none"]}

    @app.get("/api/hardware")
    async def hardware() -> dict[str, Any]:
        p = HardwareProfile.detect(settings.vram_gb_override)
        return {
            "gpu": p.gpu.name if p.gpu else None,
            "vram_gb": round(p.vram_gb, 1),
            "detected": p.detected,
            "tiers": {
                t.value: {
                    "width": p.tiers[t].width,
                    "height": p.tiers[t].height,
                    "steps": p.tiers[t].steps,
                    "seconds": p.tiers[t].measured_seconds,
                }
                for t in Tier
            },
        }

    @app.get("/api/project")
    async def project(path: str) -> dict[str, Any]:
        store = _store(path)
        try:
            p = store.load_project()
            assets = store.load_assets()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc
        return {
            "project_id": p.project_id,
            "title": p.title,
            "style_line": p.style_line.value,
            # 这部剧讲什么。剧本页拿它回填梗概框，不用凭记忆重打。
            "premise": p.premise,
            "root": str(store.root),
            "characters": [
                {"char_id": c.char_id, "name": c.name}
                for c in assets.characters.values()
            ],
            "locations": [
                {"location_id": loc.location_id, "name": loc.name}
                for loc in assets.locations.values()
            ],
            "episodes": [
                {
                    "episode_id": e.episode_id,
                    "title": e.title,
                    "synopsis": e.synopsis,
                    "shots": len(e.shots),
                    "duration_s": round(e.planned_duration_s(), 1),
                    "status": e.counts_by_status(),
                }
                for e in p.episodes
            ],
        }

    @app.get("/api/shots")
    async def shots(path: str, episode_id: str) -> dict[str, Any]:
        store = _store(path)
        p = store.load_project()
        ep = p.episode_by_id(episode_id)
        if ep is None:
            raise HTTPException(404, f"没有剧集 {episode_id}")
        return {
            "shots": [
                {
                    "shot_id": s.shot_id,
                    "order": s.order,
                    "scene_id": s.scene_id,
                    # 这一集用到哪几个场景，界面靠它算。缺了的话场景页
                    # 只能把全剧的场景一股脑列出来，看不出跟本集的关系。
                    "location_id": s.location_id,
                    "char_ids": [c.char_id for c in s.characters],
                    "shot_size": s.shot_size.value,
                    "camera_angle": s.camera_angle.value,
                    "camera_move": s.camera_move.value,
                    # 编辑器要回填这些，缺了的话打开是空的，
                    # 用户以为本来就没内容，一保存就把原文清空了
                    "first_frame_prompt": s.first_frame_prompt,
                    "motion_prompt": s.motion_prompt,
                    "negative_prompt": s.negative_prompt,
                    "subtitle_text": s.subtitle_text,
                    "transition_in": s.transition_in.value,
                    "transition_dur_s": s.transition_dur_s,
                    "continuity_notes": s.continuity_notes,
                    "duration_s": s.duration_s,
                    "duration_locked": s.duration_locked,
                    "needs_lipsync": s.needs_lipsync,
                    "status": s.status.value,
                    "attempts": s.attempts,
                    "visual_desc": s.visual_desc,
                    "dialogue": [
                        {"char_id": d.char_id, "text": d.text,
                         "duration_s": d.actual_duration_s}
                        for d in s.dialogue
                    ],
                    "gate_notes": s.gate_notes,
                    "has_video": bool(s.video_path),
                    # 审片要用。只给相对路径，取文件走 /api/media，
                    # 那里做了越界检查，不会读到项目目录之外。
                    "frame_path": s.frame_path,
                    "video_path": s.video_path,
                    "audio_paths": [
                        d.audio_path for d in s.dialogue if d.audio_path
                    ],
                    "beat": s.beat,
                }
                for s in ep.sorted_shots()
            ]
        }

    @app.get("/api/media")
    async def media(path: str, rel: str):
        """取项目内的文件。做了越界检查，不能读到项目外。"""
        store = _store(path)
        try:
            target = store.paths.abs(rel)
            target.relative_to(store.root)
        except ValueError as exc:
            raise HTTPException(403, "只能访问项目目录内的文件") from exc
        if not target.is_file():
            raise HTTPException(404, "文件不存在")
        return FileResponse(target)

    @app.get("/api/projects")
    async def list_projects() -> dict[str, Any]:
        """项目库里有哪些项目。

        以前只能手打一条绝对路径。在容器里跑的时候那是
        /data/projects/剧名 这种路径，用户根本不知道该填什么。
        """
        root = settings.workspace_path()
        items: list[dict[str, Any]] = []
        if root.is_dir():
            for child in sorted(root.iterdir()):
                if not child.is_dir() or child.name.startswith("."):
                    continue
                store = ProjectStore(child)
                if not store.exists():
                    continue
                try:
                    project = store.load_project()
                except (ValueError, OSError) as exc:
                    items.append({"path": str(child), "dir": child.name,
                                  "name": child.name, "broken": str(exc)})
                    continue
                shots = sum(len(ep.shots) for ep in project.episodes)
                done = sum(
                    1 for ep in project.episodes for sh in ep.shots
                    if sh.status.value in ("final_done", "locked")
                )
                outputs = sorted(store.paths.output.glob("*.mp4")) \
                    if store.paths.output.is_dir() else []
                items.append({
                    "path": str(child),
                    # 目录名单独给，前端不该自己去切路径分隔符，
                    # 那段代码在 Windows 和容器里得写两套
                    "dir": child.name,
                    "name": project.title or child.name,
                    "style_line": project.style_line.value,
                    "episodes": len(project.episodes),
                    "shots": shots,
                    "done_shots": done,
                    "outputs": len(outputs),
                    "mtime": store.paths.project_file.stat().st_mtime
                    if store.paths.project_file.is_file() else 0,
                })
        items.sort(key=lambda x: x.get("mtime", 0), reverse=True)
        return {"workspace": str(root), "projects": items}

    @app.post("/api/project/delete")
    async def delete_project(req: DeleteProjectRequest) -> dict[str, Any]:
        """删掉一个项目，连同它的素材和成片。

        这一步不可逆，所以设了三道闸：目录必须在项目库里面、必须确实是
        一个项目、名字必须一字不差地再打一遍。少任何一道，一次误点就能
        把跑了一夜的成片全删了。
        """
        import shutil

        if state.running:
            raise HTTPException(409, "正在跑，删项目会把跑到一半的东西弄坏")

        root = settings.workspace_path().resolve()
        try:
            target = Path(req.path).expanduser().resolve()
        except OSError as exc:
            raise HTTPException(400, f"路径不对：{exc}") from exc

        # 只让删项目库里面的。别的路径可能是用户自己放在别处的项目，
        # 也可能是手滑填的系统目录，不该由这个接口负责。
        if target == root or root not in target.parents:
            raise HTTPException(
                403, f"只能删项目库 {root} 里面的项目。别处的请自己删")
        store = ProjectStore(target)
        if not store.exists():
            raise HTTPException(404, "这个目录不是一个项目")
        if req.confirm_name != target.name:
            raise HTTPException(400, f"确认名字对不上，要一字不差地填 {target.name}")

        try:
            shutil.rmtree(target)
        except OSError as exc:
            raise HTTPException(500, f"删不掉：{exc}") from exc
        return {"deleted": str(target)}

    @app.post("/api/new")
    async def new_project(req: NewProjectRequest) -> dict[str, Any]:
        """新建项目。界面上不用先跑命令行。"""
        from ..models.character import StyleLine

        try:
            line = StyleLine(req.style_line)
        except ValueError as exc:
            raise HTTPException(400, "风格线只能是 realistic 或 anime") from exc
        # 只填名字就落在项目库根目录下。让用户去猜容器里的绝对路径
        # 是没道理的，他也不知道项目库挂在哪。
        raw = req.path.strip()
        if not raw:
            raise HTTPException(400, "得给项目起个名字")
        path = Path(raw).expanduser()
        if not path.is_absolute() and len(path.parts) == 1:
            path = settings.workspace_path() / raw
        try:
            store = ProjectStore.create(
                path, _slugify(path.name), req.title or path.name, line)
        except FileExistsError as exc:
            raise HTTPException(409, str(exc)) from exc
        except OSError as exc:
            raise HTTPException(400, f"创建目录失败：{exc}") from exc
        return {"root": str(store.root)}

    @app.post("/api/project/premise")
    async def save_premise(req: PremiseRequest) -> dict[str, Any]:
        """存下这部剧讲什么。写下一集时当提示词用。"""
        store = _store(req.project)
        try:
            project = store.load_project()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc
        project.premise = req.premise.strip()[:2000]
        store.save_project(project)
        return {"premise": project.premise}

    @app.post("/api/script/write")
    async def write_script(req: WriteScriptRequest) -> dict[str, Any]:
        """写一集剧本，回给界面让人过目。不落库。"""
        from ..stages.script import ScriptError, ScriptGenerator

        store = _store(req.project)
        try:
            project = store.load_project()
            assets = store.load_assets()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        previous = ""
        if req.continue_from_previous:
            # 只取这一集之前的几集。把后面的也塞进去，模型会把还没发生的
            # 事当成已经发生的写。
            earlier: list[str] = []
            for ep in project.episodes:
                if req.episode_id and ep.episode_id == req.episode_id:
                    break
                # 预告片是从正片里剪出来的，再拿它当写正片的上下文，
                # 模型会开始抄自己的预告，越写越像宣传语
                if ep.episode_id == TRAILER_EPISODE_ID:
                    continue
                if ep.script.strip():
                    earlier.append(f"【{ep.episode_id}】\n{ep.script.strip()}")
            previous = "\n\n".join(earlier[-3:])

        names = ([c.name for c in assets.characters.values()]
                 if req.reuse_characters else None)

        try:
            draft = await ScriptGenerator(settings.llm).generate(
                req.premise, req.duration_s, project.style_line,
                previous=previous, characters=names or None)
        except ScriptError as exc:
            raise HTTPException(400, str(exc)) from exc

        # 梗概存到项目上。下次写新一集时直接回填，不用凭记忆重打。
        if project.premise != req.premise.strip():
            project.premise = req.premise.strip()[:2000]
            store.save_project(project)

        text = draft.render()
        budget = _script_budget(req.duration_s)
        return {
            "title": draft.title,
            "logline": draft.logline,
            "script": text,
            "speakers": draft.speakers,
            "dialogue_chars": draft.dialogue_chars,
            "budget_chars": budget,
            "beats": len(draft.beats),
            # 写长了后面配音会把镜头撑爆，写短了成片不够时长，都得说出来
            "fit": ("偏长" if draft.dialogue_chars > budget * 1.35
                    else "偏短" if draft.dialogue_chars < budget * 0.6
                    else "合适"),
            "continued_from": bool(previous),
            "reused_characters": names or [],
        }

    @app.post("/api/script/premise")
    async def suggest_premises(req: PremiseIdeaRequest) -> dict[str, Any]:
        """想几个选题给人挑。不落库，挑中哪个由界面再存。"""
        from ..stages.script import ScriptError, ScriptGenerator

        store = _store(req.project)
        try:
            project = store.load_project()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        # 项目上已有的梗概算一个「已经想过的方向」，避免连点两次
        # 拿回同一批。已经写了几集的话，那些也算。
        existing = [project.premise] if project.premise.strip() else []
        existing += [e.synopsis for e in project.episodes if e.synopsis.strip()]

        try:
            ideas = await ScriptGenerator(settings.llm).generate_premises(
                req.keywords, project.style_line, req.count, existing or None)
        except ScriptError as exc:
            raise HTTPException(400, str(exc)) from exc

        return {
            "ideas": [
                {"title": i.title, "premise": i.premise, "hook": i.hook}
                for i in ideas
            ],
            "style_line": project.style_line.value,
        }

    @app.post("/api/script/trailer")
    async def write_trailer(req: TrailerRequest) -> dict[str, Any]:
        """剪一条预告片。写完不落库，回给界面让人先看。"""
        from ..stages.script import ScriptError, ScriptGenerator

        store = _store(req.project)
        try:
            project = store.load_project()
            assets = store.load_assets()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        wanted = set(req.episode_ids)
        picked = [
            ep for ep in project.episodes
            if ep.script.strip()
            # 预告片自己不该当自己的素材
            and ep.episode_id != TRAILER_EPISODE_ID
            and (not wanted or ep.episode_id in wanted)
        ]
        if not picked:
            raise HTTPException(
                400, "没有可用来剪预告的剧集。先写几集正片，再回来剪预告")

        source = "\n\n".join(
            f"【{ep.episode_id} {ep.title}】\n{ep.script.strip()}" for ep in picked)
        names = ([c.name for c in assets.characters.values()]
                 if req.reuse_characters else None)

        try:
            draft = await ScriptGenerator(settings.llm).generate_trailer(
                project.premise, req.duration_s, project.style_line,
                episodes=source, characters=names or None)
        except ScriptError as exc:
            raise HTTPException(400, str(exc)) from exc

        text = draft.render()
        budget = _script_budget(req.duration_s)
        return {
            "title": draft.title,
            "logline": draft.logline,
            "script": text,
            "speakers": draft.speakers,
            "dialogue_chars": draft.dialogue_chars,
            "budget_chars": budget,
            "beats": len(draft.beats),
            # 预告片写长了比正片更要命：刷到第三秒还没看到钩子，人就划走了
            "fit": ("偏长" if draft.dialogue_chars > budget
                    else "偏短" if draft.dialogue_chars < budget * 0.35
                    else "合适"),
            "from_episodes": [ep.episode_id for ep in picked],
            "episode_id": TRAILER_EPISODE_ID,
        }

    @app.get("/api/script/series")
    async def series_status() -> dict[str, Any]:
        return writing.snapshot()

    @app.post("/api/script/series")
    async def write_series(req: WriteSeriesRequest) -> dict[str, Any]:
        """连着写好几集，边写边存。

        跟单集不同，这个会直接落库并建出剧集。一集一集手点新建再手点写，
        写到第五集人就放弃了，那量产就无从谈起。写完可以逐集再改。
        """
        if writing.running:
            raise HTTPException(409, "已经在写了")
        store = _store(req.project)
        try:
            store.load_project()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        writing.__init__()
        writing.running = True
        writing.total = req.episodes
        writing.task = asyncio.create_task(_write_series(store, req))
        return {"started": True, "total": req.episodes}

    async def _write_series(store: ProjectStore, req: WriteSeriesRequest) -> None:
        from ..models.project import Episode
        from ..stages.script import ScriptError, ScriptGenerator

        gen = ScriptGenerator(settings.llm)
        try:
            first = store.load_project()
            if first.premise != req.premise.strip():
                first.premise = req.premise.strip()[:2000]
                store.save_project(first)
            for i in range(req.episodes):
                project = store.load_project()
                assets = store.load_assets()
                names = ([c.name for c in assets.characters.values()]
                         if req.reuse_characters else None)
                # 前几集当上下文。只取最近三集，整季塞进去小模型撑不住，
                # 离得远的剧情对下一集的连贯性也没什么帮助。
                written = [f"【{ep.episode_id}】\n{ep.script.strip()}"
                           for ep in project.episodes
                           if ep.script.strip()
                           and ep.episode_id != TRAILER_EPISODE_ID]
                previous = "\n\n".join(written[-3:])
                writing.message = f"正在写第 {i + 1} 集"
                try:
                    draft = await gen.generate(
                        req.premise, req.duration_s, project.style_line,
                        previous=previous, characters=names or None)
                except ScriptError as exc:
                    # 一集写砸了不该让前面几集白写，记下来接着往下写
                    writing.episodes.append(
                        {"episode_id": "", "error": str(exc)})
                    writing.done += 1
                    continue

                episode_id = _next_episode_id(project)
                project.episodes.append(Episode(
                    episode_id=episode_id,
                    title=draft.title,
                    synopsis=draft.logline,
                    script=draft.render(),
                    target_duration_s=req.duration_s,
                ))
                store.save_project(project)
                writing.episodes.append({
                    "episode_id": episode_id,
                    "title": draft.title,
                    "logline": draft.logline,
                    "speakers": draft.speakers,
                    "dialogue_chars": draft.dialogue_chars,
                })
                writing.done += 1
            writing.message = f"写完了 {writing.done} 集"
        except asyncio.CancelledError:
            writing.error = "已手动停止。已经写好的几集留着。"
            raise
        except Exception as exc:
            writing.error = str(exc)
        finally:
            writing.running = False

    @app.post("/api/script/series/stop")
    async def stop_series() -> dict[str, Any]:
        if writing.task and not writing.task.done():
            writing.task.cancel()
            writing.running = False
            writing.error = "已手动停止。已经写好的几集留着。"
            return {"stopped": True}
        return {"stopped": False}

    @app.post("/api/bible")
    async def make_bible(req: BibleRequest) -> dict[str, Any]:
        """从剧本出角色与场景设定。分镜表原样不动。"""
        from ..stages.bible import BibleError, BibleGenerator

        store = _store(req.project)
        try:
            project = store.load_project()
            assets = store.load_assets()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        script = req.script.strip()
        if not script and req.episode_id:
            ep = project.episode_by_id(req.episode_id)
            script = ep.script.strip() if ep else ""
        if not script:
            # 没指定就用第一集有内容的剧本。角色设定是全剧共用的，
            # 拿哪一集出都行，但总得有一集写好了。
            script = next(
                (e.script.strip() for e in project.episodes if e.script.strip()), "")
        if not script:
            raise HTTPException(400, "还没有剧本，先去写一集")

        try:
            fresh = await BibleGenerator(settings.llm).generate(
                script, project.style_line)
        except BibleError as exc:
            raise HTTPException(400, str(exc)) from exc

        # 合并，不是替换。
        #
        # 角色和场景是一个全剧共用的库，第五集的场景要出的时候，前四集
        # 的场景还在里面。整个换掉的话，那些场景连同它们的空景图一起没了，
        # 而分镜表里还留着指向它们的 id，跑起来直接报「场景未注册」。
        #
        # 同名的默认保留旧的：手改过的设定、传过的参考图都挂在旧的那一份上。
        # 勾了覆盖才让新的顶掉。
        added_c, added_l, kept = [], [], 0
        for cid, char in fresh.characters.items():
            if cid in assets.characters and not req.overwrite:
                kept += 1
                continue
            if cid not in assets.characters:
                added_c.append(cid)
            assets.characters[cid] = char
        for lid, loc in fresh.locations.items():
            if lid in assets.locations and not req.overwrite:
                kept += 1
                continue
            if lid not in assets.locations:
                added_l.append(lid)
            assets.locations[lid] = loc

        store.save_assets(assets)
        # 外观变了等于全剧提示词都变了，已渲染的镜头得退回重跑。
        # 只新增没覆盖的话，老镜头用的还是原来那份设定，不用动。
        reset = _reset_all_shots(store) if req.overwrite else 0

        return {
            "added_characters": added_c,
            "added_locations": added_l,
            "kept": kept,
            "characters": [
                {"char_id": c.char_id, "name": c.name,
                 "identity": c.appearance.identity,
                 "face": c.appearance.face,
                 "attire": c.appearance.attire,
                 "is_new": c.char_id in added_c}
                for c in assets.characters.values()
            ],
            "locations": [
                {"location_id": loc.location_id, "name": loc.name,
                 "space": loc.space, "lighting": loc.lighting,
                 "is_new": loc.location_id in added_l}
                for loc in assets.locations.values()
            ],
            "reset_shots": reset,
        }

    @app.post("/api/plan")
    async def make_plan(req: PlanRequest) -> dict[str, Any]:
        """从剧本出角色设定和分镜表。渲染之前的两个阶段。"""
        from ..stages.bible import BibleError, BibleGenerator
        from ..stages.storyboard import StoryboardError, StoryboardGenerator

        if not req.script.strip():
            raise HTTPException(400, "剧本是空的")

        store = _store(req.project)
        try:
            project = store.load_project()
            assets = store.load_assets()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        # 角色设定。已有就不重做，避免覆盖用户改过的设定。
        if req.regenerate_bible or not assets.characters:
            try:
                assets = await BibleGenerator(settings.llm).generate(
                    req.script, project.style_line)
            except BibleError as exc:
                raise HTTPException(400, str(exc)) from exc
            store.save_assets(assets)

        try:
            shots = await StoryboardGenerator(settings.llm).generate(
                req.script, assets, req.episode_id, req.duration_s)
        except StoryboardError as exc:
            raise HTTPException(400, str(exc)) from exc

        ep = project.episode_by_id(req.episode_id)
        if ep is None:
            from ..models.project import Episode
            ep = Episode(episode_id=req.episode_id,
                         target_duration_s=req.duration_s)
            project.episodes.append(ep)
        ep.script = req.script
        ep.target_duration_s = req.duration_s
        ep.shots = shots
        store.save_project(project)

        return {
            "episode_id": req.episode_id,
            "shots": len(shots),
            "duration_s": round(sum(s.duration_s for s in shots), 1),
            "lipsync": sum(1 for s in shots if s.needs_lipsync),
            "characters": [
                {"char_id": c.char_id, "name": c.name,
                 "face": c.appearance.face}
                for c in assets.characters.values()
            ],
            "locations": [
                {"location_id": loc.location_id, "name": loc.name}
                for loc in assets.locations.values()
            ],
        }

    @app.post("/api/plan/all")
    async def plan_all(req: PlanAllRequest) -> dict[str, Any]:
        """把还没分镜的剧集一次补齐。"""
        if writing.running:
            raise HTTPException(409, "剧本那边还在忙")
        store = _store(req.project)
        try:
            project = store.load_project()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        todo = [ep.episode_id for ep in project.episodes
                if ep.script.strip() and (req.overwrite or not ep.shots)]
        if not todo:
            raise HTTPException(
                400, "没有需要出分镜的剧集。有剧本又没分镜的才算")

        writing.__init__()
        writing.running = True
        writing.total = len(todo)
        writing.task = asyncio.create_task(_plan_all(store, todo))
        return {"started": True, "episodes": todo}

    async def _plan_all(store: ProjectStore, todo: list[str]) -> None:
        from ..stages.bible import BibleError, BibleGenerator
        from ..stages.storyboard import StoryboardError, StoryboardGenerator

        try:
            for i, episode_id in enumerate(todo, start=1):
                writing.message = f"正在给 {episode_id} 出分镜"
                project = store.load_project()
                assets = store.load_assets()
                ep = project.episode_by_id(episode_id)
                if ep is None:
                    continue
                try:
                    # 角色设定全剧共用，第一次缺的时候补一次就够。
                    # 每集都重出的话，同一个角色前后长得不一样。
                    if not assets.characters:
                        assets = await BibleGenerator(settings.llm).generate(
                            ep.script, project.style_line)
                        store.save_assets(assets)
                    ep.shots = await StoryboardGenerator(settings.llm).generate(
                        ep.script, assets, episode_id, ep.target_duration_s)
                    store.save_project(project)
                except (BibleError, StoryboardError) as exc:
                    writing.episodes.append(
                        {"episode_id": episode_id, "error": str(exc)})
                    writing.done += 1
                    continue
                writing.episodes.append({
                    "episode_id": episode_id,
                    "title": ep.title,
                    "shots": len(ep.shots),
                    "duration_s": round(sum(s.duration_s for s in ep.shots), 1),
                })
                writing.done += 1
            writing.message = f"出完了 {writing.done} 集的分镜"
        except asyncio.CancelledError:
            writing.error = "已手动停止。已经出好的分镜留着。"
            raise
        except Exception as exc:
            writing.error = str(exc)
        finally:
            writing.running = False

    @app.get("/api/run/preview")
    async def run_preview(
        path: str, episode_id: str = "", all_episodes: bool = False,
        skip_final: bool = False, force: bool = False,
    ) -> dict[str, Any]:
        """开跑之前先说清楚这一次会做什么、大概多久。

        以前只能按下开始再看，一按就是几十分钟。哪些镜头会重做、
        总共要等多久，这两件事应该在按下去之前就知道。
        """
        from ..models.shot import ShotStatus

        store = _store(path)
        try:
            project = store.load_project()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        if all_episodes:
            episodes = [e for e in project.episodes if e.shots]
        else:
            ep = project.episode_by_id(episode_id)
            episodes = [ep] if ep is not None else []
        if not episodes:
            raise HTTPException(400, "没有可跑的剧集，先出分镜")

        profile = HardwareProfile.detect(settings.vram_gb_override)

        # 一个镜头从它现在的状态开始，会一路走完后面所有阶段。
        # 只按当前状态归到一个阶段的话，会告诉人「配音 2 镜，粗估 16 秒」，
        # 而实际上那两镜还要出首帧、跑草稿档、跑成片档，得等十几分钟。
        # 报小了的预演比没有预演更糟。
        order = ["audio", "frames", "draft", "final"]
        entry = {
            ShotStatus.PLANNED: 0,
            ShotStatus.AUDIO_DONE: 1,
            ShotStatus.FRAME_DONE: 2,
            ShotStatus.DRAFT_REJECTED: 2,
            ShotStatus.DRAFT_DONE: 3,
            ShotStatus.FINAL_REJECTED: 3,
        }
        labels = {"audio": "配音", "frames": "首帧",
                  "draft": "草稿档", "final": "成片档"}

        stages: dict[str, int] = {k: 0 for k in order}
        total_shots = 0
        for ep in episodes:
            total_shots += len(ep.shots)
            for shot in ep.shots:
                if shot.status is ShotStatus.LOCKED and not force:
                    continue
                start_at = 0 if force else entry.get(shot.status)
                if start_at is None:
                    continue  # 已完成或已降级，这一次不动它
                for name in order[start_at:]:
                    if name == "final" and skip_final:
                        continue
                    stages[name] += 1

        seconds = 0.0
        for name, n in stages.items():
            if name in ("draft", "final") and n:
                est = profile.estimate_episode(n, Tier(name))
                seconds += est or 0.0
        # 配音和首帧比渲染快得多，按经验各给一点，别报一个明显偏小的数
        seconds += stages["audio"] * 8 + stages["frames"] * 12

        return {
            "episodes": [e.episode_id for e in episodes],
            "shots": total_shots,
            "stages": [
                {"stage": k, "label": labels[k], "shots": v}
                for k, v in stages.items() if v
            ],
            "idle": not any(stages.values()),
            "estimate_s": round(seconds),
            "estimate_text": human_time(seconds) if seconds else "",
        }

    @app.get("/api/outputs")
    async def outputs(path: str) -> dict[str, Any]:
        """列出已生成的成片。审片时直接在界面里播。"""
        store = _store(path)
        if not store.paths.output.is_dir():
            return {"files": []}
        files = []
        for f in sorted(store.paths.output.glob("*.mp4"),
                        key=lambda x: x.stat().st_mtime, reverse=True):
            files.append({
                "name": f.name,
                "rel": store.paths.rel(f),
                "size_mb": round(f.stat().st_size / (1024 * 1024), 1),
                "mtime": int(f.stat().st_mtime),
            })
        return {"files": files}

    @app.get("/api/run")
    async def run_status() -> dict[str, Any]:
        return state.snapshot()

    @app.post("/api/run")
    async def start_run(req: RunRequest) -> dict[str, Any]:
        if state.running:
            raise HTTPException(409, f"已经在跑 {state.episode_id} 了")
        store = _store(req.project)
        try:
            project = store.load_project()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        if req.all_episodes:
            queue = [ep.episode_id for ep in project.episodes if ep.shots]
            if not queue:
                raise HTTPException(400, "这个项目还没有任何一集有分镜表")
        else:
            queue = [req.episode_id]

        state.__init__()  # 重置
        state.running = True
        state.episode_id = queue[0]
        state.queue_total = len(queue)
        state.started_at = time.monotonic()
        state.task = asyncio.create_task(_run(store, req, queue))
        return {"started": True, "queue": queue}

    @app.post("/api/stop")
    async def stop_run() -> dict[str, Any]:
        if state.task and not state.task.done():
            state.task.cancel()
            state.running = False
            state.error = "已手动停止。已完成的镜头会保留，下次从这里继续。"
            return {"stopped": True}
        return {"stopped": False}

    async def _run(store: ProjectStore, req: RunRequest,
                   queue: list[str]) -> None:
        """按队列一集一集跑。

        一集出错不拖垮后面几集。量产时跑一晚上，早上发现第二集挂了
        导致后面十集都没动，那这一晚上就白熬了。
        """
        errors: list[str] = []
        try:
            client = ComfyClient(settings.comfy)
            from ..workflows_loader import load_all
            wfs = await load_all(client, store)
            pipeline = Pipeline(
                store, settings, client, wfs["video"], listener=state.record,
                image_workflow=wfs["image"], tts_workflow=wfs["tts"])
            for episode_id in queue:
                state.episode_id = episode_id
                try:
                    if req.stages:
                        report = await pipeline.run_stages(
                            episode_id, req.stages, force=req.force)
                    else:
                        report = await pipeline.run(
                            episode_id, skip_final=req.skip_final,
                            force=req.force)
                except asyncio.CancelledError:
                    raise
                except Exception as exc:
                    errors.append(f"{episode_id}：{exc}")
                    state.queue_done += 1
                    continue
                if report.output:
                    state.output = str(report.output)
                    state.outputs.append(str(report.output))
                if report.errors:
                    errors.append(f"{episode_id}：{'；'.join(report.errors)}")
                state.queue_done += 1
            if errors:
                state.error = "；".join(errors)
        except asyncio.CancelledError:
            state.error = "已手动停止。已完成的镜头会保留，下次从这里继续。"
            raise
        except Exception as exc:
            state.error = str(exc)
        finally:
            state.running = False

    # ---- 编辑 ----

    @app.get("/api/script")
    async def get_script(path: str, episode_id: str) -> dict[str, Any]:
        store = _store(path)
        ep = _episode(store, episode_id)
        return {
            "script": ep.script,
            "target_duration_s": ep.target_duration_s,
            "title": ep.title,
        }

    @app.post("/api/script")
    async def update_script(req: ScriptUpdateRequest) -> dict[str, Any]:
        """改剧本。可以顺便重出分镜。

        重出会覆盖整张分镜表，人工改过的镜头会丢，所以要显式勾选。
        """
        store = _store(req.project)
        project = store.load_project()
        ep = project.episode_by_id(req.episode_id)
        if ep is None:
            raise HTTPException(404, f"没有剧集 {req.episode_id}")

        ep.script = req.script
        if req.duration_s:
            ep.target_duration_s = req.duration_s

        result: dict[str, Any] = {"saved": True, "regenerated": False}
        if req.regenerate:
            from ..stages.storyboard import StoryboardError, StoryboardGenerator
            assets = store.load_assets()
            try:
                ep.shots = await StoryboardGenerator(settings.llm).generate(
                    req.script, assets, req.episode_id, ep.target_duration_s)
            except StoryboardError as exc:
                raise HTTPException(400, str(exc)) from exc
            result.update(regenerated=True, shots=len(ep.shots))
        store.save_project(project)
        return result

    @app.post("/api/shot")
    async def update_shot(req: ShotUpdateRequest) -> dict[str, Any]:
        """改一个镜头。

        改了画面相关的字段就把状态退回未开工，否则下次运行会跳过它，
        用户会以为改动没生效。
        """
        store = _store(req.project)
        project = store.load_project()
        ep = project.episode_by_id(req.episode_id)
        if ep is None:
            raise HTTPException(404, f"没有剧集 {req.episode_id}")
        shot = ep.shot_by_id(req.shot_id)
        if shot is None:
            raise HTTPException(404, f"没有镜头 {req.shot_id}")

        patch = req.patch.model_dump(exclude_none=True)
        texts = patch.pop("dialogue_texts", None)

        # 改了这些就得重出画面
        visual_keys = {
            "first_frame_prompt", "motion_prompt", "negative_prompt",
            "shot_size", "camera_angle", "camera_move", "duration_s",
        }
        touched_visual = bool(visual_keys & set(patch))

        try:
            # 用 model_validate 而不是逐个 setattr。
            # setattr 会把字符串原样塞进枚举字段，绕过转换，
            # 之后读出来是 str 不是枚举，比较和序列化都会出错。
            merged = shot.model_dump()
            merged.update(patch)
            validated = shot.__class__.model_validate(merged)
            for key in patch:
                setattr(shot, key, getattr(validated, key))
            if texts is not None:
                if len(texts) != len(shot.dialogue):
                    raise HTTPException(
                        400,
                        f"台词条数对不上：给了 {len(texts)} 条，"
                        f"这个镜头有 {len(shot.dialogue)} 条"
                    )
                for line, text in zip(shot.dialogue, texts):
                    if line.text != text:
                        line.text = text
                        # 台词变了，配音和时长都要重做
                        line.actual_duration_s = None
                        line.audio_path = None
                        shot.duration_locked = False
                        touched_visual = True
        except HTTPException:
            raise
        except Exception as exc:
            raise HTTPException(400, f"改动不合法：{exc}") from exc

        if touched_visual and "status" not in patch:
            from ..models.shot import ShotStatus
            shot.status = ShotStatus.PLANNED
            shot.attempts = 0
            shot.gate_notes = []

        store.save_project(project)
        return {
            "saved": True,
            "reset_to_planned": touched_visual and "status" not in patch,
            "status": shot.status.value,
        }

    @app.post("/api/shots/batch")
    async def batch_shots(req: BatchShotRequest) -> dict[str, Any]:
        """批量改状态。shot_ids 为空表示整集。"""
        from ..models.shot import ShotStatus

        actions = {"reset", "lock", "unlock", "clear_notes"}
        if req.action not in actions:
            raise HTTPException(
                400, f"不认识的操作 {req.action}，可选：{'、'.join(sorted(actions))}")

        store = _store(req.project)
        project = store.load_project()
        ep = project.episode_by_id(req.episode_id)
        if ep is None:
            raise HTTPException(404, f"没有剧集 {req.episode_id}")

        targets = (
            [s for s in ep.shots if s.shot_id in set(req.shot_ids)]
            if req.shot_ids else list(ep.shots)
        )
        if req.shot_ids:
            missing = set(req.shot_ids) - {s.shot_id for s in targets}
            if missing:
                raise HTTPException(404, f"没有这些镜头：{'、'.join(sorted(missing))}")

        changed = 0
        for shot in targets:
            if req.action == "reset":
                # 锁定的镜头是人工确认过的，批量重置不该动它们，
                # 否则一次误操作就把已经审过的片全废了
                if shot.status is ShotStatus.LOCKED:
                    continue
                shot.status = ShotStatus.PLANNED
                shot.attempts = 0
                shot.gate_notes = []
                changed += 1
            elif req.action == "lock":
                if shot.status is not ShotStatus.LOCKED:
                    shot.status = ShotStatus.LOCKED
                    changed += 1
            elif req.action == "unlock":
                if shot.status is ShotStatus.LOCKED:
                    shot.status = (
                        ShotStatus.FINAL_DONE if shot.video_path
                        else ShotStatus.PLANNED
                    )
                    changed += 1
            elif req.action == "clear_notes":
                if shot.gate_notes:
                    shot.gate_notes = []
                    changed += 1

        store.save_project(project)
        return {
            "changed": changed,
            "total": len(targets),
            "skipped_locked": sum(
                1 for s in targets
                if req.action == "reset" and s.status is ShotStatus.LOCKED
            ),
        }

    @app.post("/api/shots/reorder")
    async def reorder_shots(req: ReorderShotsRequest) -> dict[str, Any]:
        """按给定顺序重排镜头。

        必须给出完整列表。只传「把 A 挪到第 3 位」这类增量指令的话，
        界面和引擎对当前顺序的理解一旦对不上，结果就是把片子剪乱，
        而且是那种要播一遍才发现的乱。

        不重跑任何镜头：换顺序不改画面，已经渲染好的还能用。
        转场是装配时按前后镜头算的，跟着新顺序自然就对了。
        """
        store = _store(req.project)
        project = store.load_project()
        ep = project.episode_by_id(req.episode_id)
        if ep is None:
            raise HTTPException(404, f"没有剧集 {req.episode_id}")

        wanted = list(req.shot_ids)
        if len(set(wanted)) != len(wanted):
            raise HTTPException(400, "顺序里有重复的镜头 id")
        current = {s.shot_id for s in ep.shots}
        if set(wanted) != current:
            missing = sorted(current - set(wanted))
            extra = sorted(set(wanted) - current)
            raise HTTPException(
                400,
                "顺序表和这一集的镜头对不上。"
                + (f"少了：{'、'.join(missing)}。" if missing else "")
                + (f"多了：{'、'.join(extra)}。" if extra else ""),
            )

        rank = {sid: i for i, sid in enumerate(wanted)}
        moved = sum(1 for s in ep.shots if s.order != rank[s.shot_id])
        for shot in ep.shots:
            shot.order = rank[shot.shot_id]
        store.save_project(project)
        return {"moved": moved, "total": len(wanted)}

    @app.post("/api/shots/link_locations")
    async def link_locations(req: LinkLocationsRequest) -> dict[str, Any]:
        """把 location_id 空着的镜头接回场景。

        老项目里的分镜多半只填了 scene_id。那样渲染时场景描述整段丢掉，
        跑出来的画面同一个房间每镜都不一样，而且不报任何错。
        新出的分镜已经在生成时接上了（见 stages/storyboard.link_location），
        这个接口是给之前存下来的补的。
        """
        store = _store(req.project)
        try:
            project = store.load_project()
            assets = store.load_assets()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc

        known = set(assets.locations)
        linked: dict[str, int] = {}
        for ep in project.episodes:
            if req.episode_id and ep.episode_id != req.episode_id:
                continue
            n = 0
            for shot in ep.shots:
                if shot.location_id:
                    continue
                if shot.scene_id in known:
                    shot.location_id = shot.scene_id
                    n += 1
            if n:
                linked[ep.episode_id] = n
        total = sum(linked.values())
        if total:
            store.save_project(project)
        # 接上之后提示词才完整，已渲染的那些是按缺场景的提示词跑出来的
        reset = _reset_all_shots(store) if total else 0
        return {"linked": total, "episodes": linked, "reset_shots": reset}

    @app.post("/api/episode")
    async def new_episode(req: NewEpisodeRequest) -> dict[str, Any]:
        """新建一集。

        id 留空就自动往后编号。量产时不该逼用户自己想 id。
        """
        from ..models.project import Episode

        store = _store(req.project)
        project = store.load_project()

        ep_id = req.episode_id.strip() or _next_episode_id(project)
        if not re.fullmatch(r"[a-z0-9_]+", ep_id):
            raise HTTPException(400, "剧集 id 只能用小写字母、数字和下划线")
        if project.episode_by_id(ep_id):
            raise HTTPException(409, f"剧集 {ep_id} 已存在")

        project.episodes.append(Episode(
            episode_id=ep_id, title=req.title,
            target_duration_s=req.target_duration_s))
        store.save_project(project)
        return {"episode_id": ep_id, "title": req.title}

    @app.post("/api/episode/action")
    async def episode_action(req: EpisodeActionRequest) -> dict[str, Any]:
        """删除、复制或改名一集。

        复制只带走剧本和分镜的文案，不带产出物和状态。复制出来的一集
        是要重跑的，把状态也带过去会让它显示成已完成但没有文件。
        """
        store = _store(req.project)
        project = store.load_project()
        ep = project.episode_by_id(req.episode_id)
        if ep is None:
            raise HTTPException(404, f"没有剧集 {req.episode_id}")

        if req.action == "delete":
            if len(project.episodes) <= 1:
                raise HTTPException(400, "至少要留一集")
            project.episodes = [
                e for e in project.episodes if e.episode_id != req.episode_id]
            store.save_project(project)
            return {"deleted": req.episode_id}

        if req.action == "rename":
            ep.title = req.new_title
            store.save_project(project)
            return {"renamed": req.episode_id, "title": ep.title}

        if req.action == "duplicate":
            from ..models.project import Episode
            from ..models.shot import ShotStatus

            existing = {e.episode_id for e in project.episodes}
            n = len(project.episodes) + 1
            while f"ep{n:02d}" in existing:
                n += 1
            new_id = f"ep{n:02d}"

            shots = []
            for s in ep.sorted_shots():
                data = s.model_dump()
                data["shot_id"] = s.shot_id.replace(req.episode_id, new_id, 1)
                data["scene_id"] = s.scene_id.replace(req.episode_id, new_id, 1)
                # 产出物和状态不带过去，复制出来的是要重跑的
                data.update(status=ShotStatus.PLANNED, attempts=0,
                            gate_notes=[], frame_path=None, video_path=None,
                            duration_locked=False)
                for line in data.get("dialogue", []):
                    line["audio_path"] = None
                    line["actual_duration_s"] = None
                shots.append(s.__class__.model_validate(data))

            project.episodes.append(Episode(
                episode_id=new_id, title=(ep.title or req.episode_id) + " 副本",
                script=ep.script, target_duration_s=ep.target_duration_s,
                shots=shots))
            store.save_project(project)
            return {"episode_id": new_id, "shots": len(shots)}

        raise HTTPException(
            400, f"不认识的操作 {req.action}，可选：delete、duplicate、rename")

    @app.get("/api/assets")
    async def get_assets(path: str) -> dict[str, Any]:
        """角色与场景的完整设定。编辑器要回填这些。"""
        store = _store(path)
        try:
            assets = store.load_assets()
        except (FileNotFoundError, ValueError) as exc:
            raise HTTPException(400, str(exc)) from exc
        # 参考图只有图像工作流那条路会用。默认装机没有 image.json，
        # 首帧走视频模型，而那条路直接忽略参考图。
        # 不说清楚的话，用户传了图、镜头也退回重跑了，画面却一点没变。
        has_image_wf = (store.root / "workflows" / "image.json").is_file()
        return {
            "reference_images_used": has_image_wf,
            "reference_hint": (
                "" if has_image_wf else
                "当前用视频模型出首帧，这条路不看参考图。"
                "要让参考图生效，把一个图像工作流存成项目里的 "
                "workflows/image.json。"
            ),
            "characters": [
                {
                    "char_id": c.char_id, "name": c.name,
                    "identity": c.appearance.identity,
                    "body": c.appearance.body,
                    "face": c.appearance.face,
                    "attire": c.appearance.attire,
                    "style": c.appearance.style,
                    "voice_id": c.voice_id,
                    # 猜出来的性别。音色留空时按它挑，猜错了用户得看得见
                    "voice_gender": c.voice_gender,
                    "ref_front": c.ref_front,
                    "ref_three_quarter": c.ref_three_quarter,
                    "ref_back": c.ref_back,
                    "lora_trigger": c.lora_trigger,
                    "lora_strength": c.lora_strength,
                    "rendered": c.render_prompt(assets.style.style_line),
                }
                for c in assets.characters.values()
            ],
            "locations": [
                {
                    "location_id": loc.location_id, "name": loc.name,
                    "space": loc.space, "lighting": loc.lighting,
                    "palette": loc.palette,
                    # 空景图是场景一致性的锚点。以前没回传，界面上就没法
                    # 显示也没法换，等于这个字段只有命令行够得着。
                    "ref_empty": loc.ref_empty,
                    "rendered": loc.render_prompt(assets.style.style_line),
                }
                for loc in assets.locations.values()
            ],
            "style": {
                "style_line": assets.style.style_line.value,
                "global_style": assets.style.global_style,
                "negative_prompt": assets.style.negative_prompt,
                "aspect_ratio": assets.style.aspect_ratio,
            },
        }

    def _reset_all_shots(store: ProjectStore) -> int:
        """把所有未锁定的镜头退回未开工。

        角色外观和场景是所有镜头共用的，改了它们等于全剧的提示词都变了。
        不重置的话已完成的镜头会继续用旧设定，同一个角色前后长得不一样。
        """
        from ..models.shot import ShotStatus

        project = store.load_project()
        n = 0
        for ep in project.episodes:
            for shot in ep.shots:
                if shot.status in (ShotStatus.PLANNED, ShotStatus.LOCKED):
                    continue
                shot.status = ShotStatus.PLANNED
                shot.attempts = 0
                shot.gate_notes = []
                n += 1
        store.save_project(project)
        return n

    @app.post("/api/character")
    async def update_character(req: CharacterUpdateRequest) -> dict[str, Any]:
        store = _store(req.project)
        assets = store.load_assets()
        char = assets.characters.get(req.char_id)
        if char is None:
            raise HTTPException(404, f"没有角色 {req.char_id}")

        patch = req.patch.model_dump(exclude_none=True)
        appearance_keys = {"identity", "body", "face", "attire", "style"}
        # 比的是值变没变，不是字段在不在。界面一次提交整张表单，
        # 光看字段在不在的话，改个音色也会把全剧镜头退回重跑，
        # 已经渲染好的成片档白白重来一遍。
        touched = _changed(char.appearance.model_dump(), patch, appearance_keys)

        try:
            app_data = char.appearance.model_dump()
            for key in appearance_keys & set(patch):
                app_data[key] = patch.pop(key)
            char.appearance = char.appearance.__class__.model_validate(app_data)
            for key, value in patch.items():
                setattr(char, key, value)
        except Exception as exc:
            raise HTTPException(400, f"改动不合法：{exc}") from exc

        store.save_assets(assets)
        reset = _reset_all_shots(store) if (touched and req.reset_shots) else 0
        return {
            "saved": True,
            "rendered": char.render_prompt(assets.style.style_line),
            "reset_shots": reset,
        }

    # 参考图能传哪些格式。不是随便什么文件都往项目目录里塞，
    # 而且 ComfyUI 那边的 LoadImage 也只认这几种。
    _REF_SUFFIX = {
        "image/png": ".png", "image/jpeg": ".jpg", "image/webp": ".webp",
    }
    # 单张上限。参考图是给模型看的，几千像素足够，
    # 传一张两百兆的原片进来只会把项目目录撑爆。
    _REF_MAX_BYTES = 20 * 1024 * 1024

    @app.post("/api/character/reference")
    async def upload_reference(
        project: str = Form(...),
        char_id: str = Form(...),
        slot: str = Form(...),
        file: UploadFile = File(...),
    ) -> dict[str, Any]:
        """给角色传一张参考图。

        参考图是一致性最硬的手段：文字描述再细，模型每次也会重新想象
        一遍这张脸；给一张图，它就照着画。整条链路早就通了——出首帧时
        会按机位挑正面还是四分之三侧面传给 ComfyUI——只是界面上一直
        没有地方设置，等于这个功能存在但够不着。
        """
        if slot not in ("front", "three_quarter", "back"):
            raise HTTPException(400, "只有正面、四分之三侧面、背面三个位置")

        store = _store(project)
        assets = store.load_assets()
        char = assets.characters.get(char_id)
        if char is None:
            raise HTTPException(404, f"没有角色 {char_id}")

        suffix = _REF_SUFFIX.get(file.content_type or "")
        if suffix is None:
            raise HTTPException(
                400, f"只收 png、jpg、webp，收到的是 {file.content_type}")
        data = await file.read()
        if not data:
            raise HTTPException(400, "文件是空的")
        if len(data) > _REF_MAX_BYTES:
            raise HTTPException(
                400, f"太大了（{len(data) / 1024 / 1024:.0f} MB）。"
                     f"参考图给模型看，几千像素就够")

        store.paths.refs.mkdir(parents=True, exist_ok=True)
        dest = store.paths.refs / f"{char_id}_{slot}{suffix}"
        # 换格式重传时把旧的那张删掉，不然 refs 里会留下一张永远用不上的
        for old_suffix in _REF_SUFFIX.values():
            stale = store.paths.refs / f"{char_id}_{slot}{old_suffix}"
            if stale != dest and stale.is_file():
                stale.unlink()
        dest.write_bytes(data)

        rel = store.paths.rel(dest)
        setattr(char, f"ref_{slot}", rel)
        store.save_assets(assets)
        # 参考图直接决定画面长什么样，跟改外观是一回事，得重跑
        reset = _reset_all_shots(store)
        return {"saved": rel, "slot": slot, "reset_shots": reset,
                "size_kb": round(len(data) / 1024)}

    @app.post("/api/character/reference/clear")
    async def clear_reference(req: ClearReferenceRequest) -> dict[str, Any]:
        """撤掉一张参考图，退回纯文字描述。"""
        if req.slot not in ("front", "three_quarter", "back"):
            raise HTTPException(400, "只有正面、四分之三侧面、背面三个位置")
        store = _store(req.project)
        assets = store.load_assets()
        char = assets.characters.get(req.char_id)
        if char is None:
            raise HTTPException(404, f"没有角色 {req.char_id}")

        rel = getattr(char, f"ref_{req.slot}", None)
        if not rel:
            return {"cleared": False, "reset_shots": 0}
        setattr(char, f"ref_{req.slot}", None)
        store.save_assets(assets)
        # 文件留着不删。用户可能只是想先试试没有参考图的效果，
        # 删掉的话再想用回来就得重新找那张图。
        return {"cleared": True, "reset_shots": _reset_all_shots(store)}

    @app.post("/api/location/reference")
    async def upload_location_reference(
        project: str = Form(...),
        location_id: str = Form(...),
        file: UploadFile = File(...),
    ) -> dict[str, Any]:
        """给场景传一张空景图。

        空景图是场景一致性的锚点：同一个房间在十几个镜头里出现，光靠
        「冷调顶光的安保室」这句话，模型每次布置的家具位置都不一样。
        给一张没有人的空景，它就照着那个空间画。
        """
        store = _store(project)
        assets = store.load_assets()
        loc = assets.locations.get(location_id)
        if loc is None:
            raise HTTPException(404, f"没有场景 {location_id}")

        suffix = _REF_SUFFIX.get(file.content_type or "")
        if suffix is None:
            raise HTTPException(
                400, f"只收 png、jpg、webp，收到的是 {file.content_type}")
        data = await file.read()
        if not data:
            raise HTTPException(400, "文件是空的")
        if len(data) > _REF_MAX_BYTES:
            raise HTTPException(
                400, f"太大了（{len(data) / 1024 / 1024:.0f} MB）。"
                     f"参考图给模型看，几千像素就够")

        store.paths.refs.mkdir(parents=True, exist_ok=True)
        dest = store.paths.refs / f"{location_id}_empty{suffix}"
        # 换格式重传时把旧的删掉，不然 refs 里会留一张永远用不上的
        for old_suffix in _REF_SUFFIX.values():
            stale = store.paths.refs / f"{location_id}_empty{old_suffix}"
            if stale != dest and stale.is_file():
                stale.unlink()
        dest.write_bytes(data)

        rel = store.paths.rel(dest)
        loc.ref_empty = rel
        store.save_assets(assets)
        # 空景图直接决定画面长什么样，跟改场景描述是一回事，得重跑
        return {"saved": rel, "reset_shots": _reset_all_shots(store),
                "size_kb": round(len(data) / 1024)}

    @app.post("/api/location/reference/clear")
    async def clear_location_reference(
        req: ClearLocationReferenceRequest,
    ) -> dict[str, Any]:
        """撤掉空景图，退回纯文字描述。文件留着，想用回来不用重新找。"""
        store = _store(req.project)
        assets = store.load_assets()
        loc = assets.locations.get(req.location_id)
        if loc is None:
            raise HTTPException(404, f"没有场景 {req.location_id}")
        if not loc.ref_empty:
            return {"cleared": False, "reset_shots": 0}
        loc.ref_empty = None
        store.save_assets(assets)
        return {"cleared": True, "reset_shots": _reset_all_shots(store)}

    @app.post("/api/location")
    async def update_location(req: LocationUpdateRequest) -> dict[str, Any]:
        store = _store(req.project)
        assets = store.load_assets()
        loc = assets.locations.get(req.location_id)
        if loc is None:
            raise HTTPException(404, f"没有场景 {req.location_id}")

        patch = req.patch.model_dump(exclude_none=True)
        visual = {"space", "lighting", "palette"}
        touched = _changed(loc.model_dump(), patch, visual)
        try:
            data = loc.model_dump()
            data.update(patch)
            assets.locations[req.location_id] = loc.__class__.model_validate(data)
        except Exception as exc:
            raise HTTPException(400, f"改动不合法：{exc}") from exc

        store.save_assets(assets)
        reset = _reset_all_shots(store) if (touched and req.reset_shots) else 0
        return {
            "saved": True,
            "rendered": assets.locations[req.location_id].render_prompt(
                assets.style.style_line),
            "reset_shots": reset,
        }

    @app.post("/api/style")
    async def update_style(req: StyleUpdateRequest) -> dict[str, Any]:
        store = _store(req.project)
        assets = store.load_assets()
        patch = req.patch.model_dump(exclude_none=True)
        before = assets.style.model_dump()
        try:
            data = assets.style.model_dump()
            data.update(patch)
            assets.style = assets.style.__class__.model_validate(data)
        except Exception as exc:
            raise HTTPException(400, f"改动不合法：{exc}") from exc
        store.save_assets(assets)
        touched = _changed(before, patch, {"global_style", "negative_prompt",
                                           "aspect_ratio"})
        reset = _reset_all_shots(store) if (touched and req.reset_shots) else 0
        return {"saved": True, "reset_shots": reset}

    @app.get("/api/settings")
    async def get_settings() -> dict[str, Any]:
        profile = HardwareProfile.detect(settings.vram_gb_override)
        return {
            "tiers": {
                t.value: {
                    "width": profile.tiers[t].width,
                    "height": profile.tiers[t].height,
                    "steps": profile.tiers[t].steps,
                }
                for t in Tier
            },
            "assembly": {
                "fps": settings.assembly.fps,
                "crf": settings.assembly.crf,
                "subtitle_font": settings.assembly.subtitle_font,
                "subtitle_max_chars_per_line":
                    settings.assembly.subtitle_max_chars_per_line,
                "subtitle_max_lines": settings.assembly.subtitle_max_lines,
                "scene_transition_s": settings.assembly.scene_transition_s,
            },
            "gates": {
                "enabled": settings.gates.enabled,
                "max_attempts_per_shot": settings.gates.max_attempts_per_shot,
                "min_pixel_std": settings.gates.min_pixel_std,
                "min_frame_similarity": settings.gates.min_frame_similarity,
                "max_audio_drift_s": settings.gates.max_audio_drift_s,
                "target_lufs": settings.gates.target_lufs,
                "fallback_on_exhausted": settings.gates.fallback_on_exhausted,
            },
            "tts": {
                "tolerance_s": settings.tts.tolerance_s,
                "max_tempo_shift": settings.tts.max_tempo_shift,
            },
        }

    @app.post("/api/settings")
    async def update_settings(body: dict[str, Any]) -> dict[str, Any]:
        """改运行参数。

        默认只影响本次进程。勾了写回才进配置文件——调画质档位这类事
        常常是试几次才定下来，每次都写进文件反而碍事；但试定了之后
        重启一次就退回默认值，也说不过去。所以做成一个开关。
        """
        if state.running:
            raise HTTPException(409, "正在跑，改参数会让这一集前后不一致")

        # 老写法是把字段直接摊在请求体里，新写法包在 patch 里。
        # 两种都收，免得刷新慢一步的页面点一下就报 422。
        try:
            if "patch" in body:
                req = SettingsRequest.model_validate(body)
            else:
                req = SettingsRequest(patch=SettingsPatch.model_validate(body))
        except ValidationError as exc:
            raise HTTPException(422, _readable(exc)) from exc
        patch, persist = req.patch, req.persist

        data = patch.model_dump(exclude_none=True)
        profile = HardwareProfile.detect(settings.vram_gb_override)
        changed: list[str] = []

        from dataclasses import replace
        for tier, prefix in ((Tier.DRAFT, "draft"), (Tier.FINAL, "final")):
            spec = profile.tiers[tier]
            kw = {}
            for field in ("width", "height", "steps"):
                key = f"{prefix}_{field}"
                if key in data:
                    kw[field] = data[key]
                    changed.append(key)
            if kw:
                if "width" in kw and kw["width"] % 32:
                    raise HTTPException(400, "宽度必须是 32 的倍数")
                if "height" in kw and kw["height"] % 32:
                    raise HTTPException(400, "高度必须是 32 的倍数")
                _TIER_OVERRIDES[tier] = replace(spec, **kw)

        # 逐字段搬。pydantic 的字段校验在 setattr 上不生效，
        # 所以最后整个模型再验一遍，验不过就整体回滚。
        before = (settings.assembly.model_copy(deep=True),
                  settings.gates.model_copy(deep=True),
                  settings.tts.model_copy(deep=True))
        for key in ("fps", "crf", "subtitle_font", "subtitle_max_chars_per_line",
                    "subtitle_max_lines", "scene_transition_s"):
            if key in data:
                setattr(settings.assembly, key, data[key])
                changed.append(key)
        for key in ("max_attempts_per_shot", "min_pixel_std",
                    "min_frame_similarity", "max_audio_drift_s",
                    "target_lufs", "fallback_on_exhausted"):
            if key in data:
                setattr(settings.gates, key, data[key])
                changed.append(key)
        if "gates_enabled" in data:
            settings.gates.enabled = data["gates_enabled"]
            changed.append("gates_enabled")
        for key, field in (("tts_tolerance_s", "tolerance_s"),
                           ("tts_max_tempo_shift", "max_tempo_shift")):
            if key in data:
                setattr(settings.tts, field, data[key])
                changed.append(key)

        try:
            settings.assembly = type(settings.assembly).model_validate(
                settings.assembly.model_dump())
            settings.gates = type(settings.gates).model_validate(
                settings.gates.model_dump())
            settings.tts = type(settings.tts).model_validate(
                settings.tts.model_dump())
        except ValidationError as exc:
            settings.assembly, settings.gates, settings.tts = before
            raise HTTPException(400, _readable(exc)) from exc

        saved_to = None
        if persist and changed:
            try:
                saved_to = str(save_user_config(_settings_toml(settings, changed)))
            except OSError as exc:
                raise HTTPException(500, f"配置写不进去：{exc}") from exc

        return {"changed": changed, "labels": _labels(changed),
                "saved_to": saved_to}

    return app


# 界面改过的档位参数。进程内有效，重启即失效。
_TIER_OVERRIDES: dict[Tier, Any] = {}


def _episode(store: ProjectStore, episode_id: str):
    try:
        project = store.load_project()
    except (FileNotFoundError, ValueError) as exc:
        raise HTTPException(400, str(exc)) from exc
    ep = project.episode_by_id(episode_id)
    if ep is None:
        raise HTTPException(404, f"没有剧集 {episode_id}")
    return ep




def _slugify(name: str) -> str:
    """目录名转成合法的项目 id。中文目录名也要能用。"""
    import hashlib
    import re

    slug = re.sub(r"[^a-z0-9_-]+", "-", name.lower()).strip("-")
    if not slug:
        slug = "p-" + hashlib.sha1(name.encode("utf-8")).hexdigest()[:8]
    return slug


_FIELD_NAMES = {
    "base_url": "地址", "model": "模型名", "api_key": "api key",
    "temperature": "温度", "job_timeout_s": "单镜超时",
    "max_retries": "重试次数", "backend": "后端",
    "fps": "帧率", "crf": "画质 crf", "subtitle_font": "字幕字体",
    "subtitle_max_chars_per_line": "字幕单行字数",
    "subtitle_max_lines": "字幕行数", "scene_transition_s": "场景转场",
    "max_attempts_per_shot": "单镜最多重试", "min_pixel_std": "画面展布下限",
    "min_frame_similarity": "与首帧相似度下限",
    "max_audio_drift_s": "台词落点最大偏差", "target_lufs": "响度目标",
    "tolerance_s": "时长容差", "max_tempo_shift": "变速上限",
    "tts_tolerance_s": "配音时长容差", "tts_max_tempo_shift": "配音变速上限",
    "gates_enabled": "质量闸门", "fallback_on_exhausted": "重试超限降级",
    "draft_width": "草稿档宽", "draft_height": "草稿档高",
    "draft_steps": "草稿档步数", "final_width": "成片档宽",
    "final_height": "成片档高", "final_steps": "成片档步数",
    "comfy_base_url": "ComfyUI 地址", "comfy_job_timeout_s": "单镜超时",
    "comfy_max_retries": "提交重试次数", "llm_base_url": "大模型地址",
    "llm_model": "模型名", "llm_api_key": "api key",
    "llm_temperature": "温度", "tts_backend": "配音后端",
    "tts_base_url": "配音服务地址", "vram_gb_override": "显存覆盖",
}


def _labels(keys: list[str]) -> list[str]:
    """报「已应用 min_frame_similarity」不如报「与首帧相似度下限」。

    界面上的标签是中文，回执用英文字段名，用户得自己对着猜是哪一项。
    """
    return [_FIELD_NAMES.get(k, k) for k in keys]


def _readable(exc: ValidationError) -> str:
    """把 pydantic 的报错压成一句人话。

    原样抛出去的话用户看到的是一段带 url 的英文堆栈，
    真正有用的那句中文提示埋在中间，等于没提示。
    """
    lines = []
    for err in exc.errors():
        field = err["loc"][0] if err["loc"] else ""
        name = _FIELD_NAMES.get(str(field), str(field))
        kind = err["type"]
        limit = (err.get("ctx") or {}).get("le") or (err.get("ctx") or {}).get("ge")             or (err.get("ctx") or {}).get("lt") or (err.get("ctx") or {}).get("gt")
        # 2.0 这种带小数点的上限念着别扭，能取整就取整
        if isinstance(limit, float) and limit.is_integer():
            limit = int(limit)
        if kind.startswith("greater_than"):
            msg = f"不能小于 {limit}"
        elif kind.startswith("less_than"):
            msg = f"不能大于 {limit}"
        elif kind in ("int_parsing", "float_parsing", "int_type", "float_type"):
            msg = "要填数字"
        elif kind == "string_type":
            msg = "要填文字"
        else:
            msg = err["msg"].removeprefix("Value error, ")
        lines.append(f"{name}：{msg}" if name else msg)
    return "；".join(lines) or str(exc)


def _changed(current: dict[str, Any], patch: dict[str, Any],
             keys: set[str]) -> bool:
    """这批字段里有没有真的改动。

    界面一次提交整张表单，所以「字段出现在请求里」不代表用户改了它。
    按值比，值没变就不算改，也就不该触发重跑。
    """
    return any(k in patch and patch[k] != current.get(k) for k in keys)


def _script_budget(duration_s: float) -> int:
    from ..stages.script import budget_chars
    return budget_chars(duration_s)


def _next_episode_id(project) -> str:
    """下一个没被占用的剧集编号。量产时不该逼用户自己想 id。

    只数 epNN 那些。预告片挂在 trailer 上，把它也数进去的话，
    有了预告之后新建的第二集会跳号变成 ep03。
    """
    existing = {e.episode_id for e in project.episodes}
    n = sum(1 for e in project.episodes
            if re.fullmatch(r"ep\d+", e.episode_id)) + 1
    while f"ep{n:02d}" in existing:
        n += 1
    return f"ep{n:02d}"


# 参数字段落在配置文件的哪一节。画质档位不在这里：它是按显存推出来的，
# 写死在配置里等于把这台机器的显存刻进项目，换台机器就不对了。
_SETTING_SECTIONS = {
    "fps": ("assembly", "fps"),
    "crf": ("assembly", "crf"),
    "subtitle_font": ("assembly", "subtitle_font"),
    "subtitle_max_chars_per_line": ("assembly", "subtitle_max_chars_per_line"),
    "subtitle_max_lines": ("assembly", "subtitle_max_lines"),
    "scene_transition_s": ("assembly", "scene_transition_s"),
    "max_attempts_per_shot": ("gates", "max_attempts_per_shot"),
    "min_pixel_std": ("gates", "min_pixel_std"),
    "min_frame_similarity": ("gates", "min_frame_similarity"),
    "max_audio_drift_s": ("gates", "max_audio_drift_s"),
    "target_lufs": ("gates", "target_lufs"),
    "fallback_on_exhausted": ("gates", "fallback_on_exhausted"),
    "gates_enabled": ("gates", "enabled"),
    "tts_tolerance_s": ("tts", "tolerance_s"),
    "tts_max_tempo_shift": ("tts", "max_tempo_shift"),
}


def _settings_toml(settings: Settings, changed: list[str]) -> dict[str, Any]:
    """把改过的参数整理成写回配置文件的结构。"""
    current = {"assembly": settings.assembly, "gates": settings.gates,
               "tts": settings.tts}
    out: dict[str, Any] = {}
    for key in changed:
        target = _SETTING_SECTIONS.get(key)
        if target is None:
            continue  # 画质档位这类不写回
        section, field = target
        out.setdefault(section, {})[field] = getattr(current[section], field)
    return out


def _store(path: str) -> ProjectStore:
    if not path:
        raise HTTPException(400, "没有指定项目目录")
    return ProjectStore(path)


def run_server(
    settings: Settings, host: str = "127.0.0.1", port: int = 8080,
    project: Path | None = None,
) -> None:
    import uvicorn

    uvicorn.run(create_app(settings, project), host=host, port=port,
                log_level="warning")
