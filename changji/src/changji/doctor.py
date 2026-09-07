"""环境体检。

跑之前先把所有外部依赖查一遍，缺什么直接说清楚怎么补。
目标是让用户永远不会在跑到一半的时候才发现缺东西。
"""

from __future__ import annotations

import platform
import shutil
import sys
from dataclasses import dataclass
from enum import Enum

import httpx

from .assembly.ffmpeg import FFmpeg
from .config import Settings
from .hardware import HardwareProfile


class Level(str, Enum):
    OK = "ok"
    WARN = "warn"
    FAIL = "fail"


@dataclass
class Check:
    name: str
    level: Level
    detail: str
    fix: str = ""

    @property
    def symbol(self) -> str:
        return {Level.OK: "✓", Level.WARN: "!", Level.FAIL: "✗"}[self.level]


@dataclass
class Report:
    checks: list[Check]

    @property
    def failed(self) -> list[Check]:
        return [c for c in self.checks if c.level is Level.FAIL]

    @property
    def warned(self) -> list[Check]:
        return [c for c in self.checks if c.level is Level.WARN]

    @property
    def can_run(self) -> bool:
        """能不能跑完整流程。"""
        return not self.failed

    def render(self) -> str:
        lines = []
        width = max((len(c.name) for c in self.checks), default=10)
        for c in self.checks:
            lines.append(f"  {c.symbol}  {c.name:<{width}}  {c.detail}")
            if c.fix:
                for fix_line in c.fix.splitlines():
                    lines.append(f"     {' ' * width}  {fix_line}")
        lines.append("")
        if self.failed:
            lines.append(f"有 {len(self.failed)} 项必须先解决才能出片。")
        elif self.warned:
            lines.append(f"可以跑，但有 {len(self.warned)} 项建议处理。")
        else:
            lines.append("一切就绪。")
        return "\n".join(lines)


async def run_checks(settings: Settings) -> Report:
    checks: list[Check] = [_check_python()]
    checks.append(_check_ffmpeg(settings))
    checks.append(_check_fonts(settings))
    comfy = await _check_comfy(settings)
    checks.append(comfy)
    checks.append(await _check_tts(settings, comfy.level is Level.OK))
    checks.append(await _check_llm(settings))
    checks.append(_check_hardware(settings))
    checks.append(_check_workspace(settings))
    return Report(checks)


async def _check_tts(settings: Settings, comfy_ok: bool) -> Check:
    """配音是否能出真声音。

    这一项必须在开跑前就说清楚，否则用户要等到成片出来才发现整集没声音。
    """
    if settings.tts.backend == "http":
        if not settings.tts.base_url:
            return Check("配音", Level.FAIL, "配了 http 后端但没填地址",
                         "在配置里填 tts.base_url")
        return Check("配音", Level.OK, f"独立服务 {settings.tts.base_url}")

    if not comfy_ok:
        return Check("配音", Level.WARN, "ComfyUI 连不上，无法判断",
                     "先解决 ComfyUI 的连接问题")

    # 查服务端有没有本地配音节点
    try:
        async with httpx.AsyncClient(timeout=30) as http:
            r = await http.get(f"{settings.comfy.base_url}/object_info")
            r.raise_for_status()
            names = set(r.json())
    except (httpx.RequestError, httpx.HTTPStatusError, ValueError):
        return Check("配音", Level.WARN, "查不到 ComfyUI 的节点清单")

    if "UnifiedTTSTextNode" in names:
        engines = sorted(n.replace("EngineNode", "")
                         for n in names if n.endswith("EngineNode"))
        return Check("配音", Level.OK,
                     f"ComfyUI 节点可用，引擎：{'、'.join(engines[:5])}")

    return Check(
        "配音", Level.WARN, "ComfyUI 上没有本地配音节点",
        "成片会是静音。ComfyUI 自带的 TTS 节点都是云 API，本地方案要装节点包：\n"
        "  cd ComfyUI/custom_nodes\n"
        "  git clone https://github.com/diodiogod/TTS-Audio-Suite.git\n"
        "  cd TTS-Audio-Suite && python install.py\n"
        "装完重启 ComfyUI。用 Docker 的话直接重新 build 更稳，\n"
        "手动装的依赖在容器重建时会丢。",
    )


def _check_python() -> Check:
    v = sys.version_info
    text = f"{v.major}.{v.minor}.{v.micro} 于 {platform.system()}"
    if v < (3, 11):
        return Check("Python", Level.FAIL, text, "需要 3.11 或更高版本")
    return Check("Python", Level.OK, text)


def _check_ffmpeg(settings: Settings) -> Check:
    ff = FFmpeg(settings.assembly.ffmpeg_path, settings.assembly.ffprobe_path)
    if ff.available():
        return Check("FFmpeg", Level.OK, shutil.which(settings.assembly.ffmpeg_path) or "已安装")
    return Check(
        "FFmpeg", Level.FAIL, "未找到",
        "装配环节的硬依赖。\n"
        "Windows: winget install Gyan.FFmpeg\n"
        "macOS:   brew install ffmpeg\n"
        "Debian:  sudo apt install ffmpeg\n"
        "装好后仍找不到就在配置里填 assembly.ffmpeg_path",
    )


def _check_fonts(settings: Settings) -> Check:
    """中文字体。缺了字幕会烧成方框，但不影响其它环节。"""
    name = settings.assembly.subtitle_font
    if shutil.which("fc-list"):
        import subprocess
        try:
            out = subprocess.run(["fc-list"], capture_output=True, text=True,
                                 timeout=15).stdout.lower()
            if any(k in out for k in ("noto sans cjk", "source han", "wqy", "pingfang")):
                return Check("中文字体", Level.OK, "已安装")
        except (subprocess.SubprocessError, OSError):
            pass
        return Check(
            "中文字体", Level.WARN, f"没找到 {name}",
            "中文字幕会渲染成方框。\nDebian: sudo apt install fonts-noto-cjk",
        )
    # Windows 和 macOS 自带中文字体
    if platform.system() in ("Windows", "Darwin"):
        return Check("中文字体", Level.OK, "系统自带")
    return Check("中文字体", Level.WARN, "无法检测")


async def _check_comfy(settings: Settings) -> Check:
    url = settings.comfy.base_url
    try:
        async with httpx.AsyncClient(timeout=8) as http:
            r = await http.get(f"{url}/system_stats")
            r.raise_for_status()
            data = r.json()
    except (httpx.RequestError, httpx.HTTPStatusError, ValueError):
        return Check(
            "ComfyUI", Level.FAIL, f"连不上 {url}",
            "出图和出视频都要用它。\n"
            "用 Docker: docker compose up -d comfyui\n"
            "已经装在别处就改配置：\n"
            "  export CHANGJI_COMFY_BASE_URL=http://某台机器:8188",
        )
    devices = data.get("devices") or []
    if devices:
        d = devices[0]
        vram = d.get("vram_total", 0) / (1024 ** 3)
        return Check("ComfyUI", Level.OK, f"{url}  {d.get('name', '')}  {vram:.1f} GB")
    return Check("ComfyUI", Level.WARN, f"{url} 已连接但没有报告显卡",
                 "可能在用 CPU 推理，会非常慢")


async def _check_llm(settings: Settings) -> Check:
    url = settings.llm.base_url
    model = settings.llm.model
    try:
        async with httpx.AsyncClient(timeout=8) as http:
            r = await http.get(f"{url}/models",
                               headers={"Authorization": f"Bearer {settings.llm.api_key}"})
            r.raise_for_status()
            body = r.json()
    except (httpx.RequestError, httpx.HTTPStatusError, ValueError):
        return Check(
            "大模型", Level.WARN, f"连不上 {url}",
            "剧本和分镜要用它。没有它也能手写分镜表。\n"
            "用 Docker: docker compose up -d ollama\n"
            "本机装了 Ollama 就确认它已启动",
        )
    names = [m.get("id", "") for m in body.get("data", [])]
    if model in names or any(n.startswith(model.split(":")[0]) for n in names):
        return Check("大模型", Level.OK, f"{url}  {model}")
    if names:
        # 服务在跑，只是没有配置里指定的那个模型。
        # 这不是错误，用现有的任何一个都能出分镜。
        return Check(
            "大模型", Level.WARN, f"{url} 上没有 {model}",
            f"服务正常，现有模型：{'、'.join(names[:5])}\n"
            f"二选一：\n"
            f"  用现有的：export CHANGJI_LLM_MODEL={names[0]}\n"
            f"  或拉取指定的：ollama pull {model}",
        )
    return Check(
        "大模型", Level.WARN, f"{url} 一个模型都没有",
        f"拉取一个：ollama pull {model}",
    )


def _check_hardware(settings: Settings) -> Check:
    p = HardwareProfile.detect(settings.vram_gb_override)
    if p.gpu is not None:
        return Check("显卡", Level.OK, f"{p.gpu.name}  {p.gpu.vram_gb:.1f} GB")
    if settings.vram_gb_override:
        return Check("显卡", Level.OK,
                     f"按配置的 {settings.vram_gb_override:.0f} GB 推导档位")
    return Check(
        "显卡", Level.WARN, f"本机未探测到，按 {p.vram_gb:.0f} GB 估算",
        "ComfyUI 如果在别的机器上，这是正常的。\n"
        "为了让画质档位推导正确，在配置里填 vram_gb_override",
    )


def _check_workspace(settings: Settings) -> Check:
    path = settings.workspace_path()
    try:
        path.mkdir(parents=True, exist_ok=True)
        probe = path / ".changji_write_test"
        probe.write_text("ok", encoding="utf-8")
        probe.unlink()
    except OSError as exc:
        return Check("项目目录", Level.FAIL, f"{path} 不可写", str(exc))
    return Check("项目目录", Level.OK, str(path))
