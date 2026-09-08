r"""导出 `build_timeline` 的排期结果，和 Python 逐个比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_timeline_golden.py

产出 cpp/tests/golden/timeline.json。

`export_assemble_golden.py` 已经比过**由时间线拼出来的 ffmpeg 命令**，
但没有直接比时间线本身。命令里只看得见一部分（adelay 的偏移），
而时间线里最要紧的那部分——**字幕的时间戳**——是走另一条路
（烧进 .ass 文件）出去的。`coverage_audit.py` 一直把
`Timeline` / `TimelineEntry` / `build_timeline` 列在没碰过的符号里。

**为什么这是音画对齐的最后一环。** 字幕的每一条 cue 的起止时间，
来自 `line.actual_duration_s`——也就是配音阶段量出来、写回台词的那个数
（那一步刚由 `audio_stage.json` 钉住）。这里再把它累成时间轴。
算错了不会报错：字幕文件生成成功、成片渲染成功、时长也对，
只是**字幕比人声早半秒或晚半秒**，而且一集下来越飘越远，
每一条单看都"差不多对"。

三处特别容易抄歪，语料专门覆盖：

  - **溶解的回挪**：`start = max(0, cursor - overlap)`，
    而 `overlap` 只在转场不是硬切时才不为零。
    漏了这一步，用了溶解的那一集**从第二镜起全部字幕整体偏移**。
  - **`speech_cursor` 从 `start` 起算，不是从 `cursor`**。
    这两个在有溶解时不是一个数。
  - **没有 cue 的台词照样推进 `speech_cursor`**：
    空文本或 `dur == 0` 不出字幕，但后面那句的起点仍然要往后挪。
    写成 `continue` 就会让后面所有字幕提前。

⚠️ `build_timeline` 会检查视频文件**真的在盘上**（不在就抛
`AssemblyError`），所以这里要造出真实文件来。
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.assembly.assemble import build_timeline                # noqa: E402
from changji.config import AssemblyConfig                           # noqa: E402
from changji.models.project import ProjectPaths                     # noqa: E402
from changji.models.shot import (                                   # noqa: E402
    CharacterInShot, DialogueLine, Shot, ShotStatus, Transition,
)

DEST = REPO / "cpp" / "tests" / "golden" / "timeline.json"


def line(text: str, dur: float | None, *, char_id: str | None = "c_lin",
         audio: str | None = "audio/x.wav") -> DialogueLine:
    d = DialogueLine(text=text, char_id=char_id)
    d.actual_duration_s = dur
    d.audio_path = audio
    return d


def shot(sid: str, *, duration: float, lines: list[DialogueLine],
         transition: Transition = Transition.CUT,
         trans_dur: float = 0.0, video: str = None) -> Shot:
    # 说话的角色必须也在 characters 里，否则模型判定分镜自相矛盾
    speakers = sorted({l.char_id for l in lines if l.char_id})
    s = Shot(
        shot_id=sid, scene_id="sc01", order=0,
        visual_desc="雨夜天台", first_frame_prompt="雨夜天台",
        motion_prompt="推近", duration_s=duration,
        status=ShotStatus.FINAL_DONE, dialogue=lines,
        characters=[CharacterInShot(char_id=c) for c in speakers],
    )
    s.video_path = video or f"video/{sid}.mp4"
    s.transition_in = transition
    s.transition_dur_s = trans_dur
    return s


def cases() -> list[tuple[str, list[Shot]]]:
    out: list[tuple[str, list[Shot]]] = []

    out.append(("一镜一句", [
        shot("sh001", duration=3.0, lines=[line("你终于来了。", 2.0)])]))

    out.append(("一镜两句，第二句要接着第一句", [
        shot("sh001", duration=5.0,
             lines=[line("你终于来了。", 2.0), line("雨下了一夜。", 1.5)])]))

    out.append(("两镜硬切", [
        shot("sh001", duration=3.0, lines=[line("第一句。", 2.0)]),
        shot("sh002", duration=4.0, lines=[line("第二句。", 2.5)])]))

    # **溶解要把起点往回挪。** 漏了这一步，第二镜起全部字幕整体偏移。
    out.append(("两镜溶解，第二镜起点要回挪", [
        shot("sh001", duration=3.0, lines=[line("第一句。", 2.0)]),
        shot("sh002", duration=4.0, lines=[line("第二句。", 2.5)],
             transition=Transition.DISSOLVE, trans_dur=0.5)]))

    # 回挪量比游标还大：max(0, ...) 那一支
    out.append(("第一镜就带溶解，回挪不能变成负数", [
        shot("sh001", duration=3.0, lines=[line("第一句。", 2.0)],
             transition=Transition.DISSOLVE, trans_dur=1.0)]))

    out.append(("没有台词的镜头", [
        shot("sh001", duration=4.0, lines=[]),
        shot("sh002", duration=3.0, lines=[line("终于说话了。", 2.0)])]))

    # **不出字幕、但照样推进游标的两种**
    out.append(("空文本不出字幕，但后面那句要往后挪", [
        shot("sh001", duration=6.0,
             lines=[line("   ", 1.5), line("这句才有字幕。", 2.0)])]))

    out.append(("时长为 0 不出字幕，但也要占位", [
        shot("sh001", duration=6.0,
             lines=[line("这句没配出来。", 0.0), line("这句有。", 2.0)])]))

    out.append(("actual_duration_s 是 None", [
        shot("sh001", duration=5.0,
             lines=[line("还没配音。", None), line("这句配了。", 2.0)])]))

    # 旁白和对白的 style 不一样
    out.append(("旁白算 narration，有角色的算 dialogue", [
        shot("sh001", duration=6.0,
             lines=[line("雨下了一整夜。", 2.0, char_id=None),
                    line("你终于来了。", 1.5, char_id="c_lin")])]))

    # 没有 audio_path 的台词：不进 audio_paths，但字幕照出
    out.append(("有台词没音频文件", [
        shot("sh001", duration=4.0,
             lines=[line("这句没有音频文件。", 2.0, audio=None)])]))

    # 台词总时长超过镜头时长——字幕会溢出到下一镜，两边得一样地溢
    out.append(("台词比镜头长，字幕溢出到下一镜", [
        shot("sh001", duration=2.0,
             lines=[line("一句很长的台词。", 5.0)]),
        shot("sh002", duration=3.0, lines=[line("下一镜。", 1.0)])]))

    out.append(("三镜连续溶解", [
        shot("sh001", duration=3.0, lines=[line("一。", 1.0)]),
        shot("sh002", duration=3.0, lines=[line("二。", 1.0)],
             transition=Transition.DISSOLVE, trans_dur=0.5),
        shot("sh003", duration=3.0, lines=[line("三。", 1.0)],
             transition=Transition.DISSOLVE, trans_dur=0.75)]))

    return out


def dump(tl, root: Path) -> dict:
    def rel(p) -> str:
        try:
            return str(Path(p).relative_to(root)).replace("\\", "/")
        except ValueError:
            return str(p).replace("\\", "/")

    return {
        "total_duration_s": round(tl.total_duration_s, 6),
        "entries": [{
            "shot_id": e.shot_id,
            "video_path": rel(e.video_path),
            "start_s": round(e.start_s, 6),
            "duration_s": round(e.duration_s, 6),
            "transition_in": e.transition_in.value,
            "transition_dur_s": round(e.transition_dur_s, 6),
            "audio_paths": [rel(p) for p in e.audio_paths],
            "cues": [{"start_s": round(c.start_s, 6),
                      "end_s": round(c.end_s, 6),
                      "text": c.text, "style": c.style} for c in e.cues],
        } for e in tl.entries],
        "all_cues": [{"start_s": round(c.start_s, 6),
                      "end_s": round(c.end_s, 6),
                      "text": c.text, "style": c.style} for c in tl.cues()],
    }


def main() -> int:
    out = []
    with tempfile.TemporaryDirectory(prefix="changji_时间线_") as tmp:
        root = Path(tmp)
        paths = ProjectPaths(root)
        (root / "video").mkdir(parents=True, exist_ok=True)
        (root / "audio").mkdir(parents=True, exist_ok=True)
        (root / "audio" / "x.wav").write_bytes(b"wav")

        for name, shots in cases():
            # build_timeline 会查视频文件真的在不在盘上
            for s in shots:
                (root / s.video_path).write_bytes(b"mp4")
            # **输入的台词要单独记。** cues 还原不出它——
            # "空文本不出字幕""时长 0 不出字幕"这几条正是被测的行为，
            # 拿 cues 反推等于把被测的那一步当成输入。
            din = [[{"text": l.text,
                     "char_id": l.char_id,
                     "actual_duration_s": l.actual_duration_s,
                     "audio_path": l.audio_path} for l in sh.dialogue]
                   for sh in shots]
            tl = build_timeline(shots, paths, AssemblyConfig())
            out.append({"name": name, "dialogue_in": din,
                        "timeline": dump(tl, root)})

    payload = {
        "cases": out,
        "note": ("由 cpp/tests/export_timeline_golden.py 生成，不要手改。"
                 "字幕时间戳来自配音量出来的真实时长——算错了不报错，"
                 "表现是字幕和人声对不上，而且一集下来越飘越远。"),
    }
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")

    print(f"写入 {DEST}，{len(out)} 条")
    for c in out:
        tl = c["timeline"]
        starts = "、".join(f"{e['start_s']:g}" for e in tl["entries"])
        print(f"  {c['name']:28} 起点 {starts:14} "
              f"字幕 {len(tl['all_cues'])} 条  总长 {tl['total_duration_s']:g}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
