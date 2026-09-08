"""导出装配阶段的 ffmpeg 命令行语料。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_assemble_golden.py

产出 cpp/tests/golden/ffmpeg/commands.json。

**为什么需要这一份。** `golden/ffmpeg/parsers.json` 只覆盖**解析 ffmpeg 的
输出**，从来没有哪一份语料覆盖**拼 ffmpeg 的命令**。而
`test_assemble.cpp` 那 60 条断言钉的是 C++ 自己的意图，不是"和 Python 一样"。

代价是真的：2026-09-08 抓到 `mix_args` 里 loudnorm 的响度目标被 `%g`
截到 6 位有效数字，而那条用例**把截过的值当成正确答案钉住了**
（`CHECK(c.find("loudnorm=I=-14:TP=-2"))`）。自己钉自己，钉错了也是绿的。

期望值全部由**调真的 Python 函数**得到，一个都不手写：假一个 FFmpeg，
把 `run_ffmpeg` 收到的 argv 记下来。

**路径的处理。** argv 里有临时目录的绝对路径，每台机器都不一样。所以：

    项目根       -> 字面量 <ROOT>
    反斜杠       -> 正斜杠

两条都在 C++ 那边做同样的替换。**这是唯一放宽的地方**，放宽的是路径的
拼写，不是命令的语义——除此之外逐字节比。
"""
from __future__ import annotations

import asyncio
import io
import json
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.assembly.assemble import (                            # noqa: E402
    Assembler, _escape_filter_path, build_timeline,
)
from changji.assembly.ffmpeg import MediaInfo                      # noqa: E402
from changji.config import AssemblyConfig                          # noqa: E402
from changji.models.project import ProjectPaths                    # noqa: E402
from changji.models.shot import (                                  # noqa: E402
    CharacterInShot, DialogueLine, Shot, Transition,
)

DEST = REPO / "cpp" / "tests" / "golden" / "ffmpeg" / "commands.json"

# 探测结果写死。这一份语料比的是**拼出来的命令**，不是探测本身——
# 探测的解析在 parsers.json 里比过了。
PROBE_W, PROBE_H, PROBE_DUR = 1080, 1920, 12.5


class FakeFFmpeg:
    """只记不跑。argv 记的是 run_ffmpeg 收到的那一份。"""

    def __init__(self) -> None:
        self.calls: list[list[str]] = []

    async def run_ffmpeg(self, args: list[str], timeout_s: float = 1800.0) -> str:
        self.calls.append(list(args))
        return ""

    async def probe(self, path: Path) -> MediaInfo:
        return MediaInfo(path=path, duration_s=PROBE_DUR,
                         width=PROBE_W, height=PROBE_H, fps=24.0,
                         has_video=True, has_audio=True)


def norm(text: str, root: Path) -> str:
    r"""把机器相关的部分抹掉。见模块开头。

    根有三种拼法要认，**转义过的那种必须先替**：烧字幕那条命令里的路径
    已经被 `_escape_filter_path` 处理过（反斜杠变正斜杠、冒号变 `\:`），
    先做无脑的反斜杠替换会把 `C\:` 变成 `C/:`——转义就毁了，
    而且根也认不出来。
    """
    native = str(root)
    posix = native.replace("\\", "/")
    escaped = posix.replace(":", "\\:")     # 滤镜转义之后的样子
    for form in (escaped, native, posix):
        text = text.replace(form, "<ROOT>")
    return text.replace("\\", "/")


def touch(p: Path, blob: bytes = b"x" * 2048) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(blob)


def make_shots(paths: ProjectPaths) -> list[Shot]:
    """两镜，第二镜溶入；第一镜两句台词（一句旁白），第二镜没有台词。

    没有台词的那一镜是**故意的**：混音时它不贡献音频段，
    而"配音总长短于画面"正是 -shortest 会丢镜头的那个场景。
    """
    touch(paths.abs("shots/draft/a.mp4"))
    touch(paths.abs("shots/draft/b.mp4"))
    touch(paths.abs("audio/a_1.wav"))
    touch(paths.abs("audio/a_2.wav"))

    a = Shot(shot_id="ep01_sh001", scene_id="sc01", order=0,
             visual_desc="雨夜天台", first_frame_prompt="雨夜天台",
             motion_prompt="镜头缓慢推近", duration_s=6.0,
             video_path="shots/draft/a.mp4",
             transition_in=Transition.CUT, transition_dur_s=0.0,
             characters=[CharacterInShot(char_id="c_lin_wan")],
             dialogue=[
                 DialogueLine(text="你终于来了。", char_id="c_lin_wan",
                              actual_duration_s=2.5,
                              audio_path="audio/a_1.wav"),
                 DialogueLine(text="雨下了一整夜。", char_id=None,
                              actual_duration_s=2.0,
                              audio_path="audio/a_2.wav"),
             ])
    b = Shot(shot_id="ep01_sh002", scene_id="sc02", order=1,
             visual_desc="楼下街角", first_frame_prompt="楼下街角",
             motion_prompt="固定", duration_s=5.0,
             video_path="shots/draft/b.mp4",
             transition_in=Transition.DISSOLVE, transition_dur_s=0.4,
             dialogue=[])
    return [a, b]


async def collect(root: Path) -> dict:
    paths = ProjectPaths(root)
    paths.ensure()
    config = AssemblyConfig()
    shots = make_shots(paths)
    timeline = build_timeline(shots, paths, config)

    ff = FakeFFmpeg()
    asm = Assembler(ff, config, paths, target_lufs=-16.0, max_true_peak_db=-1.5)
    work = paths.output / ".work"
    work.mkdir(parents=True, exist_ok=True)

    out: dict = {}

    # ---- 统一规格 ----
    ff.calls.clear()
    normalized = await asm._normalize_all(timeline, work)
    out["normalize"] = {
        "target_w": PROBE_W, "target_h": PROBE_H,
        "srcs": [norm(str(e.video_path), root) for e in timeline.entries],
        "dests": [norm(str(p), root) for p in normalized],
        "calls": [[norm(a, root) for a in c] for c in ff.calls],
    }

    # ---- 拼接 ----
    ff.calls.clear()
    joined = await asm._concat(normalized, work)
    out["concat"] = {
        "clips": [norm(str(p), root) for p in normalized],
        "listing_path": norm(str(work / "concat.txt"), root),
        "listing": norm((work / "concat.txt").read_text(encoding="utf-8"), root),
        "dest": norm(str(joined), root),
        "calls": [[norm(a, root) for a in c] for c in ff.calls],
    }

    # ---- 混音（有配音）----
    ff.calls.clear()
    mixed = await asm._mix_audio(joined, timeline, work)
    segs = []
    for entry in timeline.entries:
        cursor = entry.start_s
        for idx, audio in enumerate(entry.audio_paths):
            segs.append({"path": norm(str(audio), root), "at_s": cursor})
            cue = entry.cues[idx] if idx < len(entry.cues) else None
            cursor += cue.duration_s if cue else 0.0
    out["mix"] = {
        "video": norm(str(joined), root),
        "segments": segs,
        "target_lufs": -16.0, "max_true_peak_db": -1.5,
        "video_duration_s": PROBE_DUR,
        "dest": norm(str(mixed), root),
        "calls": [[norm(a, root) for a in c] for c in ff.calls],
    }

    # ---- 混音（响度目标带一串小数）----
    #
    # **这一条是这份语料存在的直接理由。** 上面那条用的是默认的
    # -16.0 / -1.5，而 C++ 原来的 %g（6 位有效数字）把这两个值渲染成
    # -16 / -1.5，抹掉 ".0" 之后和 Python 一样——**默认值根本试不出那个 bug**。
    # 要让截位暴露出来，响度目标得有 6 位以上有效数字。
    ff.calls.clear()
    precise = Assembler(ff, config, paths,
                        target_lufs=-16.123456, max_true_peak_db=-1.234567)
    precise_dest = await precise._mix_audio(joined, timeline, work)
    out["mix_precise"] = {
        "video": norm(str(joined), root),
        "segments": segs,
        "target_lufs": -16.123456, "max_true_peak_db": -1.234567,
        "video_duration_s": PROBE_DUR,
        "dest": norm(str(precise_dest), root),
        "calls": [[norm(a, root) for a in c] for c in ff.calls],
    }

    # ---- 混音（没有配音，走静音轨）----
    ff.calls.clear()
    silent_timeline = build_timeline(
        [s.model_copy(update={"dialogue": []}) for s in shots], paths, config)
    silent_dest = await asm._mix_audio(joined, silent_timeline, work)
    out["silent_audio"] = {
        "video": norm(str(joined), root),
        "dest": norm(str(silent_dest), root),
        "calls": [[norm(a, root) for a in c] for c in ff.calls],
    }

    # ---- 烧字幕 ----
    ff.calls.clear()
    final = paths.output / "episode.mp4"
    await asm._burn_subtitles(mixed, timeline, final)
    out["burn"] = {
        "video": norm(str(mixed), root),
        "ass_path": norm(str(paths.subtitles / "episode.ass"), root),
        "dest": norm(str(final), root),
        "calls": [[norm(a, root) for a in c] for c in ff.calls],
    }

    # ---- 滤镜里的路径转义 ----
    # Windows 上 C:\x 里的冒号是滤镜的参数分隔符，反斜杠是转义符。
    out["escape_filter_path"] = [
        {"input": p, "expected": _escape_filter_path(Path(p))}
        for p in [
            r"C:\项目 库\雨夜天台\subtitles\ep01.ass",
            "/home/u/项目/subtitles/ep01.ass",
            r"D:\a\b.ass",
            "relative/x.ass",
        ]
    ]

    # ---- 时间线本身 ----
    # 混音的偏移量全从这里来，对不上的话 argv 里的 adelay 也就对不上。
    out["timeline"] = [
        {"shot_id": e.shot_id, "start_s": e.start_s, "duration_s": e.duration_s,
         "audio_paths": [norm(str(p), root) for p in e.audio_paths],
         "cues": [{"start_s": c.start_s, "end_s": c.end_s, "text": c.text,
                   "style": c.style} for c in e.cues]}
        for e in timeline.entries
    ]
    return out


def main() -> int:
    root = Path(tempfile.mkdtemp(prefix="changji_装配语料_"))
    try:
        payload = asyncio.run(collect(root))
    finally:
        shutil.rmtree(root, ignore_errors=True)

    payload["note"] = (
        "由 cpp/tests/export_assemble_golden.py 生成，不要手改。"
        "argv 里项目根替换成 <ROOT>，反斜杠替换成正斜杠——"
        "C++ 那边做同样的替换。除此之外逐字节比。"
    )
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    n = sum(len(v["calls"]) for k, v in payload.items()
            if isinstance(v, dict) and "calls" in v)
    print(f"写入 {DEST}，{n} 条命令")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
