# -*- coding: utf-8 -*-
"""分镜生成。一致性靠 schema 的结构约束，不靠提示词祈使。"""
import pytest

from changji.models.character import (
    AppearanceBlock, AssetLibrary, Character, Location, StyleLine,
)
from changji.models.shot import CharacterInShot, DialogueLine, Shot
from changji.stages.storyboard import (
    DURATION_SLOTS, DurationQuota, StoryboardError, _extract_json,
    build_prompt, ceil_duration, llm_shot_schema, rebalance_durations,
    snap_duration,
)


def _assets():
    return AssetLibrary(
        characters={
            "c_lin": Character(
                char_id="c_lin", name="林晚",
                appearance=AppearanceBlock(
                    identity="二十八岁女性，冷峭",
                    face="黑色长发挽起，眼型细长",
                    attire="灰色西装套裙"),
            ),
            "c_wang": Character(
                char_id="c_wang", name="王铎",
                appearance=AppearanceBlock(
                    identity="四十岁男性", face="寸头，方脸", attire="深蓝衬衫"),
            ),
        },
        locations={"loc_office": Location(
            location_id="loc_office", name="总裁办公室",
            space="落地窗环绕的高层办公室", lighting="夜间冷调顶光")},
    )


class TestSchemaGuards:
    """给大模型的 schema 必须堵死外观字段。这是一致性的结构保证。"""

    def test_schema_里没有任何外观字段(self):
        schema = llm_shot_schema(_assets())
        blob = str(schema)
        cis = schema["$defs"]["CharacterInShot"]["properties"]
        assert set(cis) == {"char_id", "expression", "action",
                            "wardrobe_state", "face_pose", "screen_pos"}
        for banned in ("appearance", "hair", "hairstyle", "outfit", "身高", "发色"):
            assert banned not in blob

    def test_角色id被收紧成枚举(self):
        schema = llm_shot_schema(_assets())
        enum = schema["$defs"]["CharacterInShot"]["properties"]["char_id"]["enum"]
        assert enum == ["c_lin", "c_wang"]

    def test_场景id被收紧成枚举(self):
        schema = llm_shot_schema(_assets())
        assert schema["properties"]["shots"]["items"]["properties"]["location_id"][
            "anyOf"][0]["enum"] == ["loc_office"]

    def test_时长被限定为可生成档位(self):
        schema = llm_shot_schema(_assets())
        assert schema["properties"]["shots"]["items"]["properties"][
            "duration_s"]["enum"] == list(DURATION_SLOTS)

    def test_运行时字段不给模型填(self):
        props = llm_shot_schema(_assets())["properties"]["shots"]["items"]["properties"]
        for runtime_only in ("status", "attempts", "frame_path", "video_path",
                             "needs_lipsync", "gate_notes"):
            assert runtime_only not in props

    def test_台词时长字段不给模型猜(self):
        dl = llm_shot_schema(_assets())["$defs"]["DialogueLine"]["properties"]
        assert "actual_duration_s" not in dl
        assert "audio_path" not in dl

    def test_没有角色时明确报错(self):
        with pytest.raises(StoryboardError, match="一个角色都没有"):
            llm_shot_schema(AssetLibrary())


class TestPrompt:
    def test_提示词只给id和名字不给外观(self):
        """给了外观描述，模型会在分镜里复述，复述必然有偏差。"""
        p = build_prompt("剧本", _assets(), DurationQuota.for_duration(60), "ep01")
        assert "c_lin" in p and "林晚" in p
        assert "黑色长发挽起" not in p
        assert "灰色西装套裙" not in p

    def test_提示词明确禁止描述外观(self):
        p = build_prompt("剧本", _assets(), DurationQuota.for_duration(60), "ep01")
        assert "不要描述角色的长相" in p

    def test_提示词带配额(self):
        q = DurationQuota.for_duration(60)
        assert q.describe() in build_prompt("剧本", _assets(), q, "ep01")


class TestDurationQuota:
    @pytest.mark.parametrize("target", [30, 60, 120, 180, 300])
    def test_配额总时长接近目标(self, target):
        q = DurationQuota.for_duration(target)
        assert abs(q.total_s - target) / target < 0.2

    def test_配额有节奏变化不平铺(self):
        q = DurationQuota.for_duration(180)
        assert len([n for n in q.slots.values() if n > 0]) >= 3

    def test_镜头数为正(self):
        assert DurationQuota.for_duration(30).shot_count > 0

    def test_非法目标被拒(self):
        with pytest.raises(ValueError):
            DurationQuota.for_duration(0)


class TestDurationSnap:
    def test_吸附到最近档位(self):
        assert snap_duration(4.6) == 5.0
        assert snap_duration(2.2) == 2.0
        assert snap_duration(100) == max(DURATION_SLOTS)

    def test_向上吸附宁长勿短(self):
        """配音 3.2 秒的镜头必须给 4 秒，给 3 秒会截断台词。"""
        assert ceil_duration(3.2) == 4.0
        assert ceil_duration(3.0) == 3.0
        assert ceil_duration(0.4) == 2.0

    def test_超过上限时给最长档位(self):
        """单段生成不了更长的，只能给最长的并在装配时处理。"""
        assert ceil_duration(99.0) == max(DURATION_SLOTS)


class TestRebalance:
    def test_把总时长拉回目标(self):
        shots = [Shot(shot_id=f"ep01_sh{i:03d}", scene_id="s1", order=i, duration_s=5.0)
                 for i in range(10)]
        rebalance_durations(shots, target_s=25.0)
        assert abs(sum(s.duration_s for s in shots) - 25.0) <= 3.0

    def test_有台词的镜头不动(self):
        """它们的时长由配音定，改了就音画不齐。"""
        from changji.models.shot import CharacterInShot, DialogueLine
        locked = Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=5.0,
                      characters=[CharacterInShot(char_id="c_lin")],
                      dialogue=[DialogueLine(char_id="c_lin", text="你说什么")])
        free = [Shot(shot_id=f"ep01_sh{i:03d}", scene_id="s1", order=i, duration_s=5.0)
                for i in range(1, 8)]
        rebalance_durations([locked] + free, target_s=18.0)
        assert locked.duration_s == 5.0

    def test_全部锁定时不硬改(self):
        from changji.models.shot import CharacterInShot, DialogueLine
        shots = [Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=5.0,
                      characters=[CharacterInShot(char_id="c_lin")],
                      dialogue=[DialogueLine(char_id="c_lin", text="话")])]
        rebalance_durations(shots, target_s=3.0)
        assert shots[0].duration_s == 5.0

    def test_已达标就不动(self):
        shots = [Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=5.0)]
        rebalance_durations(shots, target_s=5.0)
        assert shots[0].duration_s == 5.0


class TestJsonExtraction:
    def test_纯json(self):
        assert _extract_json('{"shots": []}') == {"shots": []}

    def test_代码块包裹(self):
        assert _extract_json('```json\n{"shots": []}\n```') == {"shots": []}

    def test_前后有废话(self):
        raw = '好的，这是分镜表：\n{"shots": [1]}\n希望有帮助。'
        assert _extract_json(raw) == {"shots": [1]}

    def test_嵌套括号不会截断(self):
        raw = '说明\n{"shots": [{"a": {"b": 1}}]}\n完'
        assert _extract_json(raw)["shots"][0]["a"]["b"] == 1

    def test_完全没有json时报错带原文(self):
        with pytest.raises(StoryboardError, match="找不到合法 JSON"):
            _extract_json("我不知道怎么做")


class TestCoverageGuard:
    """回归测试。

    第一版把 characters 和 dialogue 设成可选且只给一个 $ref 不加说明，
    真实模型直接把两个字段整个略过，产出一部没有台词也没有角色的哑剧。
    schema 必填加说明、提示词点名、外加一道覆盖度校验，三重保证。
    """

    def test_台词和角色是必填(self):
        item = llm_shot_schema(_assets())["properties"]["shots"]["items"]
        assert "dialogue" in item["required"]
        assert "characters" in item["required"]

    def test_两个字段都带说明(self):
        props = llm_shot_schema(_assets())["properties"]["shots"]["items"]["properties"]
        assert "不能丢" in props["dialogue"]["description"]
        assert props["characters"]["description"]

    def test_提示词点名要求填台词(self):
        p = build_prompt("剧本", _assets(), DurationQuota.for_duration(60), "ep01")
        assert "一句都不能丢" in p
        assert "characters 必须填" in p

    def test_检出漏掉的台词(self):
        from changji.stages.storyboard import StoryboardGenerator
        script = "王铎：你想清楚了？\n林晚：我该走了。"
        shots = [Shot(shot_id="ep01_sh001", scene_id="s1", order=0,
                      characters=[CharacterInShot(char_id="c_lin")])]
        problems = StoryboardGenerator.check_coverage(script, shots)
        assert any("一句台词都没有" in p for p in problems)

    def test_检出没有任何角色出镜(self):
        from changji.stages.storyboard import StoryboardGenerator
        shots = [Shot(shot_id="ep01_sh001", scene_id="s1", order=0)]
        problems = StoryboardGenerator.check_coverage("旁白剧本无对白", shots)
        assert any("没有任何角色出镜" in p for p in problems)

    def test_正常分镜不报问题(self):
        from changji.stages.storyboard import StoryboardGenerator
        script = "林晚：我该走了。"
        shots = [Shot(
            shot_id="ep01_sh001", scene_id="s1", order=0,
            characters=[CharacterInShot(char_id="c_lin")],
            dialogue=[DialogueLine(char_id="c_lin", text="我该走了。")])]
        assert StoryboardGenerator.check_coverage(script, shots) == []

    def test_纯旁白剧本不误报(self):
        from changji.stages.storyboard import StoryboardGenerator
        shots = [Shot(shot_id="ep01_sh001", scene_id="s1", order=0,
                      characters=[CharacterInShot(char_id="c_lin")],
                      dialogue=[DialogueLine(text="三年后。")])]
        assert StoryboardGenerator.check_coverage("画面描述，没有冒号对白", shots) == []


class TestDurationFeasibility:
    """回归测试。

    档位表里曾有 8 秒和 10 秒两档，而单段实际上限是 121 帧也就是 5 秒，
    超出部分被静默截断，成片比计划短了 12 秒才被闸门发现。
    档位必须由帧数上限推导，不能两处各写一份。
    """

    def test_每个档位都不会被截断(self):
        from changji.stages.render import frames_for
        for slot in DURATION_SLOTS:
            actual = frames_for(slot) / 24.0
            assert actual >= slot - 0.05, (
                f"{slot} 秒的档位实际只能生成 {actual:.2f} 秒，会被静默截断"
            )

    def test_档位不超过单段上限(self):
        from changji.stages.render import max_shot_duration_s
        assert max(DURATION_SLOTS) <= max_shot_duration_s()

    def test_配额只用可行的档位(self):
        for target in (10, 30, 60, 180, 300):
            q = DurationQuota.for_duration(target)
            for slot in q.slots:
                assert slot in DURATION_SLOTS

    def test_配额总时长仍接近目标(self):
        for target in (30, 60, 180, 300):
            q = DurationQuota.for_duration(target)
            assert abs(q.total_s - target) / target < 0.1

    def test_配额仍有节奏变化(self):
        q = DurationQuota.for_duration(180)
        assert len([n for n in q.slots.values() if n > 0]) >= 3

    def test_向上吸附不会越过上限(self):
        assert ceil_duration(100.0) == max(DURATION_SLOTS)

    def test_时长不在档位表里也不崩(self):
        """回归测试。

        老项目升级、用户手改分镜、或档位表本身变过，都会让镜头时长
        落在档位表之外。直接 index 会抛异常把整条流水线带崩。
        """
        shots = [Shot(shot_id=f"ep01_sh{i:03d}", scene_id="s1", order=i,
                      duration_s=7.5)  # 不在档位表里
                 for i in range(4)]
        rebalance_durations(shots, target_s=12.0)
        for s in shots:
            assert s.duration_s in DURATION_SLOTS


class TestSpeakerAutoFix:
    """模型偶尔给角色写了台词却忘了把他放进角色列表。

    这不是分镜错误只是漏填，说话的人必然在场。程序补上比让整条命令挂掉合理。
    """

    def _gen(self):
        from changji.config import LLMConfig
        from changji.stages.storyboard import StoryboardGenerator
        return StoryboardGenerator(LLMConfig())

    def test_补全漏填的说话人(self):
        raw = """{"shots":[{"shot_id":"ep01_sh001","scene_id":"s1","order":0,
        "first_frame_prompt":"画面","shot_size":"MS","duration_s":3.0,
        "characters":[],
        "dialogue":[{"char_id":"c_wang","text":"你想清楚了"}]}]}"""
        shots = self._gen()._parse(raw, _assets())
        assert [c.char_id for c in shots[0].characters] == ["c_wang"]

    def test_已在场的不重复添加(self):
        raw = """{"shots":[{"shot_id":"ep01_sh001","scene_id":"s1","order":0,
        "first_frame_prompt":"画面","shot_size":"MS","duration_s":3.0,
        "characters":[{"char_id":"c_lin","expression":"隐忍"}],
        "dialogue":[{"char_id":"c_lin","text":"我该走了"}]}]}"""
        shots = self._gen()._parse(raw, _assets())
        assert len(shots[0].characters) == 1
        assert shots[0].characters[0].expression == "隐忍"

    def test_旁白不会被补成角色(self):
        raw = """{"shots":[{"shot_id":"ep01_sh001","scene_id":"s1","order":0,
        "first_frame_prompt":"画面","shot_size":"LS","duration_s":3.0,
        "characters":[],"dialogue":[{"char_id":null,"text":"三年后"}]}]}"""
        shots = self._gen()._parse(raw, _assets())
        assert shots[0].characters == []

    def test_未注册的角色不会被补进去(self):
        """补全只处理已注册角色。凭空冒出的 id 仍然要被资产库校验拦下。"""
        raw = """{"shots":[{"shot_id":"ep01_sh001","scene_id":"s1","order":0,
        "first_frame_prompt":"画面","shot_size":"MS","duration_s":3.0,
        "characters":[],"dialogue":[{"char_id":"c_ghost","text":"我是谁"}]}]}"""
        with pytest.raises(StoryboardError):
            self._gen()._parse(raw, _assets())
