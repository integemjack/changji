"""命令行入口。"""

from __future__ import annotations

import asyncio
from pathlib import Path

import typer
from rich.console import Console

from .config import Settings, load_settings, user_config_path, write_default_config
from .doctor import run_checks
from .hardware import HardwareProfile, Tier
from .models.character import StyleLine
from .models.project import ProjectStore

app = typer.Typer(
    name="changji",
    help="场记：AI 短剧生产流水线",
    no_args_is_help=True,
    add_completion=False,
)
console = Console()


def _settings(project: Path | None = None) -> Settings:
    try:
        return load_settings(project)
    except ValueError as exc:
        console.print(f"[red]配置有问题：[/red]{exc}")
        raise typer.Exit(1) from exc


@app.command()
def init() -> None:
    """生成配置模板。"""
    path = user_config_path()
    if path.is_file():
        console.print(f"配置已存在：[cyan]{path}[/cyan]")
        console.print("要重新生成请先删除或改名。")
        return
    written = write_default_config()
    console.print(f"已生成配置：[cyan]{written}[/cyan]")
    console.print("接下来跑 [bold]changji doctor[/bold] 体检。")


@app.command()
def doctor(
    project: Path = typer.Option(None, "--project", "-p", help="项目目录"),
) -> None:
    """检查环境和外部服务。跑之前先跑这个。"""
    settings = _settings(project)
    report = asyncio.run(run_checks(settings))
    console.print()
    console.print(report.render())
    if not report.can_run:
        raise typer.Exit(1)


@app.command()
def new(
    path: Path = typer.Argument(..., help="项目目录，不存在会创建"),
    title: str = typer.Option("", "--title", "-t", help="项目名"),
    style: str = typer.Option("realistic", "--style", "-s",
                              help="风格线：realistic 或 anime"),
) -> None:
    """新建一个项目。"""
    try:
        line = StyleLine(style)
    except ValueError:
        console.print(f"[red]风格线只能是 realistic 或 anime，收到 {style}[/red]")
        raise typer.Exit(1) from None

    project_id = _slugify(path.name)
    try:
        store = ProjectStore.create(path, project_id, title or path.name, line)
    except FileExistsError as exc:
        console.print(f"[red]{exc}[/red]")
        raise typer.Exit(1) from exc

    console.print(f"已创建项目：[cyan]{store.root}[/cyan]")
    console.print(f"风格线：{line.value}")
    console.print()
    console.print("接下来：")
    console.print("  1. 在项目里定义角色和场景")
    console.print("  2. 写剧本，然后生成分镜")
    console.print(f"  3. [bold]changji serve --project {store.root}[/bold] 打开界面")


@app.command()
def info(
    project: Path = typer.Argument(Path("."), help="项目目录"),
) -> None:
    """看项目状态。"""
    store = ProjectStore(project)
    try:
        p = store.load_project()
        assets = store.load_assets()
    except (FileNotFoundError, ValueError) as exc:
        console.print(f"[red]{exc}[/red]")
        raise typer.Exit(1) from exc

    console.print(f"[bold]{p.title}[/bold]  ({p.project_id})")
    console.print(f"风格线 {p.style_line.value}    更新于 {p.updated_at[:19]}")
    console.print(f"角色 {len(assets.characters)} 个，场景 {len(assets.locations)} 个")
    console.print()
    if not p.episodes:
        console.print("还没有剧集。")
        return
    for ep in p.episodes:
        counts = ep.counts_by_status()
        done = counts.get("final_done", 0)
        console.print(
            f"  {ep.episode_id}  {ep.title or '(无标题)'}  "
            f"{len(ep.shots)} 镜  已完成 {done}  "
            f"计划时长 {ep.planned_duration_s():.0f} 秒"
        )
        if counts:
            detail = "  ".join(f"{k}={v}" for k, v in sorted(counts.items()))
            console.print(f"      {detail}")


@app.command()
def tiers(
    project: Path = typer.Option(None, "--project", "-p"),
    shots: int = typer.Option(36, "--shots", help="按多少个镜头估算一集"),
) -> None:
    """看本机的画质档位和产能估算。"""
    settings = _settings(project)
    profile = HardwareProfile.detect(settings.vram_gb_override)
    console.print()
    console.print(profile.describe())
    console.print()
    console.print(f"按 {shots} 个镜头一集估算：")
    for tier in (Tier.DRAFT, Tier.PREVIEW, Tier.FINAL):
        total = profile.estimate_episode(shots, tier)
        if total is None:
            continue
        console.print(f"  {tier.value:8s} {total / 60:6.1f} 分钟")
    console.print()
    console.print("草稿档用来验证叙事和构图，只有过闸门的镜头才升级成片档。")


@app.command()
def make(
    script: Path = typer.Argument(..., help="剧本文件，纯文本"),
    project: Path = typer.Option(None, "--project", "-p",
                                 help="项目目录，不给就在剧本旁边新建"),
    episode: str = typer.Option("ep01", "--episode", "-e", help="剧集 id"),
    duration: float = typer.Option(60.0, "--duration", "-d", help="目标时长，秒"),
    style: str = typer.Option("realistic", "--style", "-s",
                              help="风格线：realistic 或 anime"),
    draft_only: bool = typer.Option(False, "--draft-only",
                                    help="只跑草稿档，用来快速验证叙事"),
    plan_only: bool = typer.Option(False, "--plan-only",
                                   help="只出角色设定和分镜表，不渲染"),
) -> None:
    """从一段剧本一直做到成片。

    整条链路：角色设定、分镜表、配音、首帧、渲染、闸门、装配。
    中途可以随时中断，再跑一次会从上次的位置继续。
    """
    if not script.is_file():
        console.print(f"[red]剧本文件不存在：{script}[/red]")
        raise typer.Exit(1)
    try:
        line = StyleLine(style)
    except ValueError:
        console.print("[red]风格线只能是 realistic 或 anime[/red]")
        raise typer.Exit(1) from None

    text = script.read_text(encoding="utf-8").strip()
    if not text:
        console.print("[red]剧本是空的[/red]")
        raise typer.Exit(1)

    root = project or script.parent / script.stem
    settings = _settings(root if root.is_dir() else None)
    asyncio.run(_make(text, root, episode, duration, line, settings,
                      draft_only, plan_only))


async def _make(
    script: str, root: Path, episode_id: str, duration: float,
    style_line: StyleLine, settings: Settings, draft_only: bool, plan_only: bool,
) -> None:
    from .comfy.client import ComfyClient
    from .models.project import Episode, ProjectStore
    from .pipeline import Pipeline, progress_printer
    from .stages.bible import BibleError, BibleGenerator
    from .stages.storyboard import StoryboardError, StoryboardGenerator

    store = ProjectStore(root)
    if not store.exists():
        store = ProjectStore.create(root, _slugify(root.name), root.name, style_line)
        console.print(f"已创建项目：[cyan]{store.root}[/cyan]")

    project = store.load_project()
    assets = store.load_assets()

    # 一、角色圣经。已有就不重做，避免覆盖用户改过的设定。
    if not assets.characters:
        console.print("[cyan]角色设定[/cyan]  正在读剧本")
        try:
            assets = await BibleGenerator(settings.llm).generate(script, style_line)
        except BibleError as exc:
            console.print(f"[red]{exc}[/red]")
            raise typer.Exit(1) from exc
        store.save_assets(assets)
        for c in assets.characters.values():
            console.print(f"  [green]✓[/green] {c.name}  {c.appearance.face[:40]}")
        for loc in assets.locations.values():
            console.print(f"  [green]✓[/green] {loc.name}")
    else:
        console.print(f"[cyan]角色设定[/cyan]  已有 {len(assets.characters)} 个角色，跳过")

    # 二、分镜表
    ep = project.episode_by_id(episode_id)
    if ep is None or not ep.shots:
        console.print(f"[cyan]分镜表[/cyan]  目标 {duration:.0f} 秒")
        try:
            shots = await StoryboardGenerator(settings.llm).generate(
                script, assets, episode_id, duration)
        except StoryboardError as exc:
            console.print(f"[red]{exc}[/red]")
            raise typer.Exit(1) from exc
        if ep is None:
            ep = Episode(episode_id=episode_id, script=script,
                         target_duration_s=duration)
            project.episodes.append(ep)
        ep.shots = shots
        ep.script = script
        store.save_project(project)
        total = sum(s.duration_s for s in shots)
        lipsync = sum(1 for s in shots if s.needs_lipsync)
        console.print(f"  {len(shots)} 个镜头，总时长 {total:.0f} 秒，"
                      f"其中 {lipsync} 个需要口型")
    else:
        console.print(f"[cyan]分镜表[/cyan]  已有 {len(ep.shots)} 个镜头，跳过")

    if plan_only:
        console.print()
        console.print("分镜表已就绪。可以在界面里检查和修改：")
        console.print(f"  [bold]changji serve --project {store.root}[/bold]")
        console.print("确认无误后去掉 --plan-only 再跑一次，会从分镜之后继续。")
        return

    # 三、渲染到成片
    client = ComfyClient(settings.comfy)
    if not await client.ping():
        console.print(f"[red]连不上 ComfyUI（{settings.comfy.base_url}）。"
                      f"先跑 changji doctor 看看[/red]")
        raise typer.Exit(1)

    from .workflows_loader import load_all
    wfs = await load_all(client, store)
    if wfs["tts"] is not None:
        console.print("  已找到配音工作流，将使用真实配音")
    pipeline = Pipeline(
        store, settings, client, wfs["video"],
        listener=progress_printer(console),
        image_workflow=wfs["image"], tts_workflow=wfs["tts"])
    report = await pipeline.run(episode_id, skip_final=draft_only)

    console.print()
    console.print(report.render())
    if not report.ok:
        raise typer.Exit(1)




@app.command()
def nodes(
    kind: str = typer.Argument("tts", help="要查的类别：tts、image、video、all"),
    project: Path = typer.Option(None, "--project", "-p"),
) -> None:
    """查 ComfyUI 上有哪些可用节点。

    搭配音或出图工作流时用它，不用凭记忆猜节点名。
    """
    settings = _settings(project)
    asyncio.run(_list_nodes(settings, kind))


async def _list_nodes(settings: Settings, kind: str) -> None:
    from .comfy.client import ComfyClient, ComfyUnavailable

    keywords = {
        "tts": ("tts", "speech", "voice", "audio"),
        "image": ("image", "vae", "clip", "latent"),
        "video": ("video", "wan", "animate", "frame"),
        "all": (),
    }.get(kind)
    if keywords is None:
        console.print(f"[red]类别只能是 tts、image、video 或 all，收到 {kind}[/red]")
        raise typer.Exit(1)

    client = ComfyClient(settings.comfy)
    try:
        info = await client.object_info()
    except ComfyUnavailable as exc:
        console.print(f"[red]{exc}[/red]")
        raise typer.Exit(1) from exc

    matched = sorted(
        name for name in info
        if not keywords or any(k in name.lower() for k in keywords)
    )
    if not matched:
        console.print(f"没有找到 {kind} 相关的节点。")
        console.print("装自定义节点后重启 ComfyUI 再试。")
        return

    # 云 API 节点和本地节点分开，因为选型时这是首要区分
    cloud_marks = ("elevenlabs", "fishaudio", "heygen", "openai", "minimax",
                   "kling", "veo", "bytedance", "comfycloud", "runway", "luma",
                   "pika", "recraft", "ideogram", "stability", "tripo", "rodin",
                   "gemini", "moonvalley", "vidu", "wan_api", "pixverse")
    local = [n for n in matched if not any(m in n.lower() for m in cloud_marks)]
    cloud = [n for n in matched if any(m in n.lower() for m in cloud_marks)]

    console.print(f"[bold]{settings.comfy.base_url}[/bold] 上的 {kind} 节点")
    console.print()
    console.print(f"[cyan]本地节点 {len(local)} 个[/cyan]")
    for name in local:
        console.print(f"  {name}")
    if cloud:
        console.print()
        console.print(f"[yellow]云 API 节点 {len(cloud)} 个（要联网和密钥）[/yellow]")
        for name in cloud[:12]:
            console.print(f"  {name}")
        if len(cloud) > 12:
            console.print(f"  还有 {len(cloud) - 12} 个")


@app.command()
def serve(
    project: Path = typer.Option(None, "--project", "-p", help="默认打开的项目"),
    host: str = typer.Option("127.0.0.1", "--host"),
    port: int = typer.Option(8080, "--port"),
) -> None:
    """启动 Web 界面。"""
    from .web.server import run_server

    settings = _settings(project)
    console.print(f"场记已启动：[cyan]http://{host}:{port}[/cyan]")
    console.print(f"ComfyUI：{settings.comfy.base_url}")
    console.print("按 Ctrl+C 停止。")
    run_server(settings, host=host, port=port, project=project)


def _slugify(name: str) -> str:
    """目录名转成合法的项目 id。中文目录名也要能用。"""
    import hashlib
    import re

    slug = re.sub(r"[^a-z0-9_-]+", "-", name.lower()).strip("-")
    if not slug:
        # 纯中文目录名，用哈希兜底保证稳定且合法
        slug = "p-" + hashlib.sha1(name.encode("utf-8")).hexdigest()[:8]
    return slug


if __name__ == "__main__":
    app()
