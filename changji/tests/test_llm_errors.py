# -*- coding: utf-8 -*-
"""大模型返回非 2xx 时说人话。

这是整条流水线上最常踩的一类失败：地址填错、模型没拉下来、密钥过期。
三个用大模型的阶段原来都是 raise_for_status() 了事，抛出来的是 httpx 的
异常，Web 层只认各阶段自己的错误类型，于是一路穿到最外面变成一句
「Internal Server Error」——用户看不出该去改什么。
"""
import httpx
import pytest

from changji.config import LLMConfig
from changji.stages._llm import explain, raise_for_status
from changji.stages.bible import BibleError
from changji.stages.script import ScriptError
from changji.stages.storyboard import StoryboardError


def _resp(status: int, payload=None, text: str = "") -> httpx.Response:
    return httpx.Response(
        status,
        json=payload if payload is not None else None,
        text=None if payload is not None else text,
        request=httpx.Request("POST", "http://127.0.0.1:11434/v1/chat/completions"),
    )


@pytest.fixture
def config():
    return LLMConfig(base_url="http://127.0.0.1:11434/v1", model="qwen3:14b")


class TestExplain:
    def test_模型没拉下来时别去怪地址(self, config):
        """Ollama 地址对、模型没拉，也回 404。

        一律说「地址填错了」会把人支到错误的地方查，这比不给提示更费时间。
        """
        msg = explain(config, _resp(404, {
            "error": {"message": "model 'qwen3:14b' not found",
                      "type": "not_found_error"}}))
        assert "qwen3:14b" in msg
        assert "ollama pull" in msg
        assert "地址填错" not in msg

    def test_接口不存在才说地址(self, config):
        msg = explain(config, _resp(404, {"detail": "Not Found"}))
        assert "地址" in msg
        assert "/v1" in msg

    def test_密钥不对(self, config):
        assert "API Key" in explain(config, _resp(401, {"error": "bad key"}))
        assert "API Key" in explain(config, _resp(403, {"error": "forbidden"}))

    def test_限流(self, config):
        assert "太频繁" in explain(config, _resp(429, {"error": "slow down"}))

    def test_服务端自己崩了(self, config):
        msg = explain(config, _resp(503, {"error": "upstream down"}))
        assert "503" in msg

    def test_带上当前模型名(self, config):
        """报错里要有模型名。同一个地址上换个模型就好了的情况很常见。"""
        assert "qwen3:14b" in explain(config, _resp(500, {"error": "boom"}))

    def test_不是_json_也不能炸(self, config):
        msg = explain(config, _resp(502, text="<html>bad gateway</html>"))
        assert "502" in msg


class TestRaiseForStatus:
    def test_二百放过(self, config):
        raise_for_status(config, _resp(200, {"ok": True}), ScriptError)

    @pytest.mark.parametrize("error_cls", [ScriptError, BibleError, StoryboardError])
    def test_抛的是这个阶段自己的错误(self, config, error_cls):
        """Web 层按阶段的错误类型翻译成 400。抛 httpx 的异常就变 500 了。"""
        with pytest.raises(error_cls) as exc:
            raise_for_status(config, _resp(404, {"error": "nope"}), error_cls)
        assert "地址" in str(exc.value)
