# -*- coding: utf-8 -*-
"""项目存储。可移植性要求项目目录能整个拷到别的机器。"""
import json

import pytest

from changji.models.character import AppearanceBlock, Character, Location, StyleLine
from changji.models.project import Episode, Project, ProjectStore
from changji.models.shot import Shot, ShotStatus


def _store(tmp_path):
    return ProjectStore.create(tmp_path / "剧", "my_drama", "我的短剧")


class TestProjectStore:
    def test_创建目录结构(self, tmp_path):
        s = _store(tmp_path)
        for sub in ("refs", "audio", "frames", "shots/draft", "shots/final", "output"):
            assert (s.root / sub).is_dir()
        assert s.exists()

    def test_重复创建会拒绝(self, tmp_path):
        _store(tmp_path)
        with pytest.raises(FileExistsError):
            ProjectStore.create(tmp_path / "剧", "my_drama")

    def test_存回读一致(self, tmp_path):
        s = _store(tmp_path)
        p = s.load_project()
        p.episodes.append(Episode(episode_id="ep01", title="第一集"))
        s.save_project(p)
        assert s.load_project().episodes[0].title == "第一集"

    def test_打开非项目目录报错说清楚(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="changji new"):
            ProjectStore(tmp_path / "空").load_project()

    def test_未来版本的项目会被拒绝而不是读坏(self, tmp_path):
        s = _store(tmp_path)
        raw = json.loads(s.paths.project_file.read_text(encoding="utf-8"))
        raw["schema_version"] = 999
        s.paths.project_file.write_text(json.dumps(raw), encoding="utf-8")
        with pytest.raises(ValueError, match="更新版本"):
            s.load_project()

    def test_文件损坏时报出是哪个文件(self, tmp_path):
        s = _store(tmp_path)
        s.paths.project_file.write_text("{ 坏掉的", encoding="utf-8")
        with pytest.raises(ValueError, match="不是合法 JSON"):
            s.load_project()

    def test_原子写不留半个文件(self, tmp_path):
        s = _store(tmp_path)
        assert not list(s.root.glob("*.tmp"))


class TestPortablePaths:
    def test_项目内路径存成相对且用正斜杠(self, tmp_path):
        s = _store(tmp_path)
        f = s.paths.frames / "ep01_sh001.png"
        f.write_bytes(b"x")
        assert s.paths.rel(f) == "frames/ep01_sh001.png"

    def test_项目外路径被拒绝(self, tmp_path):
        """存绝对路径会让项目拷到别的机器就断链。"""
        s = _store(tmp_path)
        outside = tmp_path / "别处.png"
        outside.write_bytes(b"x")
        with pytest.raises(ValueError, match="可移植性"):
            s.paths.rel(outside)

    def test_外部文件复制进项目后可用(self, tmp_path):
        s = _store(tmp_path)
        outside = tmp_path / "定妆照.png"
        outside.write_bytes(b"x")
        rel = s.copy_into(outside, "refs")
        assert rel == "refs/定妆照.png"
        assert s.paths.abs(rel).is_file()

    def test_相对路径能还原(self, tmp_path):
        s = _store(tmp_path)
        assert s.paths.abs("frames/a.png") == s.root / "frames" / "a.png"

    def test_项目整体可搬迁(self, tmp_path):
        """核心可移植性测试：拷到新位置后一切照常。"""
        import shutil
        s = _store(tmp_path)
        p = s.load_project()
        p.episodes.append(Episode(episode_id="ep01", shots=[
            Shot(shot_id="ep01_sh001", scene_id="ep01_s01", order=0,
                 frame_path="frames/ep01_sh001.png")]))
        s.save_project(p)
        (s.paths.frames / "ep01_sh001.png").write_bytes(b"x")

        moved = tmp_path / "搬到这里"
        shutil.copytree(s.root, moved)
        s2 = ProjectStore(moved)
        shot = s2.load_project().episodes[0].shots[0]
        assert s2.paths.abs(shot.frame_path).is_file()


class TestEpisode:
    def _ep(self):
        return Episode(episode_id="ep01", shots=[
            Shot(shot_id="ep01_sh002", scene_id="s1", order=1, duration_s=3.0),
            Shot(shot_id="ep01_sh001", scene_id="s1", order=0, duration_s=5.0,
                 status=ShotStatus.DRAFT_DONE),
        ])

    def test_按顺序排列(self):
        assert [s.order for s in self._ep().sorted_shots()] == [0, 1]

    def test_按状态取镜头支撑断点续跑(self):
        ep = self._ep()
        assert len(ep.shots_needing(ShotStatus.PLANNED)) == 1
        assert len(ep.shots_needing(ShotStatus.DRAFT_DONE)) == 1

    def test_统计各状态数量(self):
        assert self._ep().counts_by_status() == {"planned": 1, "draft_done": 1}

    def test_累计时长(self):
        assert self._ep().planned_duration_s() == 8.0
