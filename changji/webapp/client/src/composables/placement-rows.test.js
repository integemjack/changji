import { describe, it, expect } from 'vitest'
import { placementRows } from './placement-rows'

describe('权重放哪那两行', () => {
  it('老引擎没有这一项：整块不显示，不拿默认值凑一行', () => {
    expect(placementRows(null)).toEqual([])
    expect(placementRows(undefined)).toEqual([])
  })

  it('哪一路没给 weights，哪一路就不显示', () => {
    // 接口里少了字段不等于"放内存"——凑一行出来是在替引擎编答案。
    const rows = placementRows({ video: { weights: 'cpu' }, image: {} })
    expect(rows.map((r) => r.key)).toEqual(['video'])
  })

  it('估算和实测并排给出来', () => {
    // 出片这一路估 14.6、实测 74，差五倍，而判断用的是实测那个。
    const rows = placementRows({
      video: {
        weights: 'cpu', modelGb: 18.8, liveVramGb: 14.6,
        measuredVramGb: 74.6, measuredWork: 2560 * 1440 * 81,
      },
    })
    expect(rows[0].liveVramGb).toBe(14.6)
    expect(rows[0].measured).toBe(74.6)
    expect(rows[0].measuredWorkMp).toBe(299)   // 像素×帧 换成百万
  })

  it('没量过给 null，绝不写 0', () => {
    // 写 0 会被读成"量过、占 0 GB"，那是完全另一回事。
    const rows = placementRows({ video: { weights: 'cpu', liveVramGb: 14.6 } })
    expect(rows[0].measured).toBe(null)
    expect(rows[0].measuredWorkMp).toBe(null)
  })

  it('量了但没记画幅（老持久化文件）：不报 0 MP·帧', () => {
    // work = 0 的数罩不住任何指定了大小的一镜，报个 0 只会让人困惑。
    const rows = placementRows({
      video: { weights: 'cpu', measuredVramGb: 74, measuredWork: 0 },
    })
    expect(rows[0].measured).toBe(74)
    expect(rows[0].measuredWorkMp).toBe(null)
  })

  it('两路都有就按出首帧、出片的顺序', () => {
    const rows = placementRows({
      image: { weights: 'te=cpu,vae=cpu' },
      video: { weights: 'cpu' },
    })
    expect(rows.map((r) => r.label)).toEqual(['出首帧', '出片'])
  })
})
