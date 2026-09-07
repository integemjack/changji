"""项目模型与磁盘布局。

一个项目就是一个自包含的目录，换机器整个拷走即可。目录里只有相对路径，
不含任何绝对路径，也不含模型文件（那些跟机器走，不跟项目走）。

    我的短剧/
    ├── project.json          项目元数据与分镜表
    ├── assets.json           角色与场景资产库
    ├── changji.toml          项目级配置覆盖（可选）
    ├── refs/                 角色三视图、场景空景图
    ├── audio/                配音
    ├── frames/               逐镜首帧
    ├── shots/
    │   ├── draft/            草稿档视频
    │   └── final/            成片档视频
    ├── subtitles/
    └── output/               成片
"""

from __future__ import annotations

import json
import shutil
import tempfile
from datetime import datetime, timezone
from pathlib import Path

from pydantic import BaseModel, Field

from .character import AssetLibrary, StyleLine
from .shot import Shot, ShotStatus

PROJECT_FILE = "project.json"
ASSETS_FILE = "assets.json"
SCHEMA_VERSION = 1

SUBDIRS = (
    "refs", "audio", "frames", "shots/draft", "shots/final",
    "subtitles", "output", "logs",
)


class Episode(BaseModel):
    """一集。分镜表挂在这里。"""

    model_config = {"extra": "forbid"}

    episode_id: str = Field(pattern=r"^[a-z0-9_]+$")
    title: str = ""
    synopsis: str = ""
    target_duration_s: float = Field(default=180.0, gt=0, description="目标时长")
    script: str = Field(default="", description="剧本原文")
    shots: list[Shot] = Field(default_factory=list)

    def sorted_shots(self) -> list[Shot]:
        return sorted(self.shots, key=lambda s: s.order)

    def shot_by_id(self, shot_id: str) -> Shot | None:
        return next((s for s in self.shots if s.shot_id == shot_id), None)

    def planned_duration_s(self) -> float:
        return sum(s.duration_s for s in self.shots)

    def counts_by_status(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for s in self.shots:
            out[s.status.value] = out.get(s.status.value, 0) + 1
        return out

    def shots_needing(self, status: ShotStatus) -> list[Shot]:
        """取处于某个阶段的镜头。断点续跑靠它。"""
        return [s for s in self.sorted_shots() if s.status == status]


class Project(BaseModel):
    """一个项目。"""

    model_config = {"extra": "forbid"}

    schema_version: int = SCHEMA_VERSION
    project_id: str = Field(pattern=r"^[a-z0-9_-]+$")
    title: str = ""
    style_line: StyleLine = StyleLine.REALISTIC
    # 这部剧讲什么。写下一集时当提示词用。
    # 不存的话，隔天想接着写第六集，得凭记忆把当初那句话重打一遍。
    premise: str = Field(default="", max_length=2000)
    created_at: str = Field(default_factory=lambda: datetime.now(timezone.utc).isoformat())
    updated_at: str = Field(default_factory=lambda: datetime.now(timezone.utc).isoformat())
    episodes: list[Episode] = Field(default_factory=list)

    def episode_by_id(self, episode_id: str) -> Episode | None:
        return next((e for e in self.episodes if e.episode_id == episode_id), None)

    def touch(self) -> None:
        self.updated_at = datetime.now(timezone.utc).isoformat()


class ProjectPaths:
    """项目目录布局。所有路径都由项目根推导，绝不写死。"""

    def __init__(self, root: str | Path) -> None:
        self.root = Path(root).expanduser().resolve()

    def ensure(self) -> ProjectPaths:
        for sub in SUBDIRS:
            (self.root / sub).mkdir(parents=True, exist_ok=True)
        return self

    @property
    def project_file(self) -> Path:
        return self.root / PROJECT_FILE

    @property
    def assets_file(self) -> Path:
        return self.root / ASSETS_FILE

    @property
    def refs(self) -> Path:
        return self.root / "refs"

    @property
    def audio(self) -> Path:
        return self.root / "audio"

    @property
    def frames(self) -> Path:
        return self.root / "frames"

    @property
    def subtitles(self) -> Path:
        return self.root / "subtitles"

    @property
    def output(self) -> Path:
        return self.root / "output"

    @property
    def logs(self) -> Path:
        return self.root / "logs"

    def shots(self, tier: str) -> Path:
        return self.root / "shots" / tier

    def rel(self, path: str | Path) -> str:
        """绝对路径转成相对项目根的路径。存进 JSON 的一律用这个。

        用正斜杠，保证在 Windows 上存的项目拿到 Linux 上也能读。
        """
        p = Path(path).expanduser().resolve()
        try:
            return p.relative_to(self.root).as_posix()
        except ValueError:
            # 不在项目内的文件只能存绝对路径，但这会破坏可移植性
            raise ValueError(
                f"路径不在项目目录内，存进项目会破坏可移植性：{p}\n"
                f"请先把文件复制进 {self.root}"
            ) from None

    def abs(self, rel_path: str) -> Path:
        """相对路径还原成绝对路径。"""
        return (self.root / rel_path).resolve()


class ProjectStore:
    """项目的读写。写入用原子替换，避免中途断电留下半个文件。"""

    def __init__(self, root: str | Path) -> None:
        self.paths = ProjectPaths(root)

    @property
    def root(self) -> Path:
        return self.paths.root

    def exists(self) -> bool:
        return self.paths.project_file.is_file()

    # ---- 创建 ----

    @classmethod
    def create(
        cls,
        root: str | Path,
        project_id: str,
        title: str = "",
        style_line: StyleLine = StyleLine.REALISTIC,
    ) -> ProjectStore:
        store = cls(root)
        if store.exists():
            raise FileExistsError(f"这个目录已经是一个项目了：{store.root}")
        store.paths.ensure()
        project = Project(project_id=project_id, title=title or project_id,
                          style_line=style_line)
        store.save_project(project)
        store.save_assets(AssetLibrary(style={"style_line": style_line}))
        return store

    # ---- 读 ----

    def load_project(self) -> Project:
        if not self.exists():
            raise FileNotFoundError(
                f"这里不是一个项目目录：{self.root}\n"
                f"用 changji new 创建，或者 cd 到正确的目录"
            )
        raw = self._read_json(self.paths.project_file)
        version = raw.get("schema_version", 0)
        if version > SCHEMA_VERSION:
            raise ValueError(
                f"项目是用更新版本的场记创建的（格式版本 {version}，"
                f"本机支持到 {SCHEMA_VERSION}）。请升级后再打开"
            )
        return Project.model_validate(raw)

    def load_assets(self) -> AssetLibrary:
        if not self.paths.assets_file.is_file():
            return AssetLibrary()
        return AssetLibrary.model_validate(self._read_json(self.paths.assets_file))

    # ---- 写 ----

    def save_project(self, project: Project) -> None:
        project.touch()
        self._write_json(self.paths.project_file, project.model_dump(mode="json"))

    def save_assets(self, assets: AssetLibrary) -> None:
        self._write_json(self.paths.assets_file, assets.model_dump(mode="json"))

    # ---- 内部 ----

    @staticmethod
    def _read_json(path: Path) -> dict:
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            raise ValueError(f"文件损坏，不是合法 JSON：{path}\n{exc}") from exc

    @staticmethod
    def _write_json(path: Path, data: dict) -> None:
        """原子写。先写临时文件再替换，中途断电不会留下半个文件。"""
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=str(path.parent), suffix=".tmp")
        try:
            with open(fd, "w", encoding="utf-8") as fh:
                json.dump(data, fh, ensure_ascii=False, indent=2)
                fh.flush()
            Path(tmp).replace(path)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise

    # ---- 迁移 ----

    def copy_into(self, src: str | Path, subdir: str, name: str | None = None) -> str:
        """把外部文件复制进项目，返回相对路径。

        参考图这类素材必须复制进来而不是引用原位置，否则项目拷到别的机器就断链。
        """
        source = Path(src).expanduser().resolve()
        if not source.is_file():
            raise FileNotFoundError(f"文件不存在：{source}")
        target_dir = self.root / subdir
        target_dir.mkdir(parents=True, exist_ok=True)
        target = target_dir / (name or source.name)
        if source != target:
            shutil.copy2(source, target)
        return self.paths.rel(target)
