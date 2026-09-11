import { describe, it, expect } from 'vitest'
import { describeRoomDecision } from './room-decision'

describe('腾显存的判断翻成人话', () => {
  it('还没判过就什么都不说，别硬凑一句', () => {
    expect(describeRoomDecision(null)).toBe(null)
    expect(describeRoomDecision(undefined)).toBe(null)
  })

  it('压根没判（模型本来就装着）：不能借用"够、没动"那套措辞', () => {
    // 那条记录里「当时空闲」是 0、「问来的」是假，照"够"那套渲染的话
    // 界面上会显示「问不到卡」——而那是让用户盯着报警的那一项。
    const r = describeRoomDecision({
      slot: '出片', alreadyLoaded: true, kept: true, evicted: 0,
      liveGb: 74, liveMeasured: true, freeSeenGb: null, probed: false,
    })
    expect(r.skipped).toBe(true)
    expect(r.verdict).toBe('模型本来就装着，没动别的')
    expect(r.how).toBeUndefined()      // 不给「问不到卡」留任何入口
    expect(r.free).toBeUndefined()
  })

  it('够、没动：把依据一并说清', () => {
    const r = describeRoomDecision({
      slot: '出片', kept: true, evicted: 0,
      liveGb: 74.6, liveMeasured: true, freeSeenGb: 78.7, probed: true,
    })
    expect(r.skipped).toBe(false)
    expect(r.verdict).toBe('够，没动别的模型')
    expect(r.need).toBe('74.6 GB')
    expect(r.needHow).toBe('量出来的')
    expect(r.needTrusted).toBe(true)
    expect(r.free).toBe('78.7 GB')
    expect(r.how).toBe('问显卡问来的')
  })

  it('判"够"却是拿估算判的：必须说出来', () => {
    // 出片这一路的估算被实测推翻过两次，都是往小了错五倍。
    const r = describeRoomDecision({
      slot: '出片', kept: true, evicted: 0,
      liveGb: 14.6, liveMeasured: false, freeSeenGb: 80, probed: true,
    })
    expect(r.needHow).toBe('估的，这个槽还没量过')
    expect(r.needTrusted).toBe(false)
  })

  it('卸了：说卸了几个', () => {
    const r = describeRoomDecision({
      slot: '出片', kept: false, evicted: 2,
      liveGb: 90, liveMeasured: true, freeSeenGb: 40, probed: true,
    })
    expect(r.verdict).toBe('不够，卸了 2 个')
    expect(r.ok).toBe(false)
  })

  it('空闲是推算的而不是问来的：这一条要能看出来', () => {
    const r = describeRoomDecision({
      slot: '出片', kept: true, evicted: 0,
      liveGb: 74, liveMeasured: true, freeSeenGb: 81, probed: false,
    })
    expect(r.how).toBe('问不到卡，按量到/估到的推算')
  })

  it('拿不到的数说"不知道"，不显示 NaN', () => {
    const r = describeRoomDecision({
      slot: '出片', kept: true, evicted: 0,
      liveGb: null, liveMeasured: false, freeSeenGb: undefined, probed: false,
    })
    expect(r.need).toBe('不知道')
    expect(r.free).toBe('不知道')
  })
})
