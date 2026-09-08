/**
 * 投递目标的校验。
 *
 * 重点是那条新加的"平台名认不认得"。
 */

import { describe, expect, it } from 'vitest'
import { PLATFORMS, validateTarget } from './publish.js'

const IDS = PLATFORMS.map((p) => p.id)
const ok = { name: '我的抖音号', platform: 'douyin', exportDir: 'D:/out' }

describe('validateTarget', () => {
  it('填齐了就放行', () => {
    expect(validateTarget(ok, IDS)).toBe('')
  })

  it('名字和平台是必填', () => {
    expect(validateTarget({ ...ok, name: '' }, IDS)).toMatch(/名字和平台/)
    expect(validateTarget({ ...ok, platform: '' }, IDS)).toMatch(/名字和平台/)
    expect(validateTarget(undefined, IDS)).toMatch(/名字和平台/)
  })

  it('认不出的平台要当场拒，不能收下', () => {
    // **这是这次加的。** 发布页的发片自检是
    //   platforms.find((p) => p.id === target.platform) ?? null
    // 认不出就得到 null，于是时长上限和竖屏比例**一条都不查，
    // 安安静静什么都不说**。用户以为自检过了，投出去才被平台退回来——
    // 而自检存在的全部意义就是避免这件事。
    const msg = validateTarget({ ...ok, platform: 'douyinn' }, IDS)
    expect(msg).toMatch(/认不出这个平台/)
    // 报错要把可选项列出来，否则用户不知道该填什么。
    expect(msg).toContain('douyin')
  })

  it('投递目录和 webhook 至少要有一个', () => {
    expect(validateTarget({ name: 'x', platform: 'douyin' }, IDS))
      .toMatch(/至少要填一个/)
    expect(validateTarget({ name: 'x', platform: 'douyin', webhookUrl: 'https://h/w' }, IDS))
      .toBe('')
  })

  it('webhook 要带 http(s) 前缀', () => {
    expect(validateTarget({ name: 'x', platform: 'douyin', webhookUrl: 'h/w' }, IDS))
      .toMatch(/http:\/\/ 或 https:\/\//)
    expect(validateTarget({ name: 'x', platform: 'douyin', webhookUrl: 'ftp://h/w' }, IDS))
      .toMatch(/http:\/\/ 或 https:\/\//)
  })

  it('每个平台都有 id 和名字，custom 不设上限', () => {
    for (const p of PLATFORMS) {
      expect(p.id).toBeTruthy()
      expect(p.name).toBeTruthy()
    }
    // custom 是"自定义"，不该拿别人的上限去卡它。
    const custom = PLATFORMS.find((p) => p.id === 'custom')
    expect(custom.maxDurationS).toBe(0)
    // 而它必须在白名单里，否则上面那条新校验会把自定义目标也拒掉。
    expect(IDS).toContain('custom')
  })
})
