# -*- coding: utf-8 -*-
"""中文字幕。断行自己算，不能交给渲染库。"""
import pytest

from changji.assembly.subtitles import (
    SubtitleCue, build_ass, display_width, validate_cues, wrap_chinese,
)


class TestDisplayWidth:
    def test_全角算一(self):
        assert display_width("你说什么") == 4.0

    def test_半角算半(self):
        assert display_width("abcd") == 2.0

    def test_中英混排(self):
        assert display_width("我用AI") == 3.0

    def test_空串为零(self):
        assert display_width("") == 0.0


class TestWrap:
    def test_短句不断行(self):
        assert wrap_chinese("你说什么", 15) == ["你说什么"]

    def test_优先在标点后断(self):
        lines = wrap_chinese("你到底想让我怎么做，我已经尽力了", 10)
        assert lines[0].endswith("，") or lines[0].endswith("做")
        assert len(lines) == 2

    def test_标点不落行首(self):
        """标点跑到行首是最难看的排版错误。"""
        for text in ["这件事情非常重要你一定要记住啊，千万别忘了",
                     "我们需要重新考虑整个方案的可行性问题。"]:
            for line in wrap_chinese(text, 8):
                assert line[0] not in "，。？！；：、）】》」』"

    def test_不丢字(self):
        text = "你到底想让我怎么做我已经把能做的都做完了还要我怎样"
        joined = "".join(wrap_chinese(text, 10, max_lines=2))
        assert len(joined) == len(text)

    def test_不超过行数上限(self):
        text = "这是一段特别特别长的台词" * 5
        assert len(wrap_chinese(text, 10, max_lines=2)) <= 2

    def test_空文本返回空(self):
        assert wrap_chinese("", 15) == []
        assert wrap_chinese("   ", 15) == []

    def test_单行上限被遵守(self):
        lines = wrap_chinese("我们需要重新考虑整个方案的可行性", 8, max_lines=2)
        assert display_width(lines[0]) <= 9.0


class TestAss:
    def _cues(self):
        return [
            SubtitleCue(0.0, 1.5, "你说什么"),
            SubtitleCue(1.6, 3.2, "我已经把能做的都做完了", "narration"),
        ]

    def test_生成合法ass头(self):
        out = build_ass(self._cues())
        assert "[Script Info]" in out
        assert "[V4+ Styles]" in out
        assert "[Events]" in out

    def test_时间格式正确(self):
        out = build_ass([SubtitleCue(65.25, 67.0, "测试")])
        assert "0:01:05.25" in out

    def test_断点用显式换行符(self):
        """WrapStyle 2 表示只在显式换行处断，断点由我们算。"""
        out = build_ass([SubtitleCue(0, 3, "我们需要重新考虑整个方案的可行性问题")],
                        max_chars_per_line=8)
        assert "WrapStyle: 2" in out
        assert r"\N" in out

    def test_旁白用独立样式(self):
        out = build_ass(self._cues())
        assert ",narration,," in out
        assert ",dialogue,," in out

    def test_空字幕被跳过(self):
        out = build_ass([SubtitleCue(0, 1, "   ")])
        assert "Dialogue:" not in out

    def test_字体可配置(self):
        assert "Noto Sans CJK SC" in build_ass(self._cues(), font="Noto Sans CJK SC")


class TestValidate:
    def test_正常字幕无问题(self):
        assert validate_cues([SubtitleCue(0, 1.5, "你说什么"),
                              SubtitleCue(1.6, 3.0, "我不知道")]) == []

    def test_检出时间倒挂(self):
        assert any("倒挂" in p for p in validate_cues([SubtitleCue(2.0, 1.0, "错")]))

    def test_检出重叠(self):
        problems = validate_cues([SubtitleCue(0, 2.0, "第一句"),
                                  SubtitleCue(1.0, 3.0, "第二句")])
        assert any("重叠" in p for p in problems)

    def test_检出一闪而过(self):
        assert any("看不清" in p for p in validate_cues([SubtitleCue(0, 0.2, "太快")]))

    def test_检出空字幕(self):
        assert any("空的" in p for p in validate_cues([SubtitleCue(0, 1.5, "  ")]))
