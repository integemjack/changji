"""角色圣经生成。

两阶段生成的第一阶段。先只产角色和场景，登记进资产库拿到 id，
第二阶段才产分镜，那时 schema 里已经没有外观字段可写了。

顺序不能反。先有分镜再补角色，模型已经在分镜里写过一遍外观，
之后再想统一就晚了。
"""

from __future__ import annotations

import re
from typing import Any

import httpx

from ..config import LLMConfig
from ..models.character import (
    AppearanceBlock, AssetLibrary, Character, Location, StyleLine, StyleProfile, guess_gender,
)
from ._llm import raise_for_status as _raise_for_status


class BibleError(RuntimeError):
    pass


_BIBLE_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "characters": {
            "type": "array",
            "description": "剧本里所有有名有姓或有台词的角色",
            "items": {
                "type": "object",
                "properties": {
                    "key": {
                        "type": "string",
                        "description": "英文小写下划线短标识，如 lin_wan",
                    },
                    "name": {"type": "string", "description": "剧本里的中文称呼"},
                    "identity": {
                        "type": "string",
                        "description": "身份：性别、年龄段、气质。一句话",
                    },
                    "body": {"type": "string", "description": "体型和身高感"},
                    "face": {
                        "type": "string",
                        "description": "五官、发型、发色、瞳色。这段会在几十个镜头里"
                                       "逐字复用，写得具体且不要含糊",
                    },
                    "attire": {"type": "string", "description": "默认服装"},
                },
                "required": ["key", "name", "identity", "face", "attire"],
                "additionalProperties": False,
            },
        },
        "locations": {
            "type": "array",
            "description": "剧本里出现的场景",
            "items": {
                "type": "object",
                "properties": {
                    "key": {"type": "string", "description": "英文小写下划线短标识"},
                    "name": {"type": "string", "description": "中文场景名"},
                    "space": {"type": "string", "description": "空间结构和布景"},
                    "lighting": {"type": "string", "description": "光线基调"},
                    "palette": {"type": "string", "description": "色彩方案"},
                },
                "required": ["key", "name", "space", "lighting"],
                "additionalProperties": False,
            },
        },
        "global_style": {
            "type": "string",
            "description": "全剧统一的画风、色温、质感。一句话",
        },
    },
    "required": ["characters", "locations", "global_style"],
    "additionalProperties": False,
}


def build_prompt(script: str, style_line: StyleLine) -> str:
    style_hint = (
        "画风是二次元动漫，描述用简洁的标签式短语。"
        if style_line is StyleLine.ANIME
        else "画风是真人写实，描述用自然的中文短句。"
    )
    return f"""你是一位短剧美术指导。读下面的剧本，产出角色设定和场景设定。

{style_hint}

要求：

1. 只写剧本里真实出现的角色和场景，不要自己加人加景。
2. face 这一段会在几十个镜头里被逐字复用，是角色能不能保持一致的关键。
   写具体的、可画出来的特征：发型、发色、脸型、眼型。
   不要写"好看""气质佳"这类无法转成画面的词。
3. identity 用一句话说清性别、年龄段、气质。
4. attire 写这个角色的默认服装。剧情中的换装不在这里写。
5. key 用英文小写和下划线，比如 lin_wan、office_night。
6. lighting 要说清时间和光质，比如"夜间冷调顶光，霓虹反光"。
7. global_style 是全剧统一的调子，所有镜头都会带上它。

剧本：

{script}

只输出 JSON，不要任何解释文字。"""


class BibleGenerator:
    """从剧本产出角色圣经和场景设定。"""

    def __init__(self, config: LLMConfig) -> None:
        self.config = config

    async def generate(
        self, script: str, style_line: StyleLine = StyleLine.REALISTIC,
        aspect_ratio: str = "9:16",
    ) -> AssetLibrary:
        raw = await self._complete(build_prompt(script, style_line))
        return self._parse(raw, style_line, aspect_ratio)

    async def _complete(self, prompt: str) -> str:
        payload = {
            "model": self.config.model,
            "messages": [{"role": "user", "content": prompt}],
            "temperature": self.config.temperature,
            "response_format": {
                "type": "json_schema",
                "json_schema": {"name": "bible", "strict": True,
                                "schema": _BIBLE_SCHEMA},
            },
        }
        headers = {"Authorization": f"Bearer {self.config.api_key}"}
        async with httpx.AsyncClient(timeout=self.config.timeout_s) as http:
            try:
                r = await http.post(f"{self.config.base_url}/chat/completions",
                                    json=payload, headers=headers)
            except httpx.RequestError as exc:
                raise BibleError(
                    f"连不上大模型服务（{self.config.base_url}）。\n{exc}"
                ) from exc
            if r.status_code >= 400:
                payload["response_format"] = {"type": "json_object"}
                r = await http.post(f"{self.config.base_url}/chat/completions",
                                    json=payload, headers=headers)
            _raise_for_status(self.config, r, BibleError)
            body = r.json()
        try:
            return body["choices"][0]["message"]["content"]
        except (KeyError, IndexError) as exc:
            raise BibleError(f"大模型返回格式异常：{body}") from exc

    def _parse(
        self, raw: str, style_line: StyleLine, aspect_ratio: str,
    ) -> AssetLibrary:
        from .storyboard import _extract_json

        data = _extract_json(raw)
        if not isinstance(data, dict):
            raise BibleError("大模型没有返回对象")

        characters: dict[str, Character] = {}
        for index, item in enumerate(data.get("characters") or []):
            key = _slug(item.get("key") or item.get("name", ""))
            if not key:
                continue
            char_id = f"c_{key}"
            try:
                characters[char_id] = Character(
                    char_id=char_id,
                    name=item.get("name") or key,
                    # 音色留空，配音时按服务端实际有哪些参考音频再定。
                    # 这里写死路径的话，换一台 ComfyUI 就可能对不上，
                    # 节点校验不过整条流水线直接断在配音这一步。
                    voice_id=None,
                    voice_gender=guess_gender(item.get("identity", "")),
                    voice_order=index,
                    appearance=AppearanceBlock(
                        identity=_clean(item.get("identity", "")),
                        body=_clean(item.get("body", "")),
                        face=_clean(item.get("face", "")),
                        attire=_clean(item.get("attire", "")),
                    ),
                )
            except Exception as exc:
                raise BibleError(f"角色 {key} 的设定不合法：{exc}") from exc

        locations: dict[str, Location] = {}
        for item in data.get("locations") or []:
            key = _slug(item.get("key") or item.get("name", ""))
            if not key:
                continue
            loc_id = f"loc_{key}"
            try:
                locations[loc_id] = Location(
                    location_id=loc_id,
                    name=item.get("name") or key,
                    space=_clean(item.get("space", "")),
                    lighting=_clean(item.get("lighting", "")),
                    palette=_clean(item.get("palette", "")),
                )
            except Exception as exc:
                raise BibleError(f"场景 {key} 的设定不合法：{exc}") from exc

        if not characters:
            raise BibleError(
                "大模型没有产出任何角色。检查剧本里是否真的有人物，"
                "或者换一个更强的模型"
            )

        return AssetLibrary(
            characters=characters,
            locations=locations,
            style=StyleProfile(
                style_line=style_line,
                global_style=_clean(data.get("global_style", "")),
                negative_prompt=_default_negative(style_line),
                aspect_ratio=aspect_ratio,
            ),
        )


def _slug(text: str) -> str:
    """转成合法的 id 片段。中文名也要能用。"""
    import hashlib

    s = re.sub(r"[^a-z0-9_]+", "_", str(text).lower()).strip("_")
    if not s:
        s = "x" + hashlib.sha1(str(text).encode("utf-8")).hexdigest()[:6]
    return s


def _clean(text: Any) -> str:
    """清洗模型给的字段。

    要去掉尾部的句号。这些字段拼提示词时用逗号连接，模型带来的句号会
    让结果变成「冷静克制。，身姿笔挺。，」这样标点重复的串，
    而且这个串会出现在每一个镜头里。
    """
    s = re.sub(r"\s+", " ", str(text or "")).strip()
    return s.rstrip("。.；;，,、 ")


def _default_negative(style_line: StyleLine) -> str:
    """默认负向提示词。

    动漫线要额外压写实倾向，因为 Wan 有很强的写实偏置，
    不压的话动漫输入会被往真人方向拽。
    """
    base = (
        "低质量，模糊，过曝，畸形，多余的手指，画得不好的手部，"
        "画得不好的脸部，静止不动的画面，字幕，水印"
    )
    if style_line is StyleLine.ANIME:
        return base + "，写实，照片质感，真人"
    return base
