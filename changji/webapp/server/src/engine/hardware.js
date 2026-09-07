/**
 * 硬件画像与画质档位。
 *
 * 档位不写死在配置里，由显存推导——写死等于把这台机器的显存刻进项目，
 * 换台机器就不对了。ComfyUI 在别的机器上时本机探测不到显卡，
 * 配置里的 vram_gb_override 顶上。
 */

import { execFile } from 'node:child_process'

import { roundHalfEven } from './timing.js'

/** 画质档位。分级生成靠它。 */
export const Tier = {
  DRAFT: 'draft',
  PREVIEW: 'preview',
  FINAL: 'final',
}

/**
 * 分辨率必须是 32 的倍数，否则 Wan 的潜空间对不齐。
 *
 * 用四舍六入五取偶而不是 Math.round：n=80 时 80/32=2.5，
 * Python 给 64、Math.round 给 96——同一张卡在两个引擎上会拿到不同的
 * 分辨率，而分辨率不符会被质量闸门判成「档位参数没生效」。
 */
export const round32 = (n) => Math.max(32, roundHalfEven(n / 32) * 32)

/**
 * 显存越小，分辨率和步数越保守，否则会 OOM 或者慢到不可用。
 *
 * 阈值全部比标称容量低 0.5，因为驱动和固件会占掉一部分，
 * nvidia-smi 报出来的永远小于标称值。一张 16GB 的卡通常报 15.9，
 * 按 16.0 卡阈值会把它错判成 12GB 档。
 */
const TIER_TABLE = [
  [23.5, { draft: [768, 432, 10], preview: [1280, 704, 20], final: [1920, 1088, 30] }],
  [15.5, { draft: [640, 352, 10], preview: [960, 544, 20], final: [1280, 704, 30] }],
  [11.5, { draft: [512, 288, 8], preview: [768, 432, 18], final: [960, 544, 28] }],
  [7.5, { draft: [448, 256, 8], preview: [640, 352, 16], final: [768, 432, 25] }],
]

/**
 * 基准档在 TIER_TABLE 里的下标。耗时估算以它为原点，
 * 从表里取而不是另写一个数字，避免改表后两处对不上。
 */
const REFERENCE_INDEX = 1
const REFERENCE_VRAM_GB = TIER_TABLE[REFERENCE_INDEX][0]
/** 该档位在 RTX 5080 16GB 上的实测单镜耗时。 */
const REFERENCE_SECONDS = { draft: 27.0, preview: 120.0, final: 392.0 }

/**
 * 粗估单镜耗时。
 *
 * 仅用于给用户一个数量级预期和排产估算，不是承诺。真实数字要靠标定
 * 在目标机器上实测。扩散模型耗时大致正比于像素数乘步数。
 */
export function estimateSeconds(tier, w, h, steps, vramGb) {
  const [refW, refH, refSteps] = TIER_TABLE[REFERENCE_INDEX][1][tier]
  const refWork = round32(refW) * round32(refH) * refSteps
  let base = REFERENCE_SECONDS[tier] * ((w * h * steps) / refWork)
  // 比基准档更小的机器通常算力也弱，且要频繁换入换出，给一个惩罚系数。
  // 基准档自身不吃惩罚，否则实测值会被凭空放大。
  if (vramGb < REFERENCE_VRAM_GB) base *= 1 + (REFERENCE_VRAM_GB - vramGb) * 0.08
  return Math.round(base * 10) / 10
}

/** 按显存推导三个档位的参数。 */
export function tiersForVram(vramGb) {
  let chosen = TIER_TABLE[TIER_TABLE.length - 1][1]
  let chosenVram = TIER_TABLE[TIER_TABLE.length - 1][0]
  for (const [threshold, table] of TIER_TABLE) {
    if (vramGb >= threshold) {
      chosen = table
      chosenVram = threshold
      break
    }
  }

  const specs = {}
  for (const [tier, [rawW, rawH, steps]] of Object.entries(chosen)) {
    // 统一规整到 32 的倍数。表里手写的数字可能不合规，
    // 而不是 32 的倍数会导致 Wan 的潜空间对不齐。
    const width = round32(rawW)
    const height = round32(rawH)
    specs[tier] = {
      tier,
      width,
      height,
      steps,
      measured_seconds: estimateSeconds(tier, width, height, steps, chosenVram),
    }
  }
  return specs
}

/** 按画幅调整档位的分辨率。 */
export function scaledTo(spec, aspectRatio) {
  const long = Math.max(spec.width, spec.height)
  const short = Math.min(spec.width, spec.height)
  let width
  let height
  if (aspectRatio === '9:16') {
    width = round32(short)
    height = round32(long)
  } else if (aspectRatio === '1:1') {
    width = round32(Math.trunc((spec.width + spec.height) / 2))
    height = width
  } else {
    width = round32(long)
    height = round32(short)
  }
  return { ...spec, width, height }
}

/**
 * 探测本机显卡。
 *
 * 走 nvidia-smi 而不是拉一个 CUDA 绑定进来——这一层只需要一个数字，
 * 而且 ComfyUI 多半在别的机器上，本机探测本来就常常是空的。
 */
export async function detectGpu({ execImpl = defaultExec } = {}) {
  try {
    const out = await execImpl('nvidia-smi', [
      '--query-gpu=name,memory.total',
      '--format=csv,noheader,nounits',
    ])
    const line = String(out).trim().split('\n')[0]
    if (!line) return null
    const [name, mib] = line.split(',').map((x) => x.trim())
    const total = Number(mib)
    if (!name || !Number.isFinite(total)) return null
    return { name, vram_mb: total, vram_gb: total / 1024 }
  } catch {
    return null
  }
}

/**
 * 本机硬件画像。
 *
 * 探测不到显卡时给一个保守假设并标记 detected=false——界面要能说出
 * 「没探测到，按 8GB 算」，而不是让用户以为档位就该这么低。
 */
export async function detectProfile(overrideVramGb = null, opts = {}) {
  const gpu = overrideVramGb ? null : await detectGpu(opts)
  const detected = overrideVramGb !== null || gpu !== null
  const vramGb = overrideVramGb ?? gpu?.vram_gb ?? 8.0
  return { gpu, vram_gb: vramGb, detected, tiers: tiersForVram(vramGb) }
}

/** 一集大概要跑多久。排产估算用，不是承诺。 */
export function estimateEpisode(profile, shotCount, tier) {
  const spec = profile.tiers[tier]
  if (!spec?.measured_seconds) return null
  return spec.measured_seconds * shotCount
}

function defaultExec(exe, args) {
  return new Promise((resolve, reject) => {
    execFile(exe, args, { timeout: 10000, encoding: 'utf8' }, (err, stdout) => {
      if (err) reject(err)
      else resolve(stdout)
    })
  })
}
