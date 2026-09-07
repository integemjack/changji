# -*- coding: utf-8 -*-
"""配音后端选择与 ComfyUI 后端。

推理应该跑在装了显卡和 PyTorch 的那台机器上，编排引擎不背推理框架的依赖。
"""
import pytest

from changji.comfy.workflow import ApiWorkflow
from changji.config import TTSConfig
from changji.models.project import ProjectStore
from changji.stages.audio import (
    AudioError, ComfyTTSBackend, EstimateBackend, HttpTTSBackend,
    _apply_text, build_backend,
)


class TestBackendSelection:
    def test_默认退回估算后端(self, tmp_path):
        """没配任何东西时也要能跑通，不能直接报错。"""
        b = build_backend(TTSConfig())
        assert isinstance(b, EstimateBackend)

    def test_显式配http就用http(self):
        b = build_backend(TTSConfig(backend="http", base_url="http://x:9000"))
        assert isinstance(b, HttpTTSBackend)

    def test_配了http却没填地址会报错(self):
        with pytest.raises(AudioError, match="tts.base_url"):
            build_backend(TTSConfig(backend="http"))

    def test_有工作流才用comfy后端(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        wf = ApiWorkflow({"1": {"class_type": "X", "inputs": {"text": ""}}})
        b = build_backend(TTSConfig(backend="comfy"), object(), wf, store.paths)
        assert isinstance(b, ComfyTTSBackend)

    def test_没有工作流时不用comfy后端(self, tmp_path):
        """配了 comfy 但项目里没放配音工作流，退回估算而不是崩掉。"""
        store = ProjectStore.create(tmp_path / "p", "p")
        b = build_backend(TTSConfig(backend="comfy"), object(), None, store.paths)
        assert isinstance(b, EstimateBackend)


class TestApplyText:
    """不同 TTS 引擎的节点参数名不同，按键名匹配而不是按节点类型。"""

    def test_填入text键(self):
        wf = ApiWorkflow({"1": {"class_type": "AnyTTS", "inputs": {"text": ""}}})
        assert _apply_text(wf, "你好", None, "neutral") is True
        assert wf.prompt["1"]["inputs"]["text"] == "你好"

    def test_兼容其它键名(self):
        for key in ("prompt", "input_text", "tts_text", "content"):
            wf = ApiWorkflow({"1": {"class_type": "T", "inputs": {key: ""}}})
            assert _apply_text(wf, "台词", None, "neutral") is True
            assert wf.prompt["1"]["inputs"][key] == "台词"

    def test_填入音色(self):
        wf = ApiWorkflow({"1": {"class_type": "T",
                                "inputs": {"text": "", "voice": "default"}}})
        _apply_text(wf, "台词", "v_lin", "neutral")
        assert wf.prompt["1"]["inputs"]["voice"] == "v_lin"

    def test_填入情绪(self):
        wf = ApiWorkflow({"1": {"class_type": "T",
                                "inputs": {"text": "", "emotion": "calm"}}})
        _apply_text(wf, "台词", None, "anger")
        assert wf.prompt["1"]["inputs"]["emotion"] == "anger"

    def test_中性情绪不覆盖(self):
        wf = ApiWorkflow({"1": {"class_type": "T",
                                "inputs": {"text": "", "emotion": "预设"}}})
        _apply_text(wf, "台词", None, "neutral")
        assert wf.prompt["1"]["inputs"]["emotion"] == "预设"

    def test_找不到文本节点时返回假(self):
        """调用方据此报出可操作的错误，而不是静默产出空音频。"""
        wf = ApiWorkflow({"1": {"class_type": "T", "inputs": {"steps": 20}}})
        assert _apply_text(wf, "台词", None, "neutral") is False

    def test_不会误改数字参数(self):
        wf = ApiWorkflow({"1": {"class_type": "T",
                                "inputs": {"text": "", "speed": 1.0}}})
        _apply_text(wf, "台词", None, "neutral")
        assert wf.prompt["1"]["inputs"]["speed"] == 1.0


class TestComfyBackendErrors:
    async def test_工作流没有文本节点时报可操作的错(self, tmp_path):
        store = ProjectStore.create(tmp_path / "p", "p")
        wf = ApiWorkflow({"1": {"class_type": "T", "inputs": {"steps": 20}}})
        b = ComfyTTSBackend(object(), wf, store.paths)
        with pytest.raises(AudioError, match="workflows/tts.json"):
            await b.synthesize("台词", store.paths.audio / "x.wav")


class TestSilentAudioDetection:
    """回归测试：服务端报成功不代表配音真的成功。

    实测撞上过：ComfyUI 的 TTS 节点缺少 librosa 依赖，内部捕获异常后
    输出一个一秒的空音频然后正常返回，任务状态报的是 success。
    只信状态码的客户端会拿到一堆静音文件还以为配音成功了。
    """

    def _check(self, duration, text="三年了，我该走了。", size=9000, tmp=None):
        from changji.stages.audio import _reject_silent_audio
        p = tmp / "a.flac"
        p.write_bytes(b"\x00" * size)
        _reject_silent_audio(p, duration, text)

    def test_正常时长的音频通过(self, tmp_path):
        self._check(2.6, tmp=tmp_path)  # 九个字约 2.6 秒

    def test_一秒空音频被拦下(self, tmp_path):
        with pytest.raises(AudioError, match="疑似空音频"):
            self._check(1.0, tmp=tmp_path)

    def test_报错里带上台词和期望时长(self, tmp_path):
        with pytest.raises(AudioError) as e:
            self._check(1.0, text="你到底想让我怎么做", tmp=tmp_path)
        msg = str(e.value)
        assert "你到底想让我怎么做" in msg
        assert "ComfyUI 的日志" in msg  # 指出下一步该去哪看

    def test_明显短于估算的也被拦下(self, tmp_path):
        """长台词只出了一点点声音，同样是失败。"""
        long_text = "这是一段相当长的台词需要说上好一会儿才能说完整句话"
        with pytest.raises(AudioError, match="疑似空音频"):
            self._check(1.5, text=long_text, tmp=tmp_path)

    def test_短台词的短音频不误判(self, tmp_path):
        """两个字的台词本来就短，不能因为短就判失败。"""
        self._check(1.2, text="走吧", tmp=tmp_path)


class TestVoiceApplied:
    """音色要真的填进工作流，否则全剧一个声音。"""

    def test_narrator_voice_也算音色键(self):
        from changji.stages.audio import _apply_text
        from changji.comfy.workflow import ApiWorkflow

        wf = ApiWorkflow({"2": {"class_type": "UnifiedTTSTextNode", "inputs": {
            "text": "占位", "narrator_voice": "voices_examples/x/en.wav"}}})
        assert _apply_text(wf, "我回来了。", "voices_examples/vibevoice/zh-Xinran_woman.wav", "")
        got = wf.prompt["2"]["inputs"]
        assert got["text"] == "我回来了。"
        assert got["narrator_voice"].endswith("zh-Xinran_woman.wav")

    def test_内置工作流的默认音色必须在节点列表里(self):
        """默认值也不能乱填。

        实测填过 voices_examples/vibevoice/zh-Xinran_woman.wav，
        文件在磁盘上，但不在这个节点的下拉框里，提交直接被拒。
        默认值只能用装 TTS-Audio-Suite 就一定有的那几条。
        """
        import json
        from changji import bundled_workflow

        wf = json.loads(bundled_workflow("tts.json").read_text(encoding="utf-8"))
        voice = wf["2"]["inputs"]["narrator_voice"]
        assert voice.startswith("voices_examples/")
        assert "vibevoice" not in voice, "vibevoice 的样本不在这个节点的列表里"


class TestPickVoice:
    """音色从服务端实际给的列表里挑，不能写死。"""

    OPTIONS = [
        "none",
        "voices_examples/higgs_audio/belinda.wav",
        "voices_examples/female/female_01.wav",
        "voices_examples/female/female_02.wav",
        "voices_examples/male/male_01.wav",
        "voices_examples/higgs_audio/zh_man_sichuan.wav",
    ]

    def test_女角色不能配男声(self):
        from changji.models.character import pick_voice
        v = pick_voice(self.OPTIONS, "female", 0)
        assert "female" in v or "belinda" in v, v
        assert "zh_man" not in v, "唯一的中文样本是男声，也不能给女角色用"

    def test_男角色优先中文样本(self):
        from changji.models.character import pick_voice
        assert "zh_man" in pick_voice(self.OPTIONS, "male", 0)

    def test_同性别多个角色不重号(self):
        from changji.models.character import pick_voice
        a = pick_voice(self.OPTIONS, "female", 0)
        b = pick_voice(self.OPTIONS, "female", 1)
        assert a != b

    def test_不会挑到_none(self):
        from changji.models.character import pick_voice
        assert pick_voice(["none"], "female", 0) is None

    def test_列表为空时返回空(self):
        from changji.models.character import pick_voice
        assert pick_voice([], "male", 0) is None


class TestUnknownVoiceFallback:
    """老项目里存的音色 id 服务端不认，不能因此整条流水线断掉。

    早年出角色设定时填的是 v_角色名 这种自造 id。那时候填音色的
    键名列表里没有 narrator_voice，这个 id 根本没被用上，所以没出事。
    补上键名之后它会被原样提交，节点直接拒绝，配音这一步就断了。
    """

    def _stage(self, tmp_path, voices):
        from changji.config import TTSConfig
        from changji.models.project import ProjectStore
        from changji.stages.audio import AudioStage, EstimateBackend

        backend = EstimateBackend()

        async def list_voices():
            return voices

        backend.list_voices = list_voices
        store = ProjectStore.create(tmp_path / "p", "d")
        return AudioStage(backend, TTSConfig(), store.paths)

    def _assets(self, voice_id):
        from changji.models.character import (
            AppearanceBlock, AssetLibrary, Character,
        )
        return AssetLibrary(characters={"c_a": Character(
            char_id="c_a", name="甲", voice_id=voice_id,
            voice_gender="female",
            appearance=AppearanceBlock(identity="女性", face="x", attire="y"))})

    @pytest.mark.asyncio
    async def test_不认识的音色退回自动挑(self, tmp_path):
        from changji.models.shot import DialogueLine

        pool = ["voices_examples/female/female_01.wav",
                "voices_examples/male/male_01.wav"]
        stage = self._stage(tmp_path, pool)
        got = await stage._voice_for(
            DialogueLine(char_id="c_a", text="喂"), self._assets("v_a"))
        assert got in pool, "服务端不认的 id 不能原样提交"
        assert "v_a" in stage.unknown_voices, "换掉了要记下来告诉用户"

    @pytest.mark.asyncio
    async def test_认识的音色照用不动(self, tmp_path):
        from changji.models.shot import DialogueLine

        pool = ["voices_examples/female/female_01.wav"]
        stage = self._stage(tmp_path, pool)
        got = await stage._voice_for(
            DialogueLine(char_id="c_a", text="喂"), self._assets(pool[0]))
        assert got == pool[0]
        assert not stage.unknown_voices

    @pytest.mark.asyncio
    async def test_问不到列表时照填的来(self, tmp_path):
        from changji.models.shot import DialogueLine

        stage = self._stage(tmp_path, [])
        got = await stage._voice_for(
            DialogueLine(char_id="c_a", text="喂"), self._assets("v_a"))
        assert got == "v_a", "问不到就别自作主张"
