"""ComfyUI 客户端。

用 WebSocket 监听进度而不是轮询历史接口，因为成片档一个镜头要跑好几分钟，
轮询既慢又容易漏掉中间状态。

失败分两类，处理方式完全不同：
提交时的校验失败（模型文件缺失、参数越界）是确定性的，重试没有意义，直接抛。
执行中的失败（显存不足、节点崩溃）可能是偶发的，值得重试。
"""

from __future__ import annotations

import asyncio
import json
import uuid
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import httpx
import websockets

from ..config import ComfyConfig
from .workflow import ApiWorkflow, WorkflowConverter

# WebSocket 静默多久就去查一次历史兜底。查得太勤给服务端添负担，
# 太懒则任务早跑完了界面还在等。
_RECHECK_EVERY_S = 5.0


class ComfyError(RuntimeError):
    """ComfyUI 相关错误的基类。"""


class ComfyUnavailable(ComfyError):
    """连不上。地址错了、服务没起、或者网络不通。"""


class PromptValidationError(ComfyError):
    """提交时被服务端拒绝。重试没有意义。

    最常见的原因是工作流引用的模型文件在服务端不存在。
    """

    def __init__(self, message: str, node_errors: dict[str, Any] | None = None) -> None:
        super().__init__(message)
        self.node_errors = node_errors or {}

    def human_summary(self) -> str:
        """把服务端的报错翻译成能看懂的话。"""
        if not self.node_errors:
            return str(self)
        lines = []
        for node_id, err in self.node_errors.items():
            cls = err.get("class_type", "未知节点")
            for detail in err.get("errors", []):
                extra = detail.get("extra_info", {})
                if "not in list" in str(detail.get("message", "")):
                    want = extra.get("input_name", "")
                    got = extra.get("received_value", "")
                    lines.append(
                        f"节点 {node_id}（{cls}）要的 {want} 是 {got}，服务端上没有这个文件"
                    )
                else:
                    lines.append(f"节点 {node_id}（{cls}）：{detail.get('message', detail)}")
        return "\n".join(lines) if lines else str(self)


class ExecutionError(ComfyError):
    """执行中失败。可能是偶发的，值得重试。"""


@dataclass
class JobProgress:
    prompt_id: str
    node_id: str | None = None
    step: int = 0
    total: int = 0

    @property
    def fraction(self) -> float:
        return self.step / self.total if self.total else 0.0


@dataclass
class JobResult:
    prompt_id: str
    outputs: dict[str, Any] = field(default_factory=dict)
    elapsed_s: float = 0.0

    def files(self, kind: str = "images") -> list[dict[str, Any]]:
        """取产出文件。视频节点也把结果放在 images 键下，带 animated 标记。"""
        out = []
        for node_out in self.outputs.values():
            out.extend(node_out.get(kind, []))
        return out

    def first_file(self) -> dict[str, Any] | None:
        files = self.files()
        return files[0] if files else None


class ComfyClient:
    """ComfyUI 的异步客户端。"""

    def __init__(self, config: ComfyConfig, client_id: str | None = None) -> None:
        self.config = config
        self.client_id = client_id or uuid.uuid4().hex
        self._object_info: dict[str, Any] | None = None

    # ---- 基础 ----

    async def _get(self, path: str) -> Any:
        async with httpx.AsyncClient(timeout=self.config.timeout_s) as http:
            try:
                r = await http.get(f"{self.config.base_url}{path}")
            except httpx.RequestError as exc:
                raise ComfyUnavailable(
                    f"连不上 ComfyUI（{self.config.base_url}）。"
                    f"请确认服务已启动且地址正确。\n{exc}"
                ) from exc
            r.raise_for_status()
            return r.json()

    async def ping(self) -> bool:
        """服务是否可用。"""
        try:
            await self._get("/system_stats")
            return True
        except (ComfyUnavailable, httpx.HTTPError):
            return False

    async def system_stats(self) -> dict[str, Any]:
        return await self._get("/system_stats")

    async def object_info(self, refresh: bool = False) -> dict[str, Any]:
        """节点定义。格式转换和模型清单都要用，缓存起来。"""
        if self._object_info is None or refresh:
            self._object_info = await self._get("/object_info")
        return self._object_info

    async def converter(self) -> WorkflowConverter:
        return WorkflowConverter(await self.object_info())

    async def available_models(self, node_class: str, input_name: str) -> list[str]:
        """查服务端上某个节点的某个下拉框有哪些可选值。

        用来在提交之前就发现模型缺失，而不是等服务端拒绝。
        """
        info = (await self.object_info()).get(node_class)
        if not info:
            return []
        for section in ("required", "optional"):
            spec = (info.get("input", {}).get(section) or {}).get(input_name)
            if spec and isinstance(spec[0], list):
                return list(spec[0])
        return []

    # ---- 提交与等待 ----

    async def submit(self, workflow: ApiWorkflow) -> str:
        """提交任务，返回 prompt_id。校验失败会抛 PromptValidationError。"""
        return await self._submit_as(workflow, self.client_id)

    async def _submit_as(self, workflow: ApiWorkflow, client_id: str) -> str:
        payload = {"prompt": workflow.to_dict(), "client_id": client_id}
        async with httpx.AsyncClient(timeout=self.config.timeout_s) as http:
            try:
                r = await http.post(f"{self.config.base_url}/prompt", json=payload)
            except httpx.RequestError as exc:
                raise ComfyUnavailable(f"提交任务失败：{exc}") from exc

        if r.status_code == 400:
            try:
                body = r.json()
            except json.JSONDecodeError:
                raise PromptValidationError(f"提交被拒绝：{r.text[:400]}") from None
            err = body.get("error", {})
            raise PromptValidationError(
                err.get("message") or "工作流校验失败",
                node_errors=body.get("node_errors"),
            )
        r.raise_for_status()
        return r.json()["prompt_id"]

    async def wait(
        self,
        prompt_id: str,
        on_progress: Callable[[JobProgress], None] | None = None,
        timeout_s: float | None = None,
        client_id: str | None = None,
    ) -> JobResult:
        """等一个任务跑完。走 WebSocket，掉线自动退回轮询。"""
        deadline = asyncio.get_running_loop().time() + (
            timeout_s or self.config.job_timeout_s
        )
        url = f"{self.config.ws_url}?clientId={client_id or self.client_id}"
        try:
            async with websockets.connect(url, max_size=None) as ws:
                return await self._consume(ws, prompt_id, on_progress, deadline)
        except (websockets.WebSocketException, OSError):
            # WebSocket 不可用不是致命问题，退回轮询
            return await self._wait_poll(prompt_id, deadline)

    async def _consume(
        self, ws, prompt_id: str,
        on_progress: Callable[[JobProgress], None] | None,
        deadline: float,
    ) -> JobResult:
        loop = asyncio.get_running_loop()
        start = loop.time()
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise ExecutionError(f"任务 {prompt_id} 超时")
            try:
                raw = await asyncio.wait_for(
                    ws.recv(), timeout=min(remaining, _RECHECK_EVERY_S))
            except TimeoutError:
                # 一段时间没消息不代表任务还在跑。完成消息可能根本没送到：
                # 任务在 WebSocket 连上之前就结束了，或者同一个 clientId
                # 上有并发任务、后连的把先连的挤掉了。查一次历史兜底，
                # 否则这里会白等到 job_timeout_s，默认是半小时。
                outputs = await self._history_outputs(prompt_id, missing_ok=True)
                if outputs is not None:
                    return JobResult(prompt_id, outputs, loop.time() - start)
                continue
            if isinstance(raw, bytes):
                continue  # 预览图，忽略
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue

            mtype, data = msg.get("type"), msg.get("data", {})
            if data.get("prompt_id") not in (None, prompt_id):
                continue

            if mtype == "progress" and on_progress:
                on_progress(JobProgress(
                    prompt_id=prompt_id,
                    node_id=data.get("node"),
                    step=data.get("value", 0),
                    total=data.get("max", 0),
                ))
            elif mtype == "execution_error":
                raise ExecutionError(
                    f"节点 {data.get('node_type')} 执行失败："
                    f"{data.get('exception_message')}"
                )
            elif mtype == "execution_interrupted":
                raise ExecutionError(f"任务 {prompt_id} 被中断")
            elif mtype == "executing" and data.get("node") is None:
                # node 为 None 表示整个任务跑完
                outputs = await self._history_outputs(prompt_id)
                return JobResult(prompt_id, outputs, loop.time() - start)

    async def _wait_poll(self, prompt_id: str, deadline: float) -> JobResult:
        loop = asyncio.get_running_loop()
        start = loop.time()
        while loop.time() < deadline:
            hist = await self._get(f"/history/{prompt_id}")
            entry = hist.get(prompt_id)
            if entry:
                status = entry.get("status", {})
                if status.get("status_str") == "error":
                    raise ExecutionError(self._error_from_history(status))
                return JobResult(prompt_id, entry.get("outputs", {}), loop.time() - start)
            await asyncio.sleep(2)
        raise ExecutionError(f"任务 {prompt_id} 超时")

    async def _history_outputs(
        self, prompt_id: str, missing_ok: bool = False,
    ) -> dict[str, Any] | None:
        """从历史里取产出。

        missing_ok 表示这是一次兜底查询：任务还没跑完就返回 None，
        让调用方接着等，而不是把空结果当成跑完了。
        """
        hist = await self._get(f"/history/{prompt_id}")
        entry = hist.get(prompt_id)
        if entry is None:
            return None if missing_ok else {}
        status = entry.get("status", {})
        if status.get("status_str") == "error":
            raise ExecutionError(self._error_from_history(status))
        if missing_ok and not status.get("completed", True):
            return None
        return entry.get("outputs", {})

    @staticmethod
    def _error_from_history(status: dict[str, Any]) -> str:
        for msg in status.get("messages", []):
            if isinstance(msg, (list, tuple)) and len(msg) >= 2 and msg[0] == "execution_error":
                detail = msg[1]
                return (f"节点 {detail.get('node_type')} 执行失败："
                        f"{detail.get('exception_message')}")
        return "任务执行失败"

    async def run(
        self,
        workflow: ApiWorkflow,
        on_progress: Callable[[JobProgress], None] | None = None,
        timeout_s: float | None = None,
    ) -> JobResult:
        """提交并等待完成。执行类错误会按配置重试，校验类错误直接抛。"""
        last: Exception | None = None
        for attempt in range(self.config.max_retries + 1):
            try:
                # 每个任务用独立的 clientId。ComfyUI 按 clientId 记订阅，
                # 同一个 id 上并发跑两个任务，后连的会把先连的挤下线，
                # 先连的那个永远等不到完成消息，白等到超时为止。
                job_id = f"{self.client_id}-{uuid.uuid4().hex[:8]}"
                prompt_id = await self._submit_as(workflow, job_id)
                return await self.wait(prompt_id, on_progress, timeout_s, job_id)
            except PromptValidationError:
                raise  # 重试也不会变好
            except (ExecutionError, ComfyUnavailable) as exc:
                last = exc
                if attempt < self.config.max_retries:
                    await asyncio.sleep(2 ** attempt)
        raise ExecutionError(f"重试 {self.config.max_retries} 次后仍失败：{last}") from last

    # ---- 文件 ----

    async def upload_image(self, path: str | Path, subfolder: str = "") -> str:
        """上传图片到服务端的 input 目录，返回可在工作流里引用的文件名。

        ComfyUI 可能在另一台机器上，所以不能直接传本地路径。
        """
        p = Path(path)
        if not p.is_file():
            raise ComfyError(f"要上传的文件不存在：{p}")
        data = {"overwrite": "true"}
        if subfolder:
            data["subfolder"] = subfolder
        async with httpx.AsyncClient(timeout=self.config.timeout_s) as http:
            with p.open("rb") as fh:
                r = await http.post(
                    f"{self.config.base_url}/upload/image",
                    files={"image": (p.name, fh)},
                    data=data,
                )
        r.raise_for_status()
        body = r.json()
        name = body["name"]
        sub = body.get("subfolder") or ""
        return f"{sub}/{name}" if sub else name

    async def download(self, file_ref: dict[str, Any], dest: str | Path) -> Path:
        """把产出文件下载到本地。同样因为服务端可能在别的机器上。"""
        params = {
            "filename": file_ref["filename"],
            "subfolder": file_ref.get("subfolder", ""),
            "type": file_ref.get("type", "output"),
        }
        target = Path(dest)
        target.parent.mkdir(parents=True, exist_ok=True)
        async with httpx.AsyncClient(timeout=self.config.job_timeout_s) as http:
            async with http.stream("GET", f"{self.config.base_url}/view",
                                   params=params) as r:
                r.raise_for_status()
                with target.open("wb") as fh:
                    async for chunk in r.aiter_bytes(65536):
                        fh.write(chunk)
        return target

    async def interrupt(self) -> None:
        async with httpx.AsyncClient(timeout=self.config.timeout_s) as http:
            await http.post(f"{self.config.base_url}/interrupt")
