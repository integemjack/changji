"""场记：AI 短剧生产流水线。

从剧本到成片的本地编排引擎。真正的推理交给 ComfyUI，大模型交给
任何兼容 OpenAI 接口的服务，这个包负责分镜表、资产库、任务调度、
质量闸门和成片装配。
"""

from __future__ import annotations

from pathlib import Path

__version__ = "0.1.0"

# 包根目录。定位内置工作流等资源文件用。
PACKAGE_ROOT = Path(__file__).resolve().parent


def bundled_workflow(name: str = "video.json") -> Path:
    """取内置工作流的路径。

    项目可以在自己的 workflows 目录里放同名文件覆盖它，
    这样不同的剧可以用不同的模型。
    """
    return PACKAGE_ROOT / "workflows" / name


__all__ = ["PACKAGE_ROOT", "__version__", "bundled_workflow"]
