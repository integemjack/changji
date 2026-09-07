"""硬件探测与画质档位推导。

这个文件存在的理由是可移植性。画质档位绝不能写死成某一台机器的数字，
必须由运行时探测到的实际显存推导出来。

档位的基准点来自 RTX 5080 16GB 上对 Wan 2.2 TI2V 5B 的实测：
草稿 640x352 10 步 27 秒，预览 960x544 20 步 120 秒，成片 1280x704 30 步 392 秒。
其他显存档位按分辨率和步数缩放，首次运行时会用实测校准。
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from enum import Enum


class Tier(str, Enum):
    """画质档位。分级生成靠它。"""

    DRAFT = "draft"
    PREVIEW = "preview"
    FINAL = "final"


@dataclass(frozen=True)
class TierSpec:
    """一个档位的生成参数。"""

    tier: Tier
    width: int
    height: int
    steps: int
    # 本机实测的单镜耗时，未标定时为 None
    measured_seconds: float | None = None

    def scaled_to(self, aspect_ratio: str) -> TierSpec:
        """按画幅调整。分辨率必须是 32 的倍数，否则 Wan 的潜空间对不齐。"""
        if aspect_ratio == "9:16":
            long_side, short_side = max(self.width, self.height), min(self.width, self.height)
            w = _round32(short_side)
            h = _round32(long_side)
        elif aspect_ratio == "1:1":
            side = _round32((self.width + self.height) // 2)
            w = h = side
        else:  # 16:9
            long_side, short_side = max(self.width, self.height), min(self.width, self.height)
            w = _round32(long_side)
            h = _round32(short_side)
        return TierSpec(self.tier, w, h, self.steps, self.measured_seconds)


def _round32(n: int) -> int:
    return max(32, round(n / 32) * 32)


@dataclass(frozen=True)
class GPUInfo:
    name: str
    vram_mb: int
    driver: str | None = None

    @property
    def vram_gb(self) -> float:
        return self.vram_mb / 1024


def detect_gpu() -> GPUInfo | None:
    """探测本机显卡。探测不到返回 None，由调用方决定怎么办。

    只用 nvidia-smi，不引入 torch 依赖。这个包是编排引擎，
    真正的推理在 ComfyUI 那边，可能根本不在同一台机器上。
    """
    exe = shutil.which("nvidia-smi")
    if not exe:
        return None
    try:
        out = subprocess.run(
            [exe, "--query-gpu=name,memory.total,driver_version",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=15, check=True,
        ).stdout.strip()
    except (subprocess.SubprocessError, OSError):
        return None
    if not out:
        return None
    first = out.splitlines()[0]
    parts = [p.strip() for p in first.split(",")]
    if len(parts) < 2:
        return None
    try:
        vram = int(float(parts[1]))
    except ValueError:
        return None
    return GPUInfo(name=parts[0], vram_mb=vram, driver=parts[2] if len(parts) > 2 else None)


# 按显存分的档位表。键是该档位要求的最低显存 GB。
# 显存越小，分辨率和步数越保守，否则会 OOM 或者慢到不可用。
#
# 阈值全部比标称容量低 0.5，因为驱动和固件会占掉一部分，
# nvidia-smi 报出来的永远小于标称值。一张 16GB 的卡通常报 15.9，
# 按 16.0 卡阈值会把它错判成 12GB 档。
_TIER_TABLE: list[tuple[float, dict[Tier, tuple[int, int, int]]]] = [
    (23.5, {
        Tier.DRAFT: (768, 432, 10),
        Tier.PREVIEW: (1280, 704, 20),
        Tier.FINAL: (1920, 1088, 30),
    }),
    (15.5, {
        Tier.DRAFT: (640, 352, 10),
        Tier.PREVIEW: (960, 544, 20),
        Tier.FINAL: (1280, 704, 30),
    }),
    (11.5, {
        Tier.DRAFT: (512, 288, 8),
        Tier.PREVIEW: (768, 432, 18),
        Tier.FINAL: (960, 544, 28),
    }),
    (7.5, {
        Tier.DRAFT: (448, 256, 8),
        Tier.PREVIEW: (640, 352, 16),
        Tier.FINAL: (768, 432, 25),
    }),
]

# 基准档在 _TIER_TABLE 里的下标。耗时估算以它为原点，
# 从表里取而不是另写一个数字，避免改表后两处对不上。
_REFERENCE_INDEX = 1
_REFERENCE_VRAM_GB = _TIER_TABLE[_REFERENCE_INDEX][0]
# 该档位在 RTX 5080 16GB 上的实测单镜耗时
_REFERENCE_SECONDS = {Tier.DRAFT: 27.0, Tier.PREVIEW: 120.0, Tier.FINAL: 392.0}


def tiers_for_vram(vram_gb: float) -> dict[Tier, TierSpec]:
    """按显存推导三个档位的参数。"""
    for threshold, table in _TIER_TABLE:
        if vram_gb >= threshold:
            chosen, chosen_vram = table, threshold
            break
    else:
        chosen, chosen_vram = _TIER_TABLE[-1][1], _TIER_TABLE[-1][0]

    specs: dict[Tier, TierSpec] = {}
    for tier, (w, h, steps) in chosen.items():
        # 统一规整到 32 的倍数。表里手写的数字可能不合规，
        # 而不是 32 的倍数会导致 Wan 的潜空间对不齐。
        w, h = _round32(w), _round32(h)
        specs[tier] = TierSpec(tier, w, h, steps, _estimate_seconds(tier, w, h, steps, chosen_vram))
    return specs


def _estimate_seconds(tier: Tier, w: int, h: int, steps: int, vram_gb: float) -> float:
    """粗估单镜耗时。

    仅用于给用户一个数量级预期和排产估算，不是承诺。真实数字要靠 calibrate 命令
    在目标机器上实测。扩散模型耗时大致正比于像素数乘步数。
    """
    ref_w, ref_h, ref_steps = _TIER_TABLE[_REFERENCE_INDEX][1][tier]
    ref_work = _round32(ref_w) * _round32(ref_h) * ref_steps
    work = w * h * steps
    base = _REFERENCE_SECONDS[tier] * (work / ref_work)
    # 比基准档更小的机器通常算力也弱，且要频繁换入换出，给一个惩罚系数。
    # 基准档自身不吃惩罚，否则实测值会被凭空放大。
    if vram_gb < _REFERENCE_VRAM_GB:
        base *= 1.0 + (_REFERENCE_VRAM_GB - vram_gb) * 0.08
    return round(base, 1)


@dataclass
class HardwareProfile:
    """本机硬件画像。配置里可以覆盖，方便 ComfyUI 在别的机器上时手动指定。"""

    gpu: GPUInfo | None
    vram_gb: float
    tiers: dict[Tier, TierSpec]
    detected: bool

    @classmethod
    def detect(cls, override_vram_gb: float | None = None) -> HardwareProfile:
        gpu = detect_gpu()
        if override_vram_gb is not None:
            vram = override_vram_gb
            detected = False
        elif gpu is not None:
            vram = gpu.vram_gb
            detected = True
        else:
            # 探测不到就按 12GB 这个偏保守的假设走，并明确标记未探测到
            vram = 12.0
            detected = False
        return cls(gpu=gpu, vram_gb=vram, tiers=tiers_for_vram(vram), detected=detected)

    def describe(self) -> str:
        if self.gpu is not None:
            head = f"{self.gpu.name}，显存 {self.gpu.vram_gb:.1f} GB"
        elif self.detected:
            head = f"显存 {self.vram_gb:.1f} GB"
        else:
            head = f"未探测到显卡，按 {self.vram_gb:.1f} GB 估算"
        lines = [head]
        for tier in (Tier.DRAFT, Tier.PREVIEW, Tier.FINAL):
            spec = self.tiers[tier]
            secs = f"约 {spec.measured_seconds:.0f} 秒" if spec.measured_seconds else "未标定"
            lines.append(
                f"  {tier.value:8s} {spec.width}x{spec.height}  {spec.steps} 步  单镜 {secs}"
            )
        return "\n".join(lines)

    def estimate_episode(self, shot_count: int, tier: Tier) -> float | None:
        """估算一集的纯生成时间，单位秒。"""
        spec = self.tiers[tier]
        if spec.measured_seconds is None:
            return None
        return spec.measured_seconds * shot_count
