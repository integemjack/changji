"""工作流格式转换与参数注入。

ComfyUI 有两种工作流格式，这是新手最容易栽的地方：

界面版（用户在浏览器里保存的那种）是 {"nodes": [...], "links": [...]}，
节点的参数放在 widgets_values 数组里，只有位置没有名字。

接口版（POST /prompt 要的那种）是 {"节点id": {"class_type": ..., "inputs": {...}}}，
参数是有名字的键值对。

两者之间的映射关系不在文件里，必须从服务端的 /object_info 拿。
硬编码一张映射表在 ComfyUI 升级后就会错，所以这里从服务端动态取。
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any

# 界面上有、接口上没有的伪控件。
# 它们夹在 widgets_values 里会让后面所有参数错位一格，是最隐蔽的坑。
_UI_ONLY_WIDGETS = frozenset(
    {"control_after_generate", "control_before_generate"}
)

# 控件型输入的标量类型。除此之外的全大写类型名都是连线型输入
# （MODEL、CLIP、VAE、IMAGE、LATENT、CONDITIONING、AUDIO、VIDEO 等）。
# 判断必须用白名单：INT 和 FLOAT 也是全大写，但它们是控件不是连线。
_SCALAR_WIDGET_TYPES = frozenset({"INT", "FLOAT", "STRING", "BOOLEAN"})


def _is_widget_type(type_def: Any) -> bool:
    """判断一个输入是控件（占 widgets_values 一格）还是连线。"""
    if isinstance(type_def, (list, tuple)):
        return True  # 下拉框，选项直接列在这里
    if not isinstance(type_def, str):
        return False
    if type_def in _SCALAR_WIDGET_TYPES:
        return True
    # COMBO、COMFY_DYNAMICCOMBO_V3 之类的下拉框变体
    return "COMBO" in type_def


class WorkflowError(RuntimeError):
    pass


class ApiWorkflow:
    """接口格式的工作流。可以按节点类型或标题定位并改参数。"""

    def __init__(self, prompt: dict[str, dict[str, Any]]) -> None:
        self.prompt = prompt

    def copy(self) -> ApiWorkflow:
        return ApiWorkflow(copy.deepcopy(self.prompt))

    def to_dict(self) -> dict[str, dict[str, Any]]:
        return self.prompt

    # ---- 定位 ----

    def find_by_class(self, class_type: str) -> list[str]:
        """按节点类型找出所有节点 id。"""
        return [nid for nid, node in self.prompt.items()
                if node.get("class_type") == class_type]

    def one_by_class(self, class_type: str) -> str:
        """按节点类型找唯一节点。不唯一就报错，避免改错地方。"""
        found = self.find_by_class(class_type)
        if not found:
            raise WorkflowError(f"工作流里没有 {class_type} 节点")
        if len(found) > 1:
            raise WorkflowError(
                f"工作流里有 {len(found)} 个 {class_type} 节点，无法确定改哪个。"
                f"请用节点 id 指定：{found}"
            )
        return found[0]

    # ---- 改参数 ----

    def set_input(self, node_id: str, key: str, value: Any) -> None:
        if node_id not in self.prompt:
            raise WorkflowError(f"节点 {node_id} 不存在")
        self.prompt[node_id].setdefault("inputs", {})[key] = value

    def set_by_class(self, class_type: str, **kwargs: Any) -> str:
        """定位唯一节点并批量改参数。返回节点 id。"""
        nid = self.one_by_class(class_type)
        for key, value in kwargs.items():
            self.set_input(nid, key, value)
        return nid

    def get_input(self, node_id: str, key: str) -> Any:
        return self.prompt.get(node_id, {}).get("inputs", {}).get(key)


class WorkflowConverter:
    """把界面版工作流转成接口版。

    需要服务端的 /object_info 才能知道每个节点的控件顺序和名字。
    """

    def __init__(self, object_info: dict[str, Any]) -> None:
        self.object_info = object_info
        self._widget_names: dict[str, list[str]] = {}

    def widget_names(self, class_type: str) -> list[str]:
        """取一个节点类型的控件名，按界面上的顺序。

        /object_info 里 input.required 是有序字典，顺序和界面上控件的顺序一致。
        连线型输入（MODEL、CLIP 这些）不占 widgets_values 的位置，要排除。
        """
        if class_type in self._widget_names:
            return self._widget_names[class_type]

        info = self.object_info.get(class_type)
        if info is None:
            raise WorkflowError(
                f"服务端不认识节点类型 {class_type}。"
                f"通常是缺少对应的自定义节点包"
            )

        names: list[str] = []
        input_spec = info.get("input", {})
        for section in ("required", "optional"):
            for name, spec in (input_spec.get(section) or {}).items():
                if not isinstance(spec, (list, tuple)) or not spec:
                    continue
                if _is_widget_type(spec[0]):
                    names.append(name)
        self._widget_names[class_type] = names
        return names

    def convert(self, ui_workflow: dict[str, Any]) -> ApiWorkflow:
        """界面版转接口版。"""
        nodes = ui_workflow.get("nodes")
        if nodes is None:
            raise WorkflowError(
                "这不是界面版工作流。如果已经是接口版，直接用 ApiWorkflow 包一层"
            )

        # 连线表：link_id -> (源节点 id, 源输出槽)
        link_src: dict[int, tuple[int, int]] = {}
        for link in ui_workflow.get("links", []):
            if isinstance(link, (list, tuple)) and len(link) >= 5:
                link_src[link[0]] = (link[1], link[2])

        prompt: dict[str, dict[str, Any]] = {}
        for node in nodes:
            # 界面上被静音或旁路的节点不提交
            if node.get("mode") in (2, 4):
                continue
            nid = str(node["id"])
            class_type = node["type"]
            inputs: dict[str, Any] = {}

            # 连线型输入
            for slot in node.get("inputs") or []:
                link_id = slot.get("link")
                if link_id is not None and link_id in link_src:
                    src_node, src_slot = link_src[link_id]
                    inputs[slot["name"]] = [str(src_node), src_slot]

            # 控件型输入
            values = list(node.get("widgets_values") or [])
            if values:
                names = self.widget_names(class_type)
                inputs.update(self._zip_widgets(class_type, names, values, inputs))

            prompt[nid] = {"class_type": class_type, "inputs": inputs}

        return ApiWorkflow(prompt)

    def _zip_widgets(
        self, class_type: str, names: list[str], values: list[Any],
        already_linked: dict[str, Any],
    ) -> dict[str, Any]:
        """把 widgets_values 数组对回控件名。

        难点是界面会在数组里插入伪控件。最典型的是 KSampler：界面上
        seed 后面跟着一个 control_after_generate 下拉框，它占了数组的一个位置，
        但接口不认这个参数。不跳过它，后面 steps、cfg、sampler 全部错位一格。
        """
        out: dict[str, Any] = {}
        vi = 0
        for name in names:
            # 已经由连线提供的输入不再从控件取
            if name in already_linked:
                continue
            if vi >= len(values):
                break
            out[name] = values[vi]
            vi += 1
            # 取完 seed 之后如果还有多余的值，那多半是伪控件，跳过
            if name in ("seed", "noise_seed") and vi < len(values):
                if isinstance(values[vi], str) and values[vi] in (
                    "fixed", "increment", "decrement", "randomize"
                ):
                    vi += 1
        return out


def load_ui_workflow(path: str | Path) -> dict[str, Any]:
    """读界面版工作流文件。"""
    p = Path(path)
    if not p.is_file():
        raise WorkflowError(f"工作流文件不存在：{p}")
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise WorkflowError(f"工作流文件不是合法 JSON：{p}\n{exc}") from exc


__all__ = [
    "ApiWorkflow",
    "WorkflowConverter",
    "WorkflowError",
    "load_ui_workflow",
    "_UI_ONLY_WIDGETS",
]
