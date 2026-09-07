// 配音时长与质量闸门。
//
// 配音这边：这一步在生成任何画面之前跑完，拿到每句台词的真实时长反推
// 锁定镜头时长——音画对齐从源头解决，而不是等成片后再把声音塞进去。
// 时长相关的算法和 Python 版做过 182 个用例的逐一比对。
//
// 闸门这边：无人值守时它是唯一阻止废片流入成片的机制。判定必须程序可算，
// 失败必须给出可操作的下一步，绝不静默放行。

import { describe, expect, it } from 'vitest'

import { GateConfigSchema } from '../src/engine/config.js'
import { ShotSchema } from '../src/engine/models/shot.js'
import {
  CHARS_PER_SECOND,
  TAIL_S,
  estimateSpeechDuration,
  freeShotId,
  groupLines,
  lockDuration,
  maxLineSeconds,
  splitLongText,
} from '../src/engine/stages/audio.js'
import {
  Verdict,
  decideNext,
  describeGate,
  gateAudioSync,
  gateVideo,
  looksBlank,
  looksClipped,
  summarizeGates,
} from '../src/engine/gates.js'

const shot = (over = {}) =>
  ShotSchema.parse({ shot_id: 'ep01_sh001', scene_id: 's1', order: 0, ...over })

const withLines = (texts, durations = null) =>
  shot({
    characters: [{ char_id: 'c_a' }],
    dialogue: texts.map((t, i) => ({
      char_id: 'c_a',
      text: t,
      actual_duration_s: durations ? durations[i] : null,
    })),
  })

// ---------------- 配音时长 ----------------

describe('时长估算', () => {
  it('按字数加标点停顿', () => {
    // 标点不发音但产生停顿，逗号短一点、句号问号长一点
    const plain = estimateSpeechDuration('我走了')
    expect(plain).toBeCloseTo(3 / CHARS_PER_SECOND + 0.15 + 0.25, 6)
    expect(estimateSpeechDuration('我走了。')).toBeGreaterThan(plain)
    expect(estimateSpeechDuration('我走了？')).toBeGreaterThan(
      estimateSpeechDuration('我走了，'),
    )
  })

  it('空串是零', () => {
    expect(estimateSpeechDuration('')).toBe(0)
    expect(estimateSpeechDuration('   ')).toBe(0)
  })
})

describe('超长台词切分', () => {
  it('装得下就不切', () => {
    expect(splitLongText('我走了', maxLineSeconds())).toEqual(['我走了'])
  })

  it('优先在句末标点切', () => {
    // 超过单镜上限的台词，配出来的音频装不进任何镜头，
    // 混音时会盖到下一镜上去，成片里两个人同时说话
    const long = '第一句话说完了。第二句话也说完了。第三句话还在继续说。第四句结束了。'
    const parts = splitLongText(long, maxLineSeconds())
    expect(parts.length).toBeGreaterThan(1)
    for (const p of parts) {
      expect(estimateSpeechDuration(p)).toBeLessThanOrEqual(maxLineSeconds() + 1e-9)
    }
  })

  it('没有标点就硬切，但不丢字', () => {
    const long = '啊'.repeat(120)
    const parts = splitLongText(long, maxLineSeconds())
    expect(parts.join('')).toBe(long)
  })
})

describe('时长反推锁定', () => {
  it('有台词的镜头按配音时长向上吸附并锁定', () => {
    // 宁长勿短：短了会截断台词，长了尾巴上留一点表演余韵反而自然
    const s = withLines(['我走了'], [2.6])
    const locked = lockDuration(s, 2.6)
    expect(locked).toBe(3)
    expect(s.duration_locked).toBe(true)
  })

  it('锁定时把尾巴的留白算进去', () => {
    // 2.9 + 0.25 = 3.15，吸到 4 而不是 3
    const s = withLines(['x'], [2.9])
    expect(lockDuration(s, 2.9)).toBe(4)
    expect(2.9 + TAIL_S).toBeGreaterThan(3)
  })

  it('没有台词的镜头不动，它们是节奏调节的余量', () => {
    const s = shot({ duration_s: 5 })
    expect(lockDuration(s, 0)).toBe(5)
    expect(s.duration_locked).toBe(false)
  })
})

describe('台词打包', () => {
  it('一句话不拆', () => {
    expect(groupLines(withLines(['一句话']), maxLineSeconds()).length).toBe(1)
  })

  it('装不下就分包', () => {
    // 模型不知道单段视频只能出 5 秒，一个镜头塞三句长台词是常事
    const groups = groupLines(
      withLines(['第一句话在这里', '第二句话也在这里', '第三句话还在这里']),
      maxLineSeconds(),
    )
    expect(groups.length).toBeGreaterThan(1)
    expect(groups.flat().length).toBe(3) // 一句都没丢
  })

  it('有真实时长就用真实的', () => {
    const groups = groupLines(withLines(['a', 'b'], [4.5, 4.5]), maxLineSeconds())
    expect(groups.length).toBe(2)
  })
})

describe('拆出来的镜头编号', () => {
  it('依次用字母后缀', () => {
    expect(freeShotId('ep01_sh001', new Set())).toBe('ep01_sh001_b')
    expect(freeShotId('ep01_sh001', new Set(['ep01_sh001_b']))).toBe('ep01_sh001_c')
  })

  it('字母用完了退回数字', () => {
    const used = new Set('bcdefghijklmnopqrstuvwxyz'.split('').map((c) => `a_${c}`))
    expect(freeShotId('a', used)).toBe('a_2')
  })
})

// ---------------- 质量闸门 ----------------

const config = GateConfigSchema.parse({})
const info = (over = {}) => ({ has_video: true, duration_s: 5, width: 704, height: 1280, ...over })
const sample = (over = {}) => ({ mean: 120, spread: 55, ...over })

describe('画面判定', () => {
  it('纯色画面认得出来', () => {
    // 生成失败最常见的表现。展布用百分位之差而不是极差——
    // 一张几乎全黑但有一个高光点的废图，极差能到 250 看着很正常
    expect(looksBlank({ spread: 3 })).toBe(true)
    expect(looksBlank({ spread: 40 })).toBe(false)
  })

  it('全黑和过曝认得出来', () => {
    expect(looksClipped({ mean: 2 })).toBe(true)
    expect(looksClipped({ mean: 252 })).toBe(true)
    expect(looksClipped({ mean: 120 })).toBe(false)
  })
})

describe('画面闸门', () => {
  it('正常画面通过', () => {
    const r = gateVideo(shot(), { info: info(), samples: [sample(), sample(), sample()], config })
    expect(r.verdict).toBe(Verdict.PASS)
    expect(r.metrics.spread_min).toBe(55)
  })

  it('纯色画面拦下并说清是片头还是片尾', () => {
    const r = gateVideo(shot(), {
      info: info(),
      samples: [sample(), sample(), sample({ spread: 2 })],
      config,
    })
    expect(r.verdict).toBe(Verdict.RETRY)
    expect(r.reasons[0]).toContain('片尾')
  })

  it('中途亮度剧烈跳变算崩坏', () => {
    const r = gateVideo(shot(), {
      info: info(),
      samples: [sample({ mean: 30 }), sample({ mean: 200 }), sample({ mean: 40 })],
      config,
    })
    expect(r.reasons.some((x) => x.includes('剧烈跳变'))).toBe(true)
  })

  it('时长差太多是退回不是重试', () => {
    // 帧数算错了，换个种子重跑还是一样
    const r = gateVideo(shot(), {
      info: info({ duration_s: 2 }),
      samples: [sample()],
      config,
      expectedDurationS: 5,
    })
    expect(r.verdict).toBe(Verdict.REGRESS)
    expect(r.reasons[0]).toContain('帧数算错')
  })

  it('分辨率不符说明档位参数没生效', () => {
    const r = gateVideo(shot(), {
      info: info({ width: 480, height: 854 }),
      samples: [sample()],
      config,
      expectedSize: [704, 1280],
    })
    expect(r.verdict).toBe(Verdict.REGRESS)
    expect(r.reasons[0]).toContain('档位参数没生效')
  })

  it('没有视频轨要重试', () => {
    const r = gateVideo(shot(), { info: info({ has_video: false }), samples: [], config })
    expect(r.verdict).toBe(Verdict.RETRY)
  })

  it('一帧都取不到要重试', () => {
    const r = gateVideo(shot(), { info: info(), samples: [], config })
    expect(r.reasons[0]).toContain('取不到任何画面')
  })
})

describe('音画闸门', () => {
  it('装得下就通过', () => {
    const r = gateAudioSync(shot(), { info: info({ duration_s: 5 }), config, speechS: 3 })
    expect(r.verdict).toBe(Verdict.PASS)
    expect(r.metrics.slack_s).toBe(2)
  })

  it('装不下是退回，要重新锁时长', () => {
    // 装完再发现装不下就得重做整集，所以这个检查放在装配之前
    const r = gateAudioSync(shot(), { info: info({ duration_s: 3 }), config, speechS: 5 })
    expect(r.verdict).toBe(Verdict.REGRESS)
    expect(r.reasons[0]).toContain('重新锁定时长')
  })

  it('容差之内不算超', () => {
    const r = gateAudioSync(shot(), { info: info({ duration_s: 5 }), config, speechS: 5.1 })
    expect(r.verdict).toBe(Verdict.PASS)
  })

  it('有台词却没配音时长说明配音没跑完', () => {
    const r = gateAudioSync(shot(), { info: info(), config, speechS: null })
    expect(r.verdict).toBe(Verdict.REGRESS)
    expect(r.reasons[0]).toContain('配音阶段没跑完')
  })
})

describe('失败之后怎么办', () => {
  // 这是无人值守能不能不卡死的关键
  const failed = { ok: false, verdict: Verdict.RETRY }

  it('还有重试次数就重试', () => {
    expect(decideNext(failed, shot({ attempts: 0 }), config)).toBe(Verdict.RETRY)
  })

  it('重试超限就降级，保证整集能出片', () => {
    expect(decideNext(failed, shot({ attempts: 2 }), config)).toBe(Verdict.FALLBACK)
  })

  it('关了降级就退回，让人来看', () => {
    const strict = GateConfigSchema.parse({ fallback_on_exhausted: false })
    expect(decideNext(failed, shot({ attempts: 2 }), strict)).toBe(Verdict.REGRESS)
  })

  it('退回类的失败不消耗重试次数', () => {
    // 重跑也不会变好的事，重试三次只是浪费半小时
    const regress = { ok: false, verdict: Verdict.REGRESS }
    expect(decideNext(regress, shot({ attempts: 0 }), config)).toBe(Verdict.REGRESS)
  })
})

describe('概览', () => {
  it('只列没通过的，那是人唯一要看的', () => {
    const results = [
      { shot_id: 'a', ok: true, verdict: Verdict.PASS, gate: '画面闸门', reasons: [] },
      { shot_id: 'b', ok: false, verdict: Verdict.RETRY, gate: '画面闸门', reasons: ['近乎纯色'] },
    ]
    const text = summarizeGates(results)
    expect(text).toContain('通过 1 个')
    expect(text).toContain('b 未过画面闸门：近乎纯色')
    expect(text).not.toContain('a 通过')
  })

  it('没镜头时说清楚', () => {
    expect(summarizeGates([])).toBe('没有需要检查的镜头')
  })

  it('单条描述给人看', () => {
    expect(
      describeGate({ shot_id: 'a', ok: false, gate: '画面闸门', reasons: ['太暗', '太糊'] }),
    ).toBe('a 未过画面闸门：太暗；太糊')
  })
})
