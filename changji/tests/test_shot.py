# -*- coding: utf-8 -*-
import pytest
from pydantic import ValidationError

from changji.models.shot import (
    CameraAngle, CharacterInShot, DialogueLine, FacePose, Shot, ShotSize,
    Transition, apply_lipsync_rules, derive_needs_lipsync,
)


def _shot(**kw):
    base = dict(shot_id="ep01_s03_sh007", scene_id="ep01_s03", order=7)
    base.update(kw)
    return Shot(**base)


class TestSchemaGuards:
    def test_角色外观字段被结构禁止(self):
        """大模型不能在分镜里写角色长相。这是一致性的结构保证。"""
        with pytest.raises(ValidationError):
            CharacterInShot(char_id="c_lin", hair="黑色长发")
        with pytest.raises(ValidationError):
            _shot(appearance="穿西装的男人")

    def test_台词说话人必须在场(self):
        with pytest.raises(ValidationError, match="不在本镜角色列表"):
            _shot(
                characters=[CharacterInShot(char_id="c_lin")],
                dialogue=[DialogueLine(char_id="c_wang", text="你说什么？")],
            )

    def test_旁白不需要在场角色(self):
        s = _shot(dialogue=[DialogueLine(text="三年后。")])
        assert s.has_onscreen_dialogue is False

    def test_硬切不能有转场时长(self):
        with pytest.raises(ValidationError, match="硬切"):
            _shot(transition_in=Transition.CUT, transition_dur_s=0.4)

    def test_溶解必须有转场时长(self):
        with pytest.raises(ValidationError, match="大于 0"):
            _shot(transition_in=Transition.DISSOLVE, transition_dur_s=0)

    def test_shot_id_必须规范(self):
        with pytest.raises(ValidationError):
            _shot(shot_id="Ep01 Shot 7")


class TestLipsyncRules:
    """口型判定必须是规则，不能交给大模型。"""

    def _talking(self, **kw):
        opts = dict(
            shot_size=ShotSize.MCU,
            camera_angle=CameraAngle.EYE_LEVEL,
            characters=[CharacterInShot(char_id="c_lin", face_pose=FacePose.FRONT)],
            dialogue=[DialogueLine(char_id="c_lin", text="你说什么？")],
        )
        opts.update(kw)
        return _shot(**opts)

    def test_近景正脸有台词要做口型(self):
        assert derive_needs_lipsync(self._talking()) is True

    def test_远景不做(self):
        assert derive_needs_lipsync(self._talking(shot_size=ShotSize.LS)) is False

    def test_顶拍不做(self):
        assert derive_needs_lipsync(
            self._talking(camera_angle=CameraAngle.OVERHEAD)) is False

    def test_背对镜头不做(self):
        assert derive_needs_lipsync(self._talking(
            characters=[CharacterInShot(char_id="c_lin", face_pose=FacePose.BACK)])) is False

    def test_旁白不做(self):
        assert derive_needs_lipsync(_shot(
            shot_size=ShotSize.CU, dialogue=[DialogueLine(text="三年后。")])) is False

    def test_说话人背对但另一人正脸也不做(self):
        """在场的另一个角色正脸不算数，要看说话的那个。"""
        s = self._talking(characters=[
            CharacterInShot(char_id="c_lin", face_pose=FacePose.BACK),
            CharacterInShot(char_id="c_wang", face_pose=FacePose.FRONT),
        ])
        assert derive_needs_lipsync(s) is False

    def test_批量回填(self):
        shots = [self._talking(), _shot(shot_id="ep01_s03_sh008", scene_id="ep01_s03", order=8)]
        apply_lipsync_rules(shots)
        assert [s.needs_lipsync for s in shots] == [True, False]


class TestDuration:
    def test_未配音时总时长为空(self):
        s = _shot(
            characters=[CharacterInShot(char_id="c_lin")],
            dialogue=[DialogueLine(char_id="c_lin", text="你说什么？")],
        )
        assert s.total_dialogue_duration_s is None

    def test_配音后累加(self):
        s = _shot(
            characters=[CharacterInShot(char_id="c_lin")],
            dialogue=[
                DialogueLine(char_id="c_lin", text="你说什么？", actual_duration_s=1.24),
                DialogueLine(char_id="c_lin", text="再说一遍。", actual_duration_s=1.10),
            ],
        )
        assert s.total_dialogue_duration_s == pytest.approx(2.34)

    def test_无台词镜头时长为零(self):
        assert _shot().total_dialogue_duration_s == 0.0


def test_可导出_json_schema_供约束解码():
    """同一份定义既做校验又约束大模型输出，这是不写重复代码的关键。"""
    schema = Shot.model_json_schema()
    assert schema["additionalProperties"] is False
    props = set(schema["properties"])
    # 外观类字段绝不能出现
    assert not props & {"appearance", "hair", "outfit", "face", "attire"}
    assert {"shot_id", "characters", "duration_s", "shot_size"} <= props
