"""大模型返回非 2xx 时怎么说人话。

三个用大模型的阶段（剧本、角色圣经、分镜）本来都是 `r.raise_for_status()`
了事。那会抛 httpx 的 HTTPStatusError，Web 层只认各阶段自己的错误类型，
于是这类失败一路穿到最外面变成一句「Internal Server Error」。

而这恰恰是最常见的一类失败：地址填错了、模型名写错了、密钥过期了。
用户看到的应该是「去设置里改地址」，不是一个 500。
"""

from __future__ import annotations

import httpx

from ..config import LLMConfig


def explain(config: LLMConfig, response: httpx.Response) -> str:
    """把 HTTP 状态码翻成一句能照着做的话。"""
    url = f"{config.base_url}/chat/completions"
    detail = ""
    try:
        body = response.json()
        detail = str(body.get("error") or body.get("message") or "")[:200]
    except Exception:
        detail = response.text[:200]

    if response.status_code == 404:
        # 404 有两种：地址不对，和模型没拉下来。Ollama 两种都回 404，
        # 一律说「地址填错了」会把人支到错误的地方去查。
        low = detail.lower()
        if "model" in low and ("not found" in low or "not exist" in low):
            hint = (
                f"大模型服务在，但它没有 {config.model} 这个模型。"
                f"本地 Ollama 的话先 ollama pull {config.model}，"
                f"或者去设置页的「大模型」那一节换一个已经有的模型名。"
            )
        else:
            hint = (
                f"大模型服务在 {url} 上没有这个接口。"
                f"多半是地址填错了——地址要带 /v1 结尾，"
                f"而且那台机器上的服务得真的起着。去设置页的「大模型」那一节改。"
            )
    elif response.status_code in (401, 403):
        hint = f"大模型服务拒绝了这次请求（{response.status_code}），八成是 API Key 不对。去设置页改。"
    elif response.status_code == 429:
        hint = "大模型服务说请求太频繁了，等一会儿再试。"
    elif response.status_code >= 500:
        hint = (
            f"大模型服务自己出错了（{response.status_code}）。"
            f"本地服务的话看一眼它的日志，云服务的话过一会儿再试。"
        )
    else:
        hint = f"大模型服务返回 {response.status_code}。"

    model = f"\n当前模型：{config.model}" if config.model else ""
    return hint + model + (f"\n服务说：{detail}" if detail else "")


def raise_for_status(config: LLMConfig, response: httpx.Response, error_cls) -> None:
    """状态码不对就抛出这个阶段自己的错误，带一句人能看懂的话。"""
    if response.status_code < 400:
        return
    raise error_cls(explain(config, response))
