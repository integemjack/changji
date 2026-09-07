"""生成 ffmpeg / ffprobe 输出解析的对拍语料。

跑法（在 changji/ 下）：
    .venv/Scripts/python.exe cpp/tools/gen_ffmpeg_golden.py

**期望值全部由 Python 侧的真函数算出来**，不是手写的。probe 和
measure_loudness 不是纯函数，所以这里把它们的子进程那一步换掉，
让真正的解析代码跑在样本上——换掉执行、保留解析，正是对拍要比的那部分。

样本输出是照着 ffmpeg 真实格式写的（这台机器上没装 ffmpeg，跑不出来）。
这不削弱对拍：样本在这里是**输入**，要证明的是同一份输入下两边解析结果一致。
每个样本都盯着一个具体的坑，见下面各自的注释。
"""

from __future__ import annotations

import asyncio
import io
import json
import sys
import tempfile
from dataclasses import asdict
from pathlib import Path

HERE = Path(__file__).resolve()
CPP = HERE.parent.parent
ROOT = CPP.parent
sys.path.insert(0, str(ROOT / "src"))

from changji.assembly.ffmpeg import (                                # noqa: E402
    FFmpeg, _parse_fps, _parse_signalstats, _sample_points,
)

OUT = CPP / "tests" / "golden" / "ffmpeg"


# ---- ffprobe 样本 ----

PROBE_SAMPLES = {
    # 正常的一段草稿档视频
    "draft_clip": {
        "streams": [
            {"index": 0, "codec_type": "video", "codec_name": "h264",
             "width": 448, "height": 768, "pix_fmt": "yuv420p",
             "r_frame_rate": "24/1", "nb_frames": "121",
             "sample_aspect_ratio": "1:1", "duration": "5.041667"},
        ],
        "format": {"duration": "5.041667", "format_name": "mov,mp4,m4a"},
    },
    # NTSC 帧率。直接 stod 的话 "24000/1001" 会变成 24000，
    # 之后所有按帧率算的时长全错。
    "ntsc_framerate": {
        "streams": [
            {"index": 0, "codec_type": "video", "codec_name": "h264",
             "width": 1920, "height": 1080, "pix_fmt": "yuv420p",
             "r_frame_rate": "24000/1001", "nb_frames": "240",
             "duration": "10.0"},
        ],
        "format": {"duration": "10.0"},
    },
    # format 里没有时长，只有视频轨上有。不回落的话时长永远是 0，
    # 而按时长做的校验会全部通过。
    "duration_only_on_stream": {
        "streams": [
            {"index": 0, "codec_type": "video", "width": 640, "height": 352,
             "r_frame_rate": "24/1", "duration": "3.5"},
        ],
        "format": {"format_name": "matroska"},
    },
    # 带音轨的成片
    "with_audio": {
        "streams": [
            {"index": 0, "codec_type": "video", "width": 1280, "height": 704,
             "r_frame_rate": "24/1", "nb_frames": "2880", "pix_fmt": "yuv420p"},
            {"index": 1, "codec_type": "audio", "codec_name": "aac",
             "sample_rate": "48000", "channels": 2},
        ],
        "format": {"duration": "120.0"},
    },
    # 纯音频（配音文件）
    "audio_only": {
        "streams": [
            {"index": 0, "codec_type": "audio", "codec_name": "pcm_s16le",
             "sample_rate": "24000", "channels": 1},
        ],
        "format": {"duration": "3.2"},
    },
    # 一堆字段是 "N/A"。ffprobe 对某些容器就这么给，那不是错误。
    "na_fields": {
        "streams": [
            {"index": 0, "codec_type": "video", "width": 448, "height": 768,
             "r_frame_rate": "0/0", "nb_frames": "N/A", "duration": "N/A"},
        ],
        "format": {"duration": "N/A"},
    },
}


# ---- signalstats 样本 ----

def stats_block(**kv) -> str:
    """照着 metadata=print 的真实格式拼。

    真实输出长这样（前面还有 frame:0 pts:0 那一行）：
        frame:0    pts:0       pts_time:0
        lavfi.signalstats.YMIN=16
        ...
    """
    lines = ["frame:0    pts:0       pts_time:0"]
    for k, v in kv.items():
        lines.append(f"lavfi.signalstats.{k}={v}")
    return "\n".join(lines) + "\n"


SIGNALSTATS_SAMPLES = {
    # 正常画面：展布 40 以上
    "normal_frame": stats_block(
        YMIN=16, YLOW=48, YAVG=112.5, YHIGH=201, YMAX=235, YDIF=0.0),
    # 近乎纯色：极差很大（有个高光点）但百分位展布是个位数。
    # **拿极差判的话这张会被当成正常画面。**
    "blank_with_hotspot": stats_block(
        YMIN=16, YLOW=126, YAVG=127.2, YHIGH=130, YMAX=250, YDIF=0.0),
    # 全黑
    "all_black": stats_block(
        YMIN=0, YLOW=0, YAVG=0.4, YHIGH=1, YMAX=3, YDIF=0.0),
    # 过曝
    "clipped_white": stats_block(
        YMIN=248, YLOW=250, YAVG=253.1, YHIGH=255, YMAX=255, YDIF=0.0),
    # 老版本 ffmpeg 没有 YLOW/YHIGH，只能退回极差
    "no_percentiles": stats_block(YMIN=16, YAVG=112.5, YMAX=235),
    # 混在 ffmpeg 日志中间。真实输出前后都有别的行。
    "with_log_noise": (
        "ffmpeg version 6.1 Copyright (c) 2000-2023 the FFmpeg developers\n"
        "  Stream #0:0: Video: h264, yuv420p, 448x768\n"
        + stats_block(YMIN=16, YLOW=48, YAVG=112.5, YHIGH=201, YMAX=235)
        + "frame=    1 fps=0.0 q=-0.0 Lsize=N/A time=00:00:00.04\n"
    ),
}


# ---- loudnorm 样本 ----

LOUDNORM_SAMPLE = """\
ffmpeg version 6.1 Copyright (c) 2000-2023 the FFmpeg developers
  Stream #0:0: Audio: pcm_s16le, 24000 Hz, mono, s16, 384 kb/s
[Parsed_loudnorm_0 @ 000001d4] \n\
{
\t"input_i" : "-23.45",
\t"input_tp" : "-4.32",
\t"input_lra" : "7.10",
\t"input_thresh" : "-33.51",
\t"output_i" : "-16.02",
\t"output_tp" : "-1.50",
\t"output_lra" : "6.90",
\t"output_thresh" : "-26.08",
\t"normalization_type" : "dynamic",
\t"target_offset" : "0.02"
}
size=N/A time=00:00:03.20 bitrate=N/A speed= 118x
"""


class FakeProc:
    """把 measure_loudness 的子进程那一步换掉，让真正的解析代码跑起来。"""

    def __init__(self, err: str) -> None:
        self._err = err.encode("utf-8")

    async def communicate(self):
        return b"", self._err


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    ff = FFmpeg()
    payload = {"probe": [], "signalstats": [], "loudnorm": {}, "misc": {}}

    with tempfile.TemporaryDirectory() as tmp:
        fake_file = Path(tmp) / "sample.mp4"
        fake_file.write_bytes(b"not really a video")

        for name, doc in PROBE_SAMPLES.items():
            text = json.dumps(doc, ensure_ascii=False)
            # 换掉执行、保留解析——对拍要比的就是解析那部分
            ff.run_ffprobe = lambda _args, _t=text: _echo(_t)  # type: ignore[assignment]
            try:
                info = asyncio.run(ff.probe(fake_file))
            except Exception as exc:
                # **Python 侧会炸的样本也要进语料。**
                # nb_frames 是 "N/A" 时 int() 抛 ValueError，而 ffprobe 对
                # mkv 和没有索引的流就是这么给的。记下来，C++ 侧那条用例
                # 钉的是"不复刻这个崩溃"。
                payload["probe"].append({
                    "name": name, "input": text,
                    "python_raises": f"{type(exc).__name__}: {exc}",
                })
                print(f"probe/{name}: Python 抛了 {type(exc).__name__}")
                continue
            d = asdict(info)
            d.pop("path")   # 临时目录，不进语料
            payload["probe"].append({"name": name, "input": text, "expected": d})
            print(f"probe/{name}: {d['width']}x{d['height']} "
                  f"{d['fps']:.4f}fps {d['duration_s']}s")

    for name, text in SIGNALSTATS_SAMPLES.items():
        stats = _parse_signalstats(text)
        d = asdict(stats)
        d["looks_blank"] = stats.looks_blank
        d["looks_clipped"] = stats.looks_clipped
        payload["signalstats"].append(
            {"name": name, "input": text, "expected": d})
        print(f"signalstats/{name}: spread={d['spread']} blank={d['looks_blank']}")

    # loudnorm：同样换掉子进程
    async def fake_exec(*_a, **_kw):
        return FakeProc(LOUDNORM_SAMPLE)

    import changji.assembly.ffmpeg as mod
    real = mod.asyncio.create_subprocess_exec
    mod.asyncio.create_subprocess_exec = fake_exec
    try:
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / "a.wav"
            f.write_bytes(b"x")
            loud = asyncio.run(FFmpeg().measure_loudness(f))
    finally:
        mod.asyncio.create_subprocess_exec = real
    payload["loudnorm"] = {"input": LOUDNORM_SAMPLE, "expected": loud}
    print(f"loudnorm: {len(loud)} 个字段")

    payload["misc"]["fps"] = [
        {"input": r, "expected": _parse_fps(r)}
        for r in ["24/1", "24000/1001", "30000/1001", "0/0", "25", "", "N/A",
                  "60/2", "abc/def"]
    ]
    payload["misc"]["sample_points"] = [
        {"input": n, "expected": _sample_points(n)}
        for n in [1, 2, 3, 5, 0, -1]
    ]

    dest = OUT / "parsers.json"
    dest.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                    encoding="utf-8")
    print(f"写到 {dest}")
    return 0


async def _echo(text: str) -> str:
    return text


if __name__ == "__main__":
    raise SystemExit(main())
