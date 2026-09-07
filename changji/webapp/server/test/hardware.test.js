// 硬件画像与画质档位。
//
// 档位不写死在配置里，由显存推导——写死等于把这台机器的显存刻进项目，
// 换台机器就不对了。和 Python 版做过 44 个用例的逐一比对。

import { describe, expect, it } from 'vitest'

import {
  Tier,
  detectGpu,
  detectProfile,
  estimateEpisode,
  estimateSeconds,
  round32,
  scaledTo,
  tiersForVram,
} from '../src/engine/hardware.js'

describe('32 的倍数', () => {
  it('规整到最近的 32 倍', () => {
    // 不是 32 的倍数会让 Wan 的潜空间对不齐
    expect(round32(432)).toBe(448)
    expect(round32(431)).toBe(448)
    expect(round32(1088)).toBe(1088)
  })

  it('正好在半数上取偶，和 Python 一致', () => {
    // 80/32 = 2.5：Python 的 round 给 2（取偶），Math.round 给 3。
    // 差一档就是 64 和 96，而分辨率不符会被闸门判成「档位参数没生效」，
    // 退回重跑，永远跑不出来
    expect(round32(80)).toBe(64)
    expect(round32(112)).toBe(96)
    expect(round32(144)).toBe(128)
  })

  it('再小也不低于 32', () => {
    expect(round32(1)).toBe(32)
    expect(round32(0)).toBe(32)
  })
})

describe('按显存推档位', () => {
  it('16G 卡拿 16G 档而不是 12G 档', () => {
    // 阈值比标称低 0.5：驱动和固件占掉一部分，nvidia-smi 报出来的
    // 永远小于标称值。一张 16GB 的卡通常报 15.9
    const specs = tiersForVram(15.9)
    expect([specs.final.width, specs.final.height]).toEqual([1280, 704])
  })

  it('显存越小档位越保守', () => {
    const big = tiersForVram(24)
    const small = tiersForVram(8)
    expect(big.final.width).toBeGreaterThan(small.final.width)
    expect(big.final.steps).toBeGreaterThanOrEqual(small.final.steps)
  })

  it('草稿档一定比成片档便宜', () => {
    // 分级生成的全部意义在这儿：叙事和构图在草稿档判完，过了闸门才升级
    for (const vram of [8, 12, 16, 24]) {
      const s = tiersForVram(vram)
      expect(s.draft.width * s.draft.height * s.draft.steps).toBeLessThan(
        s.final.width * s.final.height * s.final.steps,
      )
      expect(s.draft.measured_seconds).toBeLessThan(s.final.measured_seconds)
    }
  })

  it('超小显存不会崩，落到最低档', () => {
    const specs = tiersForVram(2)
    expect(specs.draft.width).toBe(448)
  })

  it('分辨率都是 32 的倍数', () => {
    for (const vram of [4, 8, 12, 16, 24, 80]) {
      for (const spec of Object.values(tiersForVram(vram))) {
        expect(spec.width % 32).toBe(0)
        expect(spec.height % 32).toBe(0)
      }
    }
  })
})

describe('画幅换算', () => {
  const base = tiersForVram(15.9)

  it('竖屏时长边在下', () => {
    const s = scaledTo(base.final, '9:16')
    expect(s.height).toBeGreaterThan(s.width)
    expect(s.width % 32).toBe(0)
    expect(s.height % 32).toBe(0)
  })

  it('横屏时长边在右', () => {
    const s = scaledTo(base.final, '16:9')
    expect(s.width).toBeGreaterThan(s.height)
  })

  it('方形取中间值', () => {
    const s = scaledTo(base.final, '1:1')
    expect(s.width).toBe(s.height)
    expect(s.width % 32).toBe(0)
  })
})

describe('耗时估算', () => {
  it('基准档不吃惩罚系数', () => {
    // 基准档的数字是实测值，加惩罚等于把实测值凭空放大
    const specs = tiersForVram(15.9)
    expect(specs.draft.measured_seconds).toBe(27)
    expect(specs.final.measured_seconds).toBe(392)
  })

  it('比基准小的机器有惩罚', () => {
    // 算力弱且要频繁换入换出
    const small = estimateSeconds(Tier.DRAFT, 640, 352, 10, 7.5)
    const ref = estimateSeconds(Tier.DRAFT, 640, 352, 10, 15.5)
    expect(small).toBeGreaterThan(ref)
  })

  it('一集的估算是单镜乘镜头数', () => {
    const profile = { vram_gb: 16, tiers: tiersForVram(16) }
    expect(estimateEpisode(profile, 8, Tier.FINAL)).toBe(392 * 8)
  })

  it('没有实测值时给 null 而不是猜', () => {
    expect(estimateEpisode({ tiers: {} }, 8, Tier.FINAL)).toBeNull()
  })
})

describe('探测', () => {
  it('探不到显卡时给保守假设并标记未探测', async () => {
    // 界面要能说出「没探测到，按 8GB 算」，而不是让用户
    // 以为档位本来就该这么低
    const profile = await detectProfile(null, {
      execImpl: async () => {
        throw new Error('nvidia-smi 不存在')
      },
    })
    expect(profile.detected).toBe(false)
    expect(profile.vram_gb).toBe(8)
    expect(profile.tiers.draft).toBeTruthy()
  })

  it('手动覆盖显存时不去探测', async () => {
    // ComfyUI 在别的机器上时本机探测不到，配置里顶上
    let called = false
    const profile = await detectProfile(24, {
      execImpl: async () => {
        called = true
        return ''
      },
    })
    expect(called).toBe(false)
    expect(profile.detected).toBe(true)
    expect(profile.vram_gb).toBe(24)
    expect(profile.tiers.final.width).toBe(1920)
  })

  it('解析 nvidia-smi 的输出', async () => {
    const gpu = await detectGpu({ execImpl: async () => 'NVIDIA GeForce RTX 5080, 16303\n' })
    expect(gpu.name).toBe('NVIDIA GeForce RTX 5080')
    expect(gpu.vram_gb).toBeCloseTo(15.92, 2)
  })

  it('输出不成样子时当探测失败', async () => {
    expect(await detectGpu({ execImpl: async () => '' })).toBeNull()
    expect(await detectGpu({ execImpl: async () => 'garbage' })).toBeNull()
  })
})
