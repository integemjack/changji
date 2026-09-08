"""配置。

可移植性的另一半。三条硬规则：

一，安装目录和数据目录彻底分开。程序装在哪都行，项目数据跟着项目走。
二，ComfyUI 是一个 URL，不是一个假设。它可以在本机，也可以在局域网另一台有显卡的机器上。
三，任何路径都不写死。配置文件里的相对路径一律相对项目根解析。

优先级从高到低：环境变量、项目配置、用户全局配置、内置默认值。
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

import tomlkit
from platformdirs import user_config_dir, user_data_dir
from pydantic import BaseModel, Field, field_validator

APP_NAME = "changji"
ENV_PREFIX = "CHANGJI_"


class ComfyConfig(BaseModel):
    """ComfyUI 连接。默认本机，但可以指向任意一台机器。"""

    model_config = {"extra": "forbid"}

    base_url: str = Field(default="http://127.0.0.1:8188")
    timeout_s: float = Field(default=60.0, gt=0)
    # 提交后等待单个任务完成的上限。成片档一个镜头可能要好几分钟
    job_timeout_s: float = Field(default=1800.0, gt=0)
    max_retries: int = Field(default=3, ge=0)

    @field_validator("base_url")
    @classmethod
    def _strip_slash(cls, v: str) -> str:
        v = v.strip().rstrip("/")
        if not v.startswith(("http://", "https://")):
            raise ValueError("ComfyUI 地址必须以 http:// 或 https:// 开头")
        return v

    @property
    def ws_url(self) -> str:
        return self.base_url.replace("http://", "ws://").replace("https://", "wss://") + "/ws"


class LLMConfig(BaseModel):
    """剧本和分镜用的大模型。默认走本地 Ollama。"""

    model_config = {"extra": "forbid"}

    base_url: str = Field(default="http://127.0.0.1:11434/v1")
    model: str = Field(default="qwen3:14b")
    api_key: str = Field(default="ollama", description="本地服务通常不校验")
    timeout_s: float = Field(default=300.0, gt=0)
    temperature: float = Field(default=0.7, ge=0.0, le=2.0)

    @field_validator("base_url")
    @classmethod
    def _strip_slash(cls, v: str) -> str:
        return v.strip().rstrip("/")


class TTSConfig(BaseModel):
    """配音。默认假设通过 ComfyUI 节点调用，也可以指向独立的 HTTP 服务。"""

    model_config = {"extra": "forbid"}

    backend: str = Field(default="comfy", description="comfy 或 http")
    base_url: str | None = Field(default=None, description="backend 为 http 时必填")
    engine: str = Field(default="cosyvoice3", description="引擎名，见 docs/选型结论.md")
    # 台词时长与镜头时长的允许偏差。超出就要靠尾帧冻结或音频微调吸收
    tolerance_s: float = Field(default=0.25, ge=0)
    # 音频变速的安全区。有口型的镜头收得更紧
    max_tempo_shift: float = Field(default=0.03, ge=0, le=0.2)


class GateConfig(BaseModel):
    """质量闸门的阈值。全自动模式下这些数字决定了废片能不能被拦住。"""

    model_config = {"extra": "forbid"}

    enabled: bool = True
    # 闸门一：画面不能是纯色或噪点
    min_pixel_std: float = Field(default=12.0, ge=0)
    min_pixel_mean: float = Field(default=8.0, ge=0)
    max_pixel_mean: float = Field(default=247.0, ge=0)
    # 闸门二：与首帧的结构相似度下限
    min_frame_similarity: float = Field(default=0.55, ge=0, le=1)
    # 闸门三：台词落点与分镜的最大偏差
    max_audio_drift_s: float = Field(default=0.15, gt=0)
    target_lufs: float = Field(default=-16.0)
    max_true_peak_db: float = Field(default=-1.5)
    # 重试策略
    max_attempts_per_shot: int = Field(default=3, ge=1)
    fallback_on_exhausted: bool = Field(
        default=True, description="重试超限时降级为静帧加运镜，保证整集能出片"
    )


class AssemblyConfig(BaseModel):
    """成片装配。"""

    model_config = {"extra": "forbid"}

    fps: int = Field(default=24, ge=1, le=120)
    # 统一编码规格。拼接环节最容易踩的坑就是各镜头规格不齐
    pix_fmt: str = "yuv420p"
    video_codec: str = "libx264"
    crf: int = Field(default=18, ge=0, le=51)
    audio_codec: str = "aac"
    audio_bitrate: str = "192k"
    # loudnorm 内部按 192k 跑，不显式收回来的话编码器会挑一个 96k
    # 之类的怪采样率。文件白白变大，有些平台还不收。
    audio_sample_rate: int = Field(default=48000, ge=8000, le=192000)
    audio_channels: int = Field(default=2, ge=1, le=2)
    # 只在场景切换处用溶解，同场景内一律硬切
    scene_transition_s: float = Field(default=0.4, ge=0, le=2)
    # 中文字幕单行上限，全角字符数
    subtitle_max_chars_per_line: int = Field(default=15, ge=6, le=30)
    subtitle_max_lines: int = Field(default=2, ge=1, le=3)
    subtitle_font: str = Field(default="Source Han Sans SC")
    ffmpeg_path: str = Field(default="ffmpeg")
    ffprobe_path: str = Field(default="ffprobe")


class Settings(BaseModel):
    """全部配置。"""

    model_config = {"extra": "forbid"}

    comfy: ComfyConfig = Field(default_factory=ComfyConfig)
    llm: LLMConfig = Field(default_factory=LLMConfig)
    tts: TTSConfig = Field(default_factory=TTSConfig)
    gates: GateConfig = Field(default_factory=GateConfig)
    assembly: AssemblyConfig = Field(default_factory=AssemblyConfig)

    # 显存覆盖。ComfyUI 在别的机器上时本机探测不到，用它手动指定
    vram_gb_override: float | None = Field(default=None, gt=0)
    # 项目库根目录。为空则用系统标准数据目录
    workspace: str | None = None

    def workspace_path(self) -> Path:
        if self.workspace:
            return Path(self.workspace).expanduser().resolve()
        return Path(user_data_dir(APP_NAME, appauthor=False)) / "projects"


def user_config_path() -> Path:
    """用户全局配置的位置。跨平台，由 platformdirs 决定。"""
    return Path(user_config_dir(APP_NAME, appauthor=False)) / "config.toml"


_ENV_MAPPING: dict[str, tuple[str, ...]] = {
    "COMFY_BASE_URL": ("comfy", "base_url"),
    "COMFY_TIMEOUT_S": ("comfy", "timeout_s"),
    "LLM_BASE_URL": ("llm", "base_url"),
    "LLM_MODEL": ("llm", "model"),
    "LLM_API_KEY": ("llm", "api_key"),
    "TTS_BASE_URL": ("tts", "base_url"),
    "WORKSPACE": ("workspace",),
    "VRAM_GB": ("vram_gb_override",),
    "FFMPEG_PATH": ("assembly", "ffmpeg_path"),
}


def _env_overrides() -> dict[str, Any]:
    """从环境变量读覆盖值。

    CHANGJI_COMFY_BASE_URL 映射到 comfy.base_url，以此类推。
    容器化部署和 CI 里这是最方便的注入方式。
    """
    out: dict[str, Any] = {}
    for suffix, path in _ENV_MAPPING.items():
        raw = os.environ.get(ENV_PREFIX + suffix)
        if raw is None or raw == "":
            continue
        cursor = out
        for key in path[:-1]:
            cursor = cursor.setdefault(key, {})
        cursor[path[-1]] = raw
    return out


def env_overridden() -> dict[str, str]:
    """哪些设置正被环境变量顶着。

    环境变量优先级最高。容器里用 compose 注入地址是常态，
    这时候在界面上改配置文件是没用的，重启还是环境变量那一套。
    界面必须把这件事说出来，否则用户会以为程序没保存。
    """
    out: dict[str, str] = {}
    for suffix, path in _ENV_MAPPING.items():
        name = ENV_PREFIX + suffix
        if os.environ.get(name):
            out["_".join(path)] = name
    return out


def _deep_merge(base: dict[str, Any], overlay: dict[str, Any]) -> dict[str, Any]:
    out = dict(base)
    for key, value in overlay.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = _deep_merge(out[key], value)
        else:
            out[key] = value
    return out


def _read_toml(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        return dict(tomlkit.parse(path.read_text(encoding="utf-8")))
    except Exception as exc:  # 配置坏了要说清楚是哪个文件
        raise ValueError(f"配置文件解析失败：{path}\n{exc}") from exc


def load_settings(project_dir: Path | None = None) -> Settings:
    """按优先级合并配置：环境变量 > 项目配置 > 用户全局配置 > 默认值。"""
    data: dict[str, Any] = {}
    data = _deep_merge(data, _read_toml(user_config_path()))
    if project_dir is not None:
        data = _deep_merge(data, _read_toml(Path(project_dir) / "changji.toml"))
    data = _deep_merge(data, _env_overrides())
    return Settings.model_validate(data)


def write_default_config(path: Path | None = None) -> Path:
    """生成一份带注释的配置模板。首次安装时用。"""
    target = path or user_config_path()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(_DEFAULT_TOML, encoding="utf-8")
    return target


_DEFAULT_TOML = """# 场记配置文件
# 优先级：环境变量 > 项目目录下的 changji.toml > 本文件 > 内置默认值

# 项目库根目录。留空则用系统标准数据目录。
# 换机器时把项目目录整个拷走即可，程序装在哪都不影响。
# workspace = "D:/短剧项目"

# 显存覆盖。ComfyUI 跑在另一台机器时本机探测不到显卡，用它手动指定。
# vram_gb_override = 16

[comfy]
# ComfyUI 地址。可以是本机，也可以是局域网里任意一台有显卡的机器。
base_url = "http://127.0.0.1:8188"
job_timeout_s = 1800
max_retries = 3

[llm]
# 剧本和分镜用的大模型。默认走本地 Ollama。
# 也可以填任何兼容 OpenAI 接口的服务。
base_url = "http://127.0.0.1:11434/v1"
model = "qwen3:14b"

[tts]
# backend 填 comfy 表示通过 ComfyUI 的 TTS 节点调用，填 http 表示独立服务。
backend = "comfy"
engine = "cosyvoice3"

[gates]
# 质量闸门。全自动模式下这些阈值决定废片能不能被拦住。
enabled = true
max_attempts_per_shot = 3
# 重试超限时降级为静帧加运镜，保证整集能出片而不是卡死。
fallback_on_exhausted = true

[assembly]
fps = 24
crf = 18
# 只在场景切换处用溶解，同场景内一律硬切。
scene_transition_s = 0.4
# 中文字幕单行上限，全角字符数。
subtitle_max_chars_per_line = 15
subtitle_font = "Source Han Sans SC"
"""


def save_user_config(patch: dict[str, Any], path: Path | None = None) -> Path:
    """把改动写回用户全局配置，保留文件里已有的注释和其它项。

    只写传进来的键。用 tomlkit 而不是重新序列化整个 Settings，
    是因为配置文件里的注释是给人看的，重写一遍就全没了。
    """
    target = path or user_config_path()
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file():
        doc = tomlkit.parse(target.read_text(encoding="utf-8"))
    else:
        doc = tomlkit.parse(_DEFAULT_TOML)

    for section, values in patch.items():
        if not isinstance(values, dict):
            doc[section] = values
            continue
        if section not in doc or not isinstance(doc[section], dict):
            doc[section] = tomlkit.table()
        for key, value in values.items():
            doc[section][key] = value

    target.write_text(tomlkit.dumps(doc), encoding="utf-8")
    return target
