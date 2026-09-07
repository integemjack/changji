"""写剧本。

流水线的第一步。以前这一步是空的，用户得自己写好剧本粘进来，
可整套东西要解决的问题就是「用 AI 写剧本、编排、出片」，
少了这一步，前面那句话只兑现了一半。

产出不是一段自由文本，是一张结构化的场次表，再由我们自己渲染成
「名字：台词」的写法。让模型直接写剧本格式的话，它一会儿用冒号
一会儿用括号，一会儿把旁白也写成对白，后面识别角色就开始出错。
形式我们定，模型只管内容。

时长靠字数控制。中文普通话大约每秒 4.6 个字，一集 60 秒的对白量
就是两百多字。不给这个预算的话，模型写出来的东西按时长算能拍十分钟。
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any

import httpx

from ..config import LLMConfig
from ..models.character import StyleLine
from .audio import CHARS_PER_SECOND


class ScriptError(RuntimeError):
    pass


# 模型爱给动作行加一对括号，给台词加一对引号，无论提示词里怎么说。
# 这些符号会一路流到分镜提示词和字幕里，所以在入口就削掉。
# 形式是我们定的，不是模型定的。
_WRAPPERS = (("（", "）"), ("(", ")"), ("【", "】"), ("[", "]"),
             ("“", "”"), ('"', '"'), ("「", "」"), ("『", "』"), ("'", "'"))


def _wraps_whole(text: str, left: str, right: str) -> bool:
    """首尾这一对是不是套住整段的那一对。

    光数左右个数不够。「（甲说）乙答（丙笑）」左右各两个，数目相等，
    但首尾那两个并不是一对，削掉就把中间的括号弄错位了。
    要从头扫一遍看深度什么时候回到零。
    """
    if left == right:
        # 引号这类左右一样的，中间不能再出现同一个符号
        return right not in text[len(left):-len(right)]
    depth = 0
    for i, ch in enumerate(text):
        if ch == left:
            depth += 1
        elif ch == right:
            depth -= 1
            if depth == 0:
                return i == len(text) - 1
            if depth < 0:
                return False
    return False


def strip_wrapper(text: str) -> str:
    """削掉整段外面套的那一对括号或引号。

    只削套住整段的那一层，中间的括号是内容的一部分，不动。
    """
    out = text.strip()
    changed = True
    while changed and len(out) >= 2:
        changed = False
        for left, right in _WRAPPERS:
            if not (out.startswith(left) and out.endswith(right)):
                continue
            if not _wraps_whole(out, left, right):
                continue
            inner = out[len(left):-len(right)].strip()
            if not inner:
                continue
            out, changed = inner, True
            break
    return out


# 模型爱在动作行开头挂一个时间码，比如「[0-3秒] 画面特写：…」。
# 提示词里没让它写，schema 里也没有这一项，它自己加的。
# 这段文字会原样进到分镜提示词里，让画面模型去理解一个时间码。
#
# 只削开头那一对括号，而且要括号里确实像时间码才削：带数字，
# 并且带秒、s 或者冒号。不做这个限制的话，「（他犹豫了）他开口」
# 这种正常写法开头也会被削掉。
_LEAD_BRACKETS = (("[", "]"), ("【", "】"), ("（", "）"), ("(", ")"))
_TIME_HINT = re.compile(r"\d")
_TIME_UNIT = ("秒", "s", "S", ":", "：", "分", "帧")


def strip_leading_timecode(text: str) -> str:
    """削掉动作行开头的时间码标记。"""
    out = text.strip()
    for left, right in _LEAD_BRACKETS:
        if not out.startswith(left):
            continue
        end = out.find(right)
        if end <= 0:
            continue
        inside = out[len(left):end]
        if not _TIME_HINT.search(inside):
            continue
        if not any(u in inside for u in _TIME_UNIT):
            continue
        return out[end + len(right):].strip()
    return out


# 模型表示「没人说话」的各种写法。schema 里写的是填空字符串，
# 但它经常填 none、null、旁白 这类词。原样当成名字的话，
# 成片里会出现一个叫 none 的角色，字幕上写着「none：寂静」。
_NO_SPEAKER = {
    "none", "null", "nil", "n/a", "na", "-", "—", "无", "空",
    "旁白", "画外音", "narrator", "voiceover", "vo", "ost",
    "none.", "（无）", "(none)",
}


def normalize_speaker(raw: object) -> str:
    """把「没人说话」的各种写法统一成空字符串。"""
    name = strip_wrapper(str(raw or ""))
    return "" if name.strip().lower() in _NO_SPEAKER else name


# 一集里对白占的比重。剩下的是动作和环境描写，不发声但占画面时长。
# 全是对白的话没有留白，成片像念稿子。
DIALOGUE_SHARE = 0.62


def budget_chars(duration_s: float) -> int:
    """这个时长大概能装多少字对白。"""
    return max(20, int(duration_s * CHARS_PER_SECOND * DIALOGUE_SHARE))


_SCHEMA: dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "required": ["title", "logline", "beats"],
    "properties": {
        "title": {
            "type": "string",
            "description": "这一集的标题，六个字以内",
        },
        "logline": {
            "type": "string",
            "description": "一句话说清这一集发生了什么，给人看的，不进成片",
        },
        "beats": {
            "type": "array",
            "minItems": 4,
            "description": "按时间顺序排的场次。动作和对白交替，不要连着五句对白",
            "items": {
                "type": "object",
                "additionalProperties": False,
                "required": ["kind", "speaker", "text"],
                "properties": {
                    "kind": {
                        "type": "string",
                        "enum": ["action", "dialogue"],
                        "description": "action 是动作或环境描写，dialogue 是有人说话",
                    },
                    "speaker": {
                        "type": "string",
                        "description": "说话的人。kind 是 action 时填空字符串",
                    },
                    "text": {
                        "type": "string",
                        "description": "这一拍的内容。对白只写说出口的话，"
                                       "不要带引号也不要带名字前缀",
                    },
                },
            },
        },
    },
}


@dataclass
class ScriptDraft:
    """写出来的一集。"""

    title: str
    logline: str
    beats: list[dict[str, str]] = field(default_factory=list)

    @property
    def speakers(self) -> list[str]:
        seen: list[str] = []
        for b in self.beats:
            name = b.get("speaker", "").strip()
            if b.get("kind") == "dialogue" and name and name not in seen:
                seen.append(name)
        return seen

    @property
    def dialogue_chars(self) -> int:
        return sum(len(b["text"]) for b in self.beats
                   if b.get("kind") == "dialogue")

    def render(self) -> str:
        """渲染成后面几步认的写法。

        对白一律「名字：台词」，动作单独成行。格式由我们定死，
        不看模型心情，否则下一步识别角色就开始出错。
        """
        lines: list[str] = []
        for b in self.beats:
            text = b.get("text", "").strip()
            if not text:
                continue
            if b.get("kind") == "dialogue":
                name = b.get("speaker", "").strip()
                lines.append(f"{name}：{text}" if name else text)
            else:
                lines.append(text)
        return "\n".join(lines)


def build_prompt(
    premise: str, duration_s: float, style_line: StyleLine,
    previous: str = "", characters: list[str] | None = None,
) -> str:
    chars = budget_chars(duration_s)
    style_hint = (
        "真人写实短剧，台词生活化，不要文绉绉的书面语"
        if style_line is StyleLine.REALISTIC
        else "动漫短剧，台词可以更有戏剧张力，但仍要口语化"
    )
    parts = [
        f"你在写一集竖屏短剧，总时长约 {duration_s:.0f} 秒。{style_hint}。",
        "",
        "硬性要求：",
        f"1. 所有对白加起来控制在 {chars} 个字左右，超出很多就是拍不完。",
        "2. 动作和对白交替推进，不要连着好几句对白，画面会没有呼吸。",
        "3. 出场角色不超过三个。人一多，短剧里根本立不住。",
        "4. 对白只写说出口的话，不要带引号，不要在 text 里重复人名。",
        "5. 同一个角色的名字前后必须一模一样，不要一会儿全名一会儿简称。",
        "6. 开头三秒就要有事发生，短剧没有铺垫的余地。",
        "7. 结尾留一个钩子或者一个明确的情绪落点。",
    ]
    if characters:
        parts += [
            "",
            f"必须沿用这些已有角色，名字一字不改：{'、'.join(characters)}。",
        ]
    if previous:
        parts += [
            "",
            "前面几集的剧本如下，这一集要接着往下写，人物关系和已经发生的事"
            "不能推翻：",
            "",
            previous.strip()[:4000],
        ]
    parts += [
        "",
        "这一集要写的：",
        "",
        premise.strip(),
        "",
        "只输出 JSON，不要任何解释文字。",
    ]
    return "\n".join(parts)


class ScriptGenerator:
    """从一句梗概写出一集剧本。"""

    def __init__(self, config: LLMConfig) -> None:
        self.config = config

    async def generate(
        self, premise: str, duration_s: float = 60.0,
        style_line: StyleLine = StyleLine.REALISTIC,
        previous: str = "", characters: list[str] | None = None,
    ) -> ScriptDraft:
        if not premise.strip():
            raise ScriptError("得先说清楚这一集要写什么")
        raw = await self._complete(
            build_prompt(premise, duration_s, style_line, previous, characters))
        return self._parse(raw)

    async def _complete(self, prompt: str) -> str:
        payload: dict[str, Any] = {
            "model": self.config.model,
            "messages": [{"role": "user", "content": prompt}],
            "temperature": self.config.temperature,
            "response_format": {
                "type": "json_schema",
                "json_schema": {"name": "script", "strict": True,
                                "schema": _SCHEMA},
            },
        }
        headers = {"Authorization": f"Bearer {self.config.api_key}"}
        async with httpx.AsyncClient(timeout=self.config.timeout_s) as http:
            try:
                r = await http.post(f"{self.config.base_url}/chat/completions",
                                    json=payload, headers=headers)
            except httpx.RequestError as exc:
                raise ScriptError(
                    f"连不上大模型服务（{self.config.base_url}）。\n{exc}"
                ) from exc
            if r.status_code >= 400:
                # 有些服务不支持 json_schema，退回宽松的 json_object
                payload["response_format"] = {"type": "json_object"}
                r = await http.post(f"{self.config.base_url}/chat/completions",
                                    json=payload, headers=headers)
            r.raise_for_status()
            body = r.json()
        try:
            return body["choices"][0]["message"]["content"]
        except (KeyError, IndexError) as exc:
            raise ScriptError(f"大模型返回格式异常：{body}") from exc

    @staticmethod
    def _parse(raw: str) -> ScriptDraft:
        from .storyboard import _extract_json

        data = _extract_json(raw)
        if not isinstance(data, dict):
            raise ScriptError("大模型没有返回对象")
        beats_raw = data.get("beats") or []
        if not isinstance(beats_raw, list) or not beats_raw:
            raise ScriptError("大模型没写出任何内容")

        beats: list[dict[str, str]] = []
        for item in beats_raw:
            if not isinstance(item, dict):
                continue
            text = strip_wrapper(
                strip_leading_timecode(str(item.get("text") or "")))
            if not text:
                continue
            kind = "dialogue" if item.get("kind") == "dialogue" else "action"
            speaker = normalize_speaker(item.get("speaker"))
            if kind == "dialogue" and not speaker:
                # 说了话却没说是谁说的，当旁白处理。丢掉的话这句台词
                # 就从成片里消失了，那比配错声音还糟。
                kind = "action"
            beats.append({"kind": kind, "speaker": speaker, "text": text})

        if not beats:
            raise ScriptError("大模型写的内容全是空的")
        if not any(b["kind"] == "dialogue" for b in beats):
            raise ScriptError("整集一句台词都没有，这样出来的是默片")

        return ScriptDraft(
            title=str(data.get("title") or "").strip(),
            logline=str(data.get("logline") or "").strip(),
            beats=beats,
        )
