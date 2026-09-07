"""中文字幕生成。

用 ASS 不用 SRT，因为 SRT 没法控样式和位置。

断行自己算，不依赖渲染库的自动换行。libass 对中文只按字符断不按语义断，
一句话会在词中间折断，观感很差。自己算好断点插换行符才对。
"""

from __future__ import annotations

import unicodedata
from dataclasses import dataclass
from pathlib import Path

# 断在这些标点之后是自然的
_BREAK_AFTER = "，。？！；：、…—"
# 这些不能出现在行首
_NO_LINE_START = "，。？！；：、）】》」』…—·%"
# 这些不能出现在行尾
_NO_LINE_END = "（【《「『"


def display_width(text: str) -> float:
    """按显示宽度算长度。全角算一，半角算半。"""
    total = 0.0
    for ch in text:
        total += 1.0 if unicodedata.east_asian_width(ch) in "WF" else 0.5
    return total


def wrap_chinese(text: str, max_width: float = 15.0, max_lines: int = 2) -> list[str]:
    """中文断行。

    优先在标点后断，其次在宽度上限处断，但要避开标点不能在行首行尾的情况。
    超过行数上限就返回，由调用方决定拆成两条字幕还是截断。
    """
    text = text.strip()
    if not text:
        return []
    if display_width(text) <= max_width:
        return [text]

    lines: list[str] = []
    rest = text
    while rest and len(lines) < max_lines:
        cut = _find_break(rest, max_width)
        if cut <= 0 or cut >= len(rest):
            lines.append(rest)
            rest = ""
            break
        lines.append(rest[:cut].strip())
        rest = rest[cut:].strip()

    if rest:
        # 塞不下的部分并进最后一行，宁可最后一行长一点也不丢字
        lines[-1] = (lines[-1] + rest).strip()
    return [ln for ln in lines if ln]


def _find_break(text: str, max_width: float) -> int:
    """找一个断点。返回切分位置。"""
    width = 0.0
    limit_idx = len(text)
    for i, ch in enumerate(text):
        width += 1.0 if unicodedata.east_asian_width(ch) in "WF" else 0.5
        if width > max_width:
            limit_idx = i
            break

    # 在宽度范围内从后往前找标点
    for i in range(limit_idx - 1, max(0, limit_idx - 8), -1):
        if text[i] in _BREAK_AFTER:
            return i + 1

    # 没有标点就在宽度上限处断，但避开非法位置
    cut = limit_idx
    guard = 0
    while cut > 1 and guard < 6:
        guard += 1
        if cut < len(text) and text[cut] in _NO_LINE_START:
            cut -= 1
            continue
        if text[cut - 1] in _NO_LINE_END:
            cut -= 1
            continue
        break
    return max(1, cut)


@dataclass
class SubtitleCue:
    """一条字幕。"""

    start_s: float
    end_s: float
    text: str
    style: str = "dialogue"

    @property
    def duration_s(self) -> float:
        return self.end_s - self.start_s


def _ass_time(seconds: float) -> str:
    """ASS 的时间格式：时:分:秒.厘秒。"""
    seconds = max(0.0, seconds)
    h = int(seconds // 3600)
    m = int((seconds % 3600) // 60)
    s = seconds % 60
    return f"{h:d}:{m:02d}:{s:05.2f}"


def build_ass(
    cues: list[SubtitleCue],
    width: int = 1080,
    height: int = 1920,
    font: str = "Source Han Sans SC",
    font_size: int = 54,
    max_chars_per_line: int = 15,
    max_lines: int = 2,
    margin_v: int = 180,
) -> str:
    """生成 ASS 字幕文件内容。

    竖屏短剧字幕通常放在下方偏上一点，避开平台的界面元素。
    """
    header = f"""[Script Info]
ScriptType: v4.00+
PlayResX: {width}
PlayResY: {height}
WrapStyle: 2
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: dialogue,{font},{font_size},&H00FFFFFF,&H000000FF,&H00000000,&H64000000,0,0,0,0,100,100,0,0,1,3,1,2,60,60,{margin_v},1
Style: narration,{font},{int(font_size * 0.92)},&H00E8E8E8,&H000000FF,&H00000000,&H64000000,0,1,0,0,100,100,0,0,1,3,1,2,60,60,{margin_v},1
Style: title,{font},{int(font_size * 1.4)},&H00FFFFFF,&H000000FF,&H00000000,&H96000000,1,0,0,0,100,100,2,0,1,4,2,5,60,60,0,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
"""
    lines = [header]
    # WrapStyle 2 表示只在显式换行符处断行，这正是我们要的：
    # 断点由上面的 wrap_chinese 算好，不让渲染库自作主张。
    for cue in cues:
        if not cue.text.strip():
            continue
        wrapped = wrap_chinese(cue.text, max_chars_per_line, max_lines)
        body = r"\N".join(wrapped)
        style = cue.style if cue.style in ("dialogue", "narration", "title") else "dialogue"
        lines.append(
            f"Dialogue: 0,{_ass_time(cue.start_s)},{_ass_time(cue.end_s)},"
            f"{style},,0,0,0,,{body}"
        )
    return "\n".join(lines) + "\n"


def write_ass(path: str | Path, cues: list[SubtitleCue], **kwargs) -> Path:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    # ASS 要 UTF-8，加 BOM 能让一些播放器正确识别中文
    target.write_text(build_ass(cues, **kwargs), encoding="utf-8-sig")
    return target


def validate_cues(cues: list[SubtitleCue], max_chars_per_line: int = 15) -> list[str]:
    """字幕自检。装配后闸门要用。"""
    problems: list[str] = []
    for i, cue in enumerate(cues):
        if cue.end_s <= cue.start_s:
            problems.append(f"第 {i + 1} 条字幕时间倒挂")
        if not cue.text.strip():
            problems.append(f"第 {i + 1} 条字幕是空的")
        if cue.duration_s < 0.4:
            problems.append(f"第 {i + 1} 条字幕只显示 {cue.duration_s:.2f} 秒，看不清")
        wrapped = wrap_chinese(cue.text, max_chars_per_line)
        if any(display_width(ln) > max_chars_per_line * 1.6 for ln in wrapped):
            problems.append(f"第 {i + 1} 条字幕断行后仍然超长")
    for a, b in zip(cues, cues[1:]):
        if b.start_s < a.end_s - 0.01:
            problems.append(f"字幕重叠：{a.text[:10]} 与 {b.text[:10]}")
    return problems
