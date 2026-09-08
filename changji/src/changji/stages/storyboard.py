"""剧本转分镜。

两阶段生成。第一遍只出角色圣经，第二遍才出分镜，而且第二遍的 schema 里
根本没有描述外观的字段。大模型想写也写不进去，一致性因此是结构保证
而不是提示词祈使。

角色 id 用枚举限定为已注册列表，模型编不出新角色。
时长用配额表下发，不让模型自由填，因为它在几十个镜头规模上做算术不可靠。
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any

import httpx

from ..config import LLMConfig
from ..models.character import AssetLibrary
from ..models.shot import Shot, apply_lipsync_rules
from .render import max_shot_duration_s
from ._llm import raise_for_status as _raise_for_status


class StoryboardError(RuntimeError):
    pass


# 视频模型支持的时长档位。分镜必须落在这些值上，自由时长没法生成。
#
# 上限由 MAX_FRAMES 推导，不能自己写死。早先这里有 8 秒和 10 秒两档，
# 而单段实际上限是 121 帧也就是 5 秒，超出的部分被静默截断，
# 成片比计划短了一大截，闸门报出来才发现。
_ALL_SLOTS = (2.0, 3.0, 4.0, 5.0, 8.0, 10.0)
DURATION_SLOTS = tuple(
    s for s in _ALL_SLOTS if s <= max_shot_duration_s()
) or (2.0,)


@dataclass
class DurationQuota:
    """时长配额。先定骨架再填内容，比让模型自己算总时长可靠得多。"""

    slots: dict[float, int]

    @property
    def total_s(self) -> float:
        return sum(d * n for d, n in self.slots.items())

    @property
    def shot_count(self) -> int:
        return sum(self.slots.values())

    def describe(self) -> str:
        parts = [f"{n} 个 {d:g} 秒镜头" for d, n in sorted(self.slots.items()) if n]
        return "，".join(parts)

    @classmethod
    def for_duration(cls, target_s: float) -> DurationQuota:
        """按目标时长分配镜头。

        节奏上短镜头占多数，长镜头留给情绪戏。全用同一时长会平铺直叙。
        """
        if target_s <= 0:
            raise ValueError("目标时长必须大于 0")
        # 分配比例。只用真正可生成的档位，长镜头多分一点时长，
        # 短镜头多分一点数量，这样节奏有变化而不是平铺。
        weights = {2.0: 0.10, 3.0: 0.22, 4.0: 0.18, 5.0: 0.50}
        plan = {d: w for d, w in weights.items() if d in DURATION_SLOTS}
        if not plan:
            plan = {DURATION_SLOTS[-1]: 1.0}
        scale = 1.0 / sum(plan.values())
        slots: dict[float, int] = {}
        for dur, share in plan.items():
            slots[dur] = max(0, round(target_s * share * scale / dur))
        # 用最长的档位补足或削减差额
        pad = DURATION_SLOTS[-1]
        slots[pad] = max(1, slots.get(pad, 0))
        diff = target_s - sum(d * n for d, n in slots.items())
        slots[pad] = max(1, slots[pad] + round(diff / pad))
        return cls({d: n for d, n in slots.items() if n > 0})


def snap_duration(seconds: float) -> float:
    """把任意时长吸附到最近的可生成档位。"""
    return min(DURATION_SLOTS, key=lambda s: abs(s - seconds))


def ceil_duration(seconds: float) -> float:
    """向上吸附。配音时长反推镜头时长时用，宁长勿短。"""
    for slot in DURATION_SLOTS:
        if slot >= seconds - 1e-6:
            return slot
    return DURATION_SLOTS[-1]


# 分镜阶段允许大模型填的字段。外观类字段不在其中，这是刻意的。
_LLM_SHOT_FIELDS = (
    "shot_id", "scene_id", "order", "visual_desc", "first_frame_prompt",
    "motion_prompt", "shot_size", "camera_angle", "camera_move", "camera_id",
    "characters", "location_id", "duration_s", "dialogue", "transition_in",
    "transition_dur_s", "subtitle_text", "beat", "continuity_notes",
    "missing_info",
)


def llm_shot_schema(assets: AssetLibrary) -> dict[str, Any]:
    """生成给大模型的 JSON Schema。

    从 Shot 模型导出，然后做三件事：删掉运行时字段、把角色和场景 id
    收紧成枚举、去掉 needs_lipsync（那个由规则算）。

    从同一份 Pydantic 定义导出而不是手写，保证 schema 和校验永远一致。
    """
    full = Shot.model_json_schema()
    props = full.get("properties", {})
    defs = full.get("$defs", {})

    kept = {k: v for k, v in props.items() if k in _LLM_SHOT_FIELDS}

    char_ids = assets.character_ids()
    loc_ids = assets.location_ids()
    if not char_ids:
        raise StoryboardError("资产库里一个角色都没有。请先生成角色圣经")

    # 角色 id 收紧成枚举。这是防止模型凭空造角色最硬的手段。
    if "CharacterInShot" in defs:
        cis = defs["CharacterInShot"]
        cis.setdefault("properties", {})["char_id"] = {
            "type": "string", "enum": char_ids,
            "description": "必须是已注册角色之一",
        }
    if "DialogueLine" in defs:
        defs["DialogueLine"].setdefault("properties", {})["char_id"] = {
            "anyOf": [{"type": "string", "enum": char_ids}, {"type": "null"}],
            "description": "说话角色。旁白留空",
        }
        # 时长由配音阶段回填，不让模型猜
        for gone in ("audio_path", "actual_duration_s", "voice_id"):
            defs["DialogueLine"].get("properties", {}).pop(gone, None)

    if loc_ids:
        kept["location_id"] = {
            "anyOf": [{"type": "string", "enum": loc_ids}, {"type": "null"}]
        }
    kept["duration_s"] = {
        "type": "number", "enum": list(DURATION_SLOTS),
        "description": "只能取这些值",
    }

    # characters 和 dialogue 必须是必填并且带说明。
    # 只给一个 $ref 而不说要填什么，模型会整个略过这两个字段，
    # 结果是分镜里一句台词都没有，配音和口型全部落空。
    kept["characters"] = {
        "type": "array",
        "items": {"$ref": "#/$defs/CharacterInShot"},
        "description": "本镜出现的角色。没有人物出镜就填空数组",
    }
    kept["dialogue"] = {
        "type": "array",
        "items": {"$ref": "#/$defs/DialogueLine"},
        "description": "本镜的台词和旁白，逐句填。剧本里的每一句话都必须落到某个镜头上，"
                       "不能丢。这一镜没有人说话就填空数组",
    }

    return {
        "type": "object",
        "properties": {
            "shots": {"type": "array", "items": {
                "type": "object",
                "properties": kept,
                "required": ["shot_id", "scene_id", "order", "first_frame_prompt",
                             "shot_size", "duration_s", "characters", "dialogue"],
                "additionalProperties": False,
            }}
        },
        "required": ["shots"],
        "additionalProperties": False,
        "$defs": defs,
    }


def build_prompt(
    script: str, assets: AssetLibrary, quota: DurationQuota, episode_id: str,
) -> str:
    """拼给大模型的提示词。

    角色和场景只给 id 和名字，不给外观描述。给了模型就会忍不住在分镜里
    复述一遍，而复述必然有偏差，那正是漂移的来源。
    """
    roster = "\n".join(
        f"  {cid}：{assets.characters[cid].name}" for cid in assets.character_ids()
    )
    places = "\n".join(
        f"  {lid}：{assets.locations[lid].name}" for lid in assets.location_ids()
    ) or "  （未定义场景，location_id 留空）"

    return f"""你是一位短剧分镜师。把下面的剧本拆成分镜表。

可用角色（只能用这些 id）：
{roster}

可用场景：
{places}

镜头配额，必须严格按这个数量和时长分配：
  {quota.describe()}
  合计 {quota.shot_count} 个镜头，总时长 {quota.total_s:g} 秒

硬性要求：

1. shot_id 用 {episode_id}_sh001 这样的格式，三位数字，按顺序递增。
2. order 从 0 开始递增。
3. duration_s 只能取配额里出现过的值，且各档位的数量必须与配额完全一致。
4. characters 必须填。凡是这一镜里出现的人，都要在这里列出 char_id，
   并填本镜的表情、动作、面部朝向。画面里没有人才填空数组。
   只填这些，绝对不要描述角色的长相、发型、发色、身材或服装样式，
   那些由系统统一管理，你写了会被丢弃并造成前后不一致。
   换装只能通过 wardrobe_state 填一个状态名。
5. first_frame_prompt 描述这一镜的画面：环境、光线、构图、角色的姿态和位置。
   同样不要描述角色长相，系统会自动拼接。
6. dialogue 必须填。剧本里的每一句台词都要落到某个镜头上，一句都不能丢。
   说话的人填 char_id，旁白留空。这一镜没人说话才填空数组。
   说话的角色也必须同时出现在 characters 里。
7. 同一场景内连续镜头尽量复用 camera_id，避免越轴。
8. transition_in 默认 cut 且 transition_dur_s 必须为 0；
   只有场景切换才用 dissolve，此时 transition_dur_s 填 0.4。
9. continuity_notes 记录需要与前后镜保持一致的细节，比如道具在哪只手。
10. 如果剧本里有信息不足以确定画面的地方，写进 missing_info，不要自己编。

剧本：

{script}

只输出 JSON，不要任何解释文字。"""


class StoryboardGenerator:
    """调大模型出分镜。"""

    def __init__(self, config: LLMConfig) -> None:
        self.config = config

    async def generate(
        self, script: str, assets: AssetLibrary, episode_id: str,
        target_duration_s: float,
    ) -> list[Shot]:
        quota = DurationQuota.for_duration(target_duration_s)
        schema = llm_shot_schema(assets)
        prompt = build_prompt(script, assets, quota, episode_id)
        raw = await self._complete(prompt, schema)
        shots = self._parse(raw, assets)

        gaps = self.check_coverage(script, shots)
        if gaps:
            raise StoryboardError(
                "分镜表不完整：\n" + "\n".join(gaps)
                + "\n\n换一个更强的模型，或者手工补齐这些字段后再跑。"
            )

        apply_lipsync_rules(shots)
        return shots

    async def _complete(self, prompt: str, schema: dict[str, Any]) -> str:
        payload = {
            "model": self.config.model,
            "messages": [{"role": "user", "content": prompt}],
            "temperature": self.config.temperature,
            "response_format": {
                "type": "json_schema",
                "json_schema": {"name": "storyboard", "strict": True, "schema": schema},
            },
        }
        headers = {"Authorization": f"Bearer {self.config.api_key}"}
        async with httpx.AsyncClient(timeout=self.config.timeout_s) as http:
            try:
                r = await http.post(
                    f"{self.config.base_url}/chat/completions",
                    json=payload, headers=headers,
                )
            except httpx.RequestError as exc:
                raise StoryboardError(
                    f"连不上大模型服务（{self.config.base_url}）。\n"
                    f"本地跑 Ollama 的话确认它已启动，"
                    f"或在配置里改 llm.base_url。\n{exc}"
                ) from exc
            if r.status_code == 404:
                raise StoryboardError(
                    f"大模型服务返回 404。检查 llm.base_url 是否带了 /v1，"
                    f"以及模型 {self.config.model} 是否已拉取"
                )
            if r.status_code >= 400:
                # 有些服务不支持 json_schema，退回普通 JSON 模式
                payload["response_format"] = {"type": "json_object"}
                r = await http.post(
                    f"{self.config.base_url}/chat/completions",
                    json=payload, headers=headers,
                )
            _raise_for_status(self.config, r, StoryboardError)
            body = r.json()
        try:
            return body["choices"][0]["message"]["content"]
        except (KeyError, IndexError) as exc:
            raise StoryboardError(f"大模型返回格式异常：{body}") from exc

    def _parse(self, raw: str, assets: AssetLibrary) -> list[Shot]:
        data = _extract_json(raw)
        items = data.get("shots") if isinstance(data, dict) else data
        if not isinstance(items, list):
            raise StoryboardError(f"大模型没有返回镜头列表，拿到的是 {type(items).__name__}")
        if not items:
            raise StoryboardError("大模型返回了空的分镜表")

        shots: list[Shot] = []
        problems: list[str] = []
        for i, item in enumerate(items):
            item.setdefault("order", i)
            item["duration_s"] = snap_duration(
                float(item.get("duration_s") or DURATION_SLOTS[-1]))
            # 模型常忘了硬切必须零时长，这里兜一下而不是报错退出
            if item.get("transition_in", "cut") == "cut":
                item["transition_dur_s"] = 0.0
            elif not item.get("transition_dur_s"):
                item["transition_dur_s"] = 0.4
            _add_missing_speakers(item, set(assets.characters))
            link_location(item, set(assets.locations))
            try:
                shots.append(Shot.model_validate(item))
            except Exception as exc:
                problems.append(f"第 {i + 1} 个镜头不合法：{exc}")

        if problems:
            raise StoryboardError(
                "分镜表有 {} 个镜头不合法：\n{}".format(
                    len(problems), "\n".join(problems[:5])
                )
            )

        ref_problems = assets.validate_references(
            {c.char_id for s in shots for c in s.characters},
            {s.location_id for s in shots if s.location_id},
        )
        if ref_problems:
            raise StoryboardError("分镜引用了未注册的资产：\n" + "\n".join(ref_problems))
        return shots

    @staticmethod
    def check_coverage(script: str, shots: list[Shot]) -> list[str]:
        """检查分镜有没有漏掉剧本里的东西。

        大模型很容易只写画面不写台词，产出一部哑剧。这类问题在生成阶段
        就能检出，不该等到配音阶段发现一句话都没有。
        """
        problems: list[str] = []
        script_has_dialogue = bool(re.search(r"[：:]\s*\S", script))
        shot_lines = sum(len(s.dialogue) for s in shots)
        if script_has_dialogue and shot_lines == 0:
            problems.append(
                "剧本里有对白，但分镜表里一句台词都没有。"
                "模型多半漏填了 dialogue 字段"
            )
        if not any(s.characters for s in shots):
            problems.append(
                "所有镜头的 characters 都是空的，没有任何角色出镜。"
                "模型多半漏填了 characters 字段"
            )
        return problems


def link_location(item: dict[str, Any], known: set[str]) -> bool:
    """location_id 空着但 scene_id 正是一个已注册场景时，把它接上。

    schema 里 scene_id 和 location_id 是两个字段，模型十次有八次把场景
    id 填进 scene_id 就完事了。后果不是报错——分镜表照样合法，是渲染时
    render.py 只在 location_id 有值时才把场景描述拼进提示词，于是空间和
    光线那一段整个丢掉，同一个房间在每个镜头里都长得不一样。

    这类静默失败最难查，所以在这里接上，而不是指望模型下次填对。
    返回是否改动过，调用方要靠它决定用不用存盘。
    """
    if item.get("location_id"):
        return False
    scene = str(item.get("scene_id") or "")
    if scene in known:
        item["location_id"] = scene
        return True
    return False


def _add_missing_speakers(item: dict[str, Any], known: set[str]) -> None:
    """把有台词但没进角色列表的说话人补进去。

    模型偶尔会给某个角色写台词却忘了把他放进 characters。这不是分镜错误，
    只是漏填：说话的人必然在场。程序补上比让整条命令挂掉合理。
    """
    dialogue = item.get("dialogue") or []
    if not dialogue:
        return
    chars = item.setdefault("characters", [])
    present = {c.get("char_id") for c in chars if isinstance(c, dict)}
    for line in dialogue:
        if not isinstance(line, dict):
            continue
        speaker = line.get("char_id")
        if speaker and speaker in known and speaker not in present:
            chars.append({"char_id": speaker})
            present.add(speaker)


def _extract_json(raw: str) -> Any:
    """从大模型输出里抠出 JSON。

    即使要求只输出 JSON，模型也常包一层代码块或加一句话。
    """
    text = raw.strip()
    fence = re.search(r"```(?:json)?\s*(.+?)```", text, re.S)
    if fence:
        text = fence.group(1).strip()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass
    # 退一步，找第一个平衡的对象或数组
    for opener, closer in (("{", "}"), ("[", "]")):
        start = text.find(opener)
        if start == -1:
            continue
        depth = 0
        for idx in range(start, len(text)):
            if text[idx] == opener:
                depth += 1
            elif text[idx] == closer:
                depth -= 1
                if depth == 0:
                    try:
                        return json.loads(text[start:idx + 1])
                    except json.JSONDecodeError:
                        break
    raise StoryboardError(f"大模型输出里找不到合法 JSON：\n{raw[:400]}")


def rebalance_durations(shots: list[Shot], target_s: float, tolerance_s: float = 3.0) -> list[Shot]:
    """把总时长拉回目标值。

    偏差优先摊到无对白的过渡镜上，有台词的镜头不动，
    因为它们的时长是由配音定的。
    """
    current = sum(s.duration_s for s in shots)
    if abs(current - target_s) <= tolerance_s:
        return shots

    adjustable = [s for s in shots if not s.dialogue and not s.duration_locked]
    if not adjustable:
        return shots

    diff = target_s - current
    step = 1 if diff > 0 else -1
    guard = 0
    while abs(diff) > tolerance_s and guard < 500:
        guard += 1
        moved = False
        for shot in adjustable:
            # 先吸附再查表。时长可能不在档位表里：老项目升级、用户手改分镜、
            # 或者档位表本身变过。直接 index 会抛异常把整条流水线带崩。
            current_slot = snap_duration(shot.duration_s)
            idx = DURATION_SLOTS.index(current_slot)
            new_idx = idx + step
            if not (0 <= new_idx < len(DURATION_SLOTS)):
                continue
            delta = DURATION_SLOTS[new_idx] - shot.duration_s
            if abs(diff - delta) < abs(diff):
                shot.duration_s = DURATION_SLOTS[new_idx]
                diff -= delta
                moved = True
                if abs(diff) <= tolerance_s:
                    break
        if not moved:
            break
    return shots
