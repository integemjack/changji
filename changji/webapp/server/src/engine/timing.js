/**
 * 时长与帧数。
 *
 * 这一整块的存在理由是一次踩过的坑：分镜表里写了 8 秒和 10 秒的镜头，
 * 而视频模型单段实际只能出到 5 秒。超出的部分被静默截断——不报错、
 * 不告警，成片比计划短了一大截，一直到质量闸门那步才发现。
 *
 * 所以时长档位必须由帧数上限推导，不能各写一份。
 */

/**
 * 四舍六入五取偶。
 *
 * Python 的 round 用的是这个规则（round-half-to-even），JS 的 Math.round
 * 是逢五进一。差别只在正好落在 .5 上的时候，但那不是罕见情况：
 * 90 秒的配额算出来 slot 2 是 90×0.10/2 = 4.5，Python 给 4 个镜头，
 * Math.round 给 5 个——同一份分镜要求，两边算出的镜头数不一样。
 *
 * 这条规则本身谈不上更对，但整套东西的档位表、配额、帧数都是按它调出来的，
 * 换掉等于悄悄改了所有已有项目的分镜规格。所以照搬。
 */
export function roundHalfEven(x) {
  const floor = Math.floor(x)
  const diff = x - floor
  if (diff > 0.5) return floor + 1
  if (diff < 0.5) return floor
  return floor % 2 === 0 ? floor : floor + 1
}

/**
 * 单段帧数上限。
 *
 * 超过这个数会在约 100 帧处到达末帧然后往回跑，出现乒乓现象。
 * 这是模型本身的限制，不是可调参数。
 */
export const MAX_FRAMES = 121

/**
 * 单个镜头能生成的最长时长。
 *
 * 分镜的时长档位必须由它推导。早先档位表里有 8 秒和 10 秒，
 * 而实际上限是 5 秒，多出来的部分被静默截断。
 */
export const maxShotDurationS = (fps = 24) => MAX_FRAMES / fps

/**
 * 时长换算帧数。
 *
 * Wan 要求帧数满足 4n+1。超过上限的时长会被截断，
 * 调用方应该先用 maxShotDurationS 把时长限住。
 */
export function framesFor(durationS, fps = 24) {
  const raw = roundHalfEven(durationS * fps)
  const n = Math.max(1, roundHalfEven((raw - 1) / 4))
  return Math.min(4 * n + 1, MAX_FRAMES)
}

/**
 * 视频模型支持的时长档位。分镜必须落在这些值上，自由时长没法生成。
 *
 * 上限由 MAX_FRAMES 推导，不能自己写死。
 */
const ALL_SLOTS = [2, 3, 4, 5, 8, 10]

export const DURATION_SLOTS = (() => {
  const usable = ALL_SLOTS.filter((s) => s <= maxShotDurationS())
  return usable.length ? usable : [2]
})()

/** 把任意时长吸附到最近的可生成档位。 */
export function snapDuration(seconds) {
  return DURATION_SLOTS.reduce((best, s) =>
    Math.abs(s - seconds) < Math.abs(best - seconds) ? s : best,
  )
}

/** 向上吸附。配音时长反推镜头时长时用，宁长勿短。 */
export function ceilDuration(seconds) {
  for (const slot of DURATION_SLOTS) {
    if (slot >= seconds - 1e-6) return slot
  }
  return DURATION_SLOTS[DURATION_SLOTS.length - 1]
}

/**
 * 时长配额。先定骨架再填内容，比让模型自己算总时长可靠得多。
 *
 * 大模型在几十个镜头规模上做算术很不可靠——让它自己凑总时长，
 * 出来的分镜表加起来能差出一倍。
 */
export class DurationQuota {
  constructor(slots) {
    this.slots = slots
  }

  get totalS() {
    return Object.entries(this.slots).reduce((a, [d, n]) => a + Number(d) * n, 0)
  }

  get shotCount() {
    return Object.values(this.slots).reduce((a, n) => a + n, 0)
  }

  describe() {
    return Object.entries(this.slots)
      .map(([d, n]) => [Number(d), n])
      .filter(([, n]) => n)
      .sort((a, b) => a[0] - b[0])
      .map(([d, n]) => `${n} 个 ${d} 秒镜头`)
      .join('，')
  }

  /**
   * 按目标时长分配镜头。
   *
   * 节奏上短镜头占多数，长镜头留给情绪戏。全用同一时长会平铺直叙。
   */
  static forDuration(targetS) {
    if (targetS <= 0) throw new Error('目标时长必须大于 0')

    // 分配比例。只用真正可生成的档位，长镜头多分一点时长，
    // 短镜头多分一点数量，这样节奏有变化而不是平铺。
    const weights = { 2: 0.1, 3: 0.22, 4: 0.18, 5: 0.5 }
    let plan = Object.fromEntries(
      Object.entries(weights).filter(([d]) => DURATION_SLOTS.includes(Number(d))),
    )
    if (!Object.keys(plan).length) {
      plan = { [DURATION_SLOTS[DURATION_SLOTS.length - 1]]: 1 }
    }
    const scale = 1 / Object.values(plan).reduce((a, b) => a + b, 0)

    const slots = {}
    for (const [d, share] of Object.entries(plan)) {
      slots[d] = Math.max(0, roundHalfEven((targetS * share * scale) / Number(d)))
    }

    // 用最长的档位补足或削减差额
    const pad = DURATION_SLOTS[DURATION_SLOTS.length - 1]
    slots[pad] = Math.max(1, slots[pad] ?? 0)
    const sum = Object.entries(slots).reduce((a, [d, n]) => a + Number(d) * n, 0)
    slots[pad] = Math.max(1, slots[pad] + roundHalfEven((targetS - sum) / pad))

    return new DurationQuota(
      Object.fromEntries(Object.entries(slots).filter(([, n]) => n > 0)),
    )
  }
}

/**
 * 把总时长拉回目标值。
 *
 * 偏差优先摊到无对白的过渡镜上，有台词的镜头不动——它们的时长是由配音
 * 定的，动了就音画对不上。
 */
export function rebalanceDurations(shots, targetS, toleranceS = 3) {
  const current = shots.reduce((a, s) => a + s.duration_s, 0)
  if (Math.abs(current - targetS) <= toleranceS) return shots

  const adjustable = shots.filter((s) => !s.dialogue.length && !s.duration_locked)
  if (!adjustable.length) return shots

  let diff = targetS - current
  const step = diff > 0 ? 1 : -1
  let guard = 0
  while (Math.abs(diff) > toleranceS && guard < 500) {
    guard += 1
    let moved = false
    for (const shot of adjustable) {
      // 先吸附再查表。时长可能不在档位表里：老项目升级、用户手改分镜、
      // 或者档位表本身变过。直接查会拿到 -1，把这一步算歪。
      const idx = DURATION_SLOTS.indexOf(snapDuration(shot.duration_s))
      const nextIdx = idx + step
      if (nextIdx < 0 || nextIdx >= DURATION_SLOTS.length) continue
      const delta = DURATION_SLOTS[nextIdx] - shot.duration_s
      if (Math.abs(diff - delta) < Math.abs(diff)) {
        shot.duration_s = DURATION_SLOTS[nextIdx]
        diff -= delta
        moved = true
      }
      if (Math.abs(diff) <= toleranceS) break
    }
    if (!moved) break
  }
  return shots
}
