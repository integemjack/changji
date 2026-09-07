# -*- coding: utf-8 -*-
"""角色圣经生成。两阶段生成的第一阶段。"""
import pytest

from changji.models.character import AppearanceBlock, StyleLine
from changji.stages.bible import (
    BibleError, BibleGenerator, _clean, _default_negative, _slug, build_prompt,
)
from changji.config import LLMConfig


class TestSlug:
    def test_英文转小写下划线(self):
        assert _slug("Lin Wan") == "lin_wan"
        assert _slug("office-night") == "office_night"

    def test_中文名也能生成合法id(self):
        s = _slug("林晚")
        assert s and s.replace("_", "").isalnum()
        assert _slug("林晚") == _slug("林晚")  # 稳定

    def test_不同中文名不撞车(self):
        assert _slug("林晚") != _slug("王铎")


class TestClean:
    def test_去掉尾部句号(self):
        """回归测试。

        模型给的字段常自带句号，各段用逗号拼接后会变成
        「冷静克制。，身姿笔挺。，」这样标点重复的串，
        而且这个串会出现在每一个镜头的提示词里。
        """
        assert _clean("冷静而克制。") == "冷静而克制"
        assert _clean("身姿笔挺，") == "身姿笔挺"
        assert _clean("专业干练；") == "专业干练"

    def test_保留句中标点(self):
        assert _clean("黑色直发，发尾内扣。") == "黑色直发，发尾内扣"

    def test_压缩空白(self):
        assert _clean("  黑色  直发  ") == "黑色 直发"

    def test_空值安全(self):
        assert _clean(None) == ""
        assert _clean("") == ""


class TestAppearanceRender:
    def test_拼接时不产生重复标点(self):
        """兜底。用户手写的设定也常带句号。"""
        block = AppearanceBlock(
            identity="二十八岁女性，冷静克制。",
            body="身姿笔挺。",
            face="黑色直发，发尾内扣。",
            attire="灰色西装套裙。",
        )
        out = block.render(StyleLine.REALISTIC)
        assert "。，" not in out
        assert not out.endswith("。")
        assert "冷静克制，身姿笔挺" in out

    def test_动漫线用英文逗号(self):
        block = AppearanceBlock(identity="1girl", face="long black hair",
                                attire="school uniform")
        assert ", " in block.render(StyleLine.ANIME)

    def test_空段被跳过(self):
        block = AppearanceBlock(identity="女性", body="", face="长发", attire="西装")
        out = block.render(StyleLine.REALISTIC)
        assert "，，" not in out


class TestPrompt:
    def test_写实与动漫提示词不同(self):
        a = build_prompt("剧本", StyleLine.REALISTIC)
        b = build_prompt("剧本", StyleLine.ANIME)
        assert a != b
        assert "真人写实" in a
        assert "二次元" in b

    def test_强调面部要具体(self):
        p = build_prompt("剧本", StyleLine.REALISTIC)
        assert "逐字复用" in p
        assert "无法转成画面" in p

    def test_禁止凭空加人(self):
        assert "不要自己加人加景" in build_prompt("剧本", StyleLine.REALISTIC)


class TestNegative:
    def test_动漫线额外压写实(self):
        """Wan 有很强的写实偏置，不压的话动漫输入会被往真人方向拽。"""
        n = _default_negative(StyleLine.ANIME)
        assert "写实" in n and "真人" in n

    def test_写实线不含压写实的词(self):
        n = _default_negative(StyleLine.REALISTIC)
        assert "真人" not in n


class TestParse:
    def _gen(self):
        return BibleGenerator(LLMConfig())

    def test_解析正常输出(self):
        raw = """{"characters":[{"key":"lin_wan","name":"林晚",
        "identity":"二十八岁女性。","body":"清瘦","face":"黑色长发。",
        "attire":"灰西装。"}],"locations":[{"key":"office","name":"办公室",
        "space":"落地窗办公室","lighting":"冷调顶光"}],
        "global_style":"电影感。"}"""
        a = self._gen()._parse(raw, StyleLine.REALISTIC, "9:16")
        assert "c_lin_wan" in a.characters
        assert "loc_office" in a.locations
        assert a.characters["c_lin_wan"].appearance.identity == "二十八岁女性"
        assert a.style.global_style == "电影感"
        assert a.style.aspect_ratio == "9:16"

    def test_记下性别留到运行时挑音色(self):
        """音色不能在这一步写死。

        参考音色是 ComfyUI 那边的下拉框，装了哪些插件就有哪些选项，
        换一台机器列表就不一样。写死一条路径提交上去，节点校验不过，
        整条流水线断在配音这一步。所以这里只记性别和序号。
        """
        raw = """{"characters":[
        {"key":"lin","name":"林","identity":"二十八岁女性","face":"y","attire":"z"},
        {"key":"chen","name":"陈","identity":"三十岁男性","face":"y","attire":"z"}
        ],"locations":[],"global_style":"s"}"""
        a = self._gen()._parse(raw, StyleLine.REALISTIC, "9:16")
        lin = a.characters["c_lin"]
        chen = a.characters["c_chen"]
        assert lin.voice_id is None, "这一步不该定死音色"
        assert lin.voice_gender == "female"
        assert chen.voice_gender == "male"
        assert lin.voice_order != chen.voice_order, "序号要不同才能分到不同的声音"

    def test_性别看不出来时留空(self):
        raw = """{"characters":[
        {"key":"a","name":"甲","identity":"x","face":"y","attire":"z"}
        ],"locations":[],"global_style":"s"}"""
        a = self._gen()._parse(raw, StyleLine.REALISTIC, "9:16")
        assert a.characters["c_a"].voice_gender == ""

    def test_没有角色时报错(self):
        raw = '{"characters":[],"locations":[],"global_style":"x"}'
        with pytest.raises(BibleError, match="没有产出任何角色"):
            self._gen()._parse(raw, StyleLine.REALISTIC, "9:16")

    def test_代码块包裹也能解析(self):
        raw = """```json
        {"characters":[{"key":"a","name":"甲","identity":"x","face":"y",
        "attire":"z"}],"locations":[],"global_style":"s"}
        ```"""
        a = self._gen()._parse(raw, StyleLine.REALISTIC, "9:16")
        assert "c_a" in a.characters

    def test_生成的id能被分镜schema接受(self):
        """圣经产出的 id 必须能直接喂给分镜阶段做枚举。"""
        from changji.stages.storyboard import llm_shot_schema
        raw = """{"characters":[{"key":"lin_wan","name":"林晚","identity":"x",
        "face":"y","attire":"z"}],"locations":[{"key":"office","name":"办公室",
        "space":"s","lighting":"l"}],"global_style":"g"}"""
        a = self._gen()._parse(raw, StyleLine.REALISTIC, "9:16")
        schema = llm_shot_schema(a)
        assert schema["$defs"]["CharacterInShot"]["properties"]["char_id"]["enum"] \
            == ["c_lin_wan"]
