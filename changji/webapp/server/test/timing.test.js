// 时长与帧数。
//
// 这一组钉的是一次踩过的坑：分镜表里写了 8 秒和 10 秒的镜头，而视频模型
// 单段实际只能出到 5 秒，超出部分被静默截断——不报错、不告警，成片比计划
// 短一大截。所以档位必须由帧数上限推导，不能各写一份。
//
// 全部数值都和 Python 版做过 1021 个用例的逐一比对，一致后才落成测试。

import { describe, expect, it } from 'vitest'

import {
  DURATION_SLOTS,
  DurationQuota,
  MAX_FRAMES,
  ceilDuration,
  framesFor,
  maxShotDurationS,
  rebalanceDurations,
  roundHalfEven,
  snapDuration,
} from '../src/engine/timing.js'
import { ShotSchema } from '../src/engine/models/shot.js'

describe('四舍六入五取偶', () => {
  it('正好在半整数上取偶数', () => {
    // Python 的 round 是这个规则，Math.round 不是。差别只在 .5 上，
    // 但 90 秒配额正好算出 4.5——两边会给出不同的镜头数
    expect(roundHalfEven(4.5)).toBe(4)
    expect(roundHalfEven(5.5)).toBe(6)
    expect(roundHalfEven(0.5)).toBe(0)
    expect(roundHalfEven(1.5)).toBe(2)
  })

  it('不在半整数上就是普通四舍五入', () => {
    expect(roundHalfEven(4.4)).toBe(4)
    expect(roundHalfEven(4.6)).toBe(5)
    expect(roundHalfEven(-1.2)).toBe(-1)
  })
})

describe('帧数', () => {
  it('档位上限由帧数上限推导', () => {
    expect(maxShotDurationS()).toBeCloseTo(MAX_FRAMES / 24, 6)
    // 5 秒能出，8 秒和 10 秒出不了，所以档位表里不该有它们
    expect(DURATION_SLOTS).toEqual([2, 3, 4, 5])
  })

  it('帧数满足 4n+1', () => {
    for (const d of [1, 2, 3, 4, 5, 2.5, 3.7]) {
      expect((framesFor(d) - 1) % 4).toBe(0)
    }
  })

  it('超过上限会被截断', () => {
    expect(framesFor(10)).toBe(MAX_FRAMES)
    expect(framesFor(100)).toBe(MAX_FRAMES)
  })

  it('至少给一帧以上', () => {
    expect(framesFor(0.01)).toBe(5)
  })
})

describe('档位吸附', () => {
  it('吸到最近的档位', () => {
    expect(snapDuration(2.4)).toBe(2)
    expect(snapDuration(3.6)).toBe(4)
    expect(snapDuration(100)).toBe(5)
  })

  it('向上吸附宁长勿短', () => {
    // 配音时长反推镜头时长时用。短了会把话截掉
    expect(ceilDuration(2.1)).toBe(3)
    expect(ceilDuration(3)).toBe(3)
    expect(ceilDuration(0.5)).toBe(2)
    expect(ceilDuration(9)).toBe(5) // 超上限就给最长的
  })
})

describe('时长配额', () => {
  it('总时长接近目标', () => {
    for (const target of [30, 60, 90, 180]) {
      const quota = DurationQuota.forDuration(target)
      expect(Math.abs(quota.totalS - target)).toBeLessThanOrEqual(5)
    }
  })

  it('只用能生成的档位', () => {
    const quota = DurationQuota.forDuration(120)
    for (const d of Object.keys(quota.slots)) {
      expect(DURATION_SLOTS).toContain(Number(d))
    }
  })

  it('短镜头多长镜头少，节奏才有变化', () => {
    const quota = DurationQuota.forDuration(180)
    expect(quota.shotCount).toBeGreaterThan(180 / 5)
  })

  it('90 秒那一档保持和 Python 版一致', () => {
    // 这一条专门盯着 4.5 那个半整数。用 Math.round 会变成 5
    expect(DurationQuota.forDuration(90).slots).toEqual({ 2: 4, 3: 7, 4: 4, 5: 9 })
  })

  it('目标时长必须大于零', () => {
    expect(() => DurationQuota.forDuration(0)).toThrow()
  })

  it('描述给人看的', () => {
    expect(DurationQuota.forDuration(30).describe()).toMatch(/个 \d 秒镜头/)
  })
})

describe('总时长再平衡', () => {
  const shot = (over) =>
    ShotSchema.parse({ shot_id: 'a', scene_id: 's', order: 0, ...over })

  it('偏差摊到无对白的过渡镜上', () => {
    const shots = [
      shot({ shot_id: 'a', duration_s: 2 }),
      shot({ shot_id: 'b', order: 1, duration_s: 2 }),
      shot({ shot_id: 'c', order: 2, duration_s: 2 }),
    ]
    rebalanceDurations(shots, 15, 1)
    expect(shots.reduce((a, s) => a + s.duration_s, 0)).toBeGreaterThan(6)
  })

  it('有台词的镜头不动', () => {
    // 它们的时长是由配音定的，动了就音画对不上
    const spoken = shot({
      shot_id: 'a',
      duration_s: 2,
      characters: [{ char_id: 'c_a' }],
      dialogue: [{ char_id: 'c_a', text: '我走了' }],
    })
    rebalanceDurations([spoken], 30, 1)
    expect(spoken.duration_s).toBe(2)
  })

  it('锁定时长的镜头不动', () => {
    const locked = shot({ duration_s: 2, duration_locked: true })
    rebalanceDurations([locked], 30, 1)
    expect(locked.duration_s).toBe(2)
  })

  it('已经在容差内就原样返回', () => {
    const shots = [shot({ duration_s: 5 })]
    rebalanceDurations(shots, 5.5, 3)
    expect(shots[0].duration_s).toBe(5)
  })

  it('不会因为时长不在档位表里就崩掉', () => {
    // 老项目升级、用户手改分镜、档位表本身变过，都会出现这种值
    const odd = shot({ duration_s: 7.3 })
    expect(() => rebalanceDurations([odd], 20, 1)).not.toThrow()
  })
})
