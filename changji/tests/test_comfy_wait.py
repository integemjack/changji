# -*- coding: utf-8 -*-
"""等任务完成这件事不能只信 WebSocket。

实测踩过的坑：一集里三个配音任务，并发跑两个，结果第一个永远等不到
完成消息，整条流水线卡在配音阶段十分钟不动，最后要等满半小时的
job_timeout 才报错。ComfyUI 那边三个任务全都成功了。

原因是 ComfyUI 按 clientId 记订阅，同一个 id 上后连的 WebSocket 会把
先连的挤下线。另外任务也可能在 WebSocket 连上之前就跑完，那条完成
消息同样不会补发。

所以两件事都要做：每个任务用独立的 clientId，以及静默一段时间就去
查一次历史兜底。
"""
import asyncio

import pytest

from changji.comfy.client import ComfyClient, ExecutionError, JobResult
from changji.config import ComfyConfig


class FakeWS:
    """一个永远收不到消息的 WebSocket。模拟被挤下线的那个连接。"""

    def __init__(self, messages=None):
        self._messages = list(messages or [])

    async def recv(self):
        if self._messages:
            return self._messages.pop(0)
        await asyncio.sleep(3600)  # 永远等不到

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False


@pytest.fixture
def client():
    return ComfyClient(ComfyConfig(base_url="http://x:8188", job_timeout_s=60))


@pytest.mark.asyncio
async def test_没消息也能从历史里认领结果(client, monkeypatch):
    """完成消息丢了，兜底查询要能把结果捞回来。"""
    monkeypatch.setattr("changji.comfy.client._RECHECK_EVERY_S", 0.05)

    calls = []

    async def fake_get(path):
        calls.append(path)
        if len(calls) < 3:
            return {}  # 前两次还没跑完
        return {"p1": {"status": {"status_str": "success", "completed": True},
                       "outputs": {"3": {"audio": [{"filename": "a.flac"}]}}}}

    monkeypatch.setattr(client, "_get", fake_get)
    result = await asyncio.wait_for(
        client._consume(FakeWS(), "p1", None,
                        asyncio.get_running_loop().time() + 10),
        timeout=5)
    assert isinstance(result, JobResult)
    assert result.files("audio")[0]["filename"] == "a.flac"
    assert len(calls) >= 3, "得反复查，不能查一次就放弃"


@pytest.mark.asyncio
async def test_还没跑完不能当成跑完了(client, monkeypatch):
    """历史里查不到就接着等，不能把空结果当成成功。"""
    monkeypatch.setattr("changji.comfy.client._RECHECK_EVERY_S", 0.02)

    async def fake_get(path):
        return {}

    monkeypatch.setattr(client, "_get", fake_get)
    with pytest.raises(ExecutionError, match="超时"):
        await asyncio.wait_for(
            client._consume(FakeWS(), "p1", None,
                            asyncio.get_running_loop().time() + 0.3),
            timeout=5)


@pytest.mark.asyncio
async def test_历史里报错要抛出来(client, monkeypatch):
    monkeypatch.setattr("changji.comfy.client._RECHECK_EVERY_S", 0.02)

    async def fake_get(path):
        return {"p1": {"status": {
            "status_str": "error", "completed": False,
            "messages": [["execution_error",
                          {"node_type": "KSampler",
                           "exception_message": "显存不够"}]]}}}

    monkeypatch.setattr(client, "_get", fake_get)
    with pytest.raises(ExecutionError, match="显存不够"):
        await asyncio.wait_for(
            client._consume(FakeWS(), "p1", None,
                            asyncio.get_running_loop().time() + 5),
            timeout=5)


@pytest.mark.asyncio
async def test_每个任务用独立的_clientid(client, monkeypatch):
    """并发时共用一个 clientId，先连的那个会被挤下线。"""
    seen = []

    async def fake_submit_as(workflow, client_id):
        seen.append(client_id)
        return "p" + str(len(seen))

    async def fake_wait(prompt_id, on_progress=None, timeout_s=None,
                        client_id=None):
        seen.append(("wait", client_id))
        return JobResult(prompt_id)

    monkeypatch.setattr(client, "_submit_as", fake_submit_as)
    monkeypatch.setattr(client, "wait", fake_wait)

    from changji.comfy.workflow import ApiWorkflow
    wf = ApiWorkflow({"1": {"class_type": "X", "inputs": {}}})
    await client.run(wf)
    await client.run(wf)

    ids = [x for x in seen if isinstance(x, str)]
    assert len(set(ids)) == 2, "两次运行必须是两个 clientId"
    assert all(i.startswith(client.client_id) for i in ids)
    # 提交用的 id 和监听用的 id 必须是同一个，否则照样收不到消息
    waits = [x[1] for x in seen if isinstance(x, tuple)]
    assert waits == ids


class TestAudioProgress:
    """配音要逐镜报进度，否则界面上是一根不动的进度条。"""

    @pytest.mark.asyncio
    async def test_每配完一个镜头回调一次(self, tmp_path):
        from changji.config import TTSConfig
        from changji.models.character import AssetLibrary
        from changji.models.project import ProjectStore
        from changji.models.shot import Shot, ShotSize
        from changji.stages.audio import AudioStage, EstimateBackend

        store = ProjectStore.create(tmp_path / "p", "drama")
        shots = [Shot(shot_id=f"sh{i:03d}", scene_id="s1", order=i,
                      duration_s=3.0, shot_size=ShotSize.MS)
                 for i in range(4)]
        seen = []
        stage = AudioStage(EstimateBackend(), TTSConfig(), store.paths)
        await stage.run(shots, AssetLibrary(), concurrency=2,
                        on_shot=lambda plan, done, total: seen.append((done, total)))
        assert len(seen) == 4
        assert sorted(d for d, _ in seen) == [1, 2, 3, 4], "计数要单调递增不重号"
        assert {t for _, t in seen} == {4}


class TestHumanTime:
    """等的人要知道还剩多久，「还剩 1847 秒」没人愿意心算。"""

    def test_说人话(self):
        from changji.pipeline import human_time
        assert human_time(5) == "5 秒"
        assert human_time(90) == "2 分钟"
        assert human_time(1847) == "31 分钟"
        assert human_time(7200) == "2.0 小时"

    def test_负数不出现(self):
        from changji.pipeline import human_time
        assert human_time(-10) == "0 秒"
