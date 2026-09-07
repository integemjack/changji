"""工作流加载。

命令行和界面共用一份逻辑，避免两处各写一遍然后其中一处漏掉某个工作流。
之前就漏过：配音工作流做好了但没有任何地方去加载它。

查找顺序是先项目后内置。项目里放同名文件就能覆盖内置的，
这样不同的剧可以用不同的模型，不用改代码也不用改配置。
"""

from __future__ import annotations


from . import bundled_workflow
from .comfy.client import ComfyClient
from .comfy.workflow import ApiWorkflow, load_ui_workflow
from .models.project import ProjectStore


async def load_workflow(
    client: ComfyClient, store: ProjectStore, name: str,
    required: bool = True,
) -> ApiWorkflow | None:
    """加载一个工作流。

    name 是不带扩展名的文件名，比如 video、tts、image。
    非必需的工作流找不到就返回 None，由调用方决定退回什么行为。
    """
    candidate = store.root / "workflows" / f"{name}.json"
    if not candidate.is_file():
        candidate = bundled_workflow(f"{name}.json")
    if not candidate.is_file():
        if not required:
            return None
        raise FileNotFoundError(
            f"找不到 {name} 工作流。请把 ComfyUI 里导出的工作流保存到"
            f"项目的 workflows/{name}.json"
        )

    raw = load_ui_workflow(candidate)
    if "nodes" in raw:
        # 界面版，需要服务端的节点定义才能转成接口版
        return (await client.converter()).convert(raw)
    return ApiWorkflow(raw)


async def load_all(
    client: ComfyClient, store: ProjectStore,
) -> dict[str, ApiWorkflow | None]:
    """加载全部工作流。

    只有视频是必需的，其余缺了各有退路：没有图像工作流就用视频模型
    出单帧，没有配音工作流就用估算后端只算时长。
    """
    video = await load_workflow(client, store, "video", required=True)
    image = await load_workflow(client, store, "image", required=False)
    tts = await load_workflow(client, store, "tts", required=False)
    return {"video": video, "image": image, "tts": tts}
