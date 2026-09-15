/**
 * 三个画质档的**真实像素尺寸**，和引擎那份对不对得上。
 *
 * **为什么值得单写一条用例。** 这六个数是**抄过来的**：真身在
 * `cpp/src/config/settings.cpp` 的 `VideoConfig::size()`。而引擎那边的注释
 * 自己记着它已经改过两回——
 *
 *   · 标准档从 704×1280 换成 544×928（2026-09-10，像素数降 44%）；
 *   · hd 档 2026-09-11 又加回来，就是原来那个 704×1280。
 *
 * 抄的一方落后，症状是**界面上写着一个你拿不到的分辨率**——而这几个数
 * 出现的地方正是"选哪一档"的那个下拉框（ShowDialog）和项目页那一行摘要。
 * 人是照着这个数做的选择。引擎那边的注释也把话说在这儿了：「名字仍然叫
 * "720p"……**界面上跟着显示真实尺寸，不靠这个名字**」——那句话成立的前提
 * 就是这六个数跟得上。
 *
 * 做法同 `speech-rate.test.js`：直接读那份 C++ 源码比。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

import { VIDEO_QUALITIES, qualitySize } from './labels.js'

const CPP = fileURLToPath(new URL('../../../../cpp/src/config/settings.cpp', import.meta.url))

/**
 * 把 `VideoConfig::size()` 里那三档的长短边读出来。
 *
 * 形状是：先给默认值（720p），再在 if/else if 里覆盖 2k 和 hd。
 */
function engineSizes() {
  const src = fs.readFileSync(CPP, 'utf8')
  const at = src.indexOf('std::pair<int, int> VideoConfig::size() const {')
  if (at < 0) throw new Error('settings.cpp 里找不到 VideoConfig::size()')
  const body = src.slice(at, src.indexOf('\n}', at))
  const num = (re) => {
    const m = body.match(re)
    if (!m) throw new Error(`VideoConfig::size() 的形状变了，取不到：${re}`)
    return Number(m[1])
  }
  return {
    '720p': { long: num(/int long_side = (\d+);/), short: num(/int short_side = (\d+);/) },
    '2k': {
      long: num(/quality == "2k"\)[\s\S]*?long_side = (\d+);/),
      short: num(/quality == "2k"\)[\s\S]*?short_side = (\d+);/),
    },
    hd: {
      long: num(/quality == "hd"\)[\s\S]*?long_side = (\d+);/),
      short: num(/quality == "hd"\)[\s\S]*?short_side = (\d+);/),
    },
  }
}

describe('画质档的真实尺寸', () => {
  it('三档的长短边都和引擎一致', () => {
    const engine = engineSizes()
    const ui = Object.fromEntries(
      VIDEO_QUALITIES.map((q) => [q.value, { long: q.long, short: q.short }]),
    )
    expect(ui).toEqual(engine)
  })

  it('取到的确实是三档六个数，不是空转', () => {
    const engine = engineSizes()
    expect(Object.keys(engine).sort()).toEqual(['2k', '720p', 'hd'])
    for (const v of Object.values(engine)) {
      expect(v.long).toBeGreaterThan(v.short)
      expect(v.short).toBeGreaterThan(100)
    }
  })

  it('**每一边都是 32 的倍数**——不对齐的话出图直接失败，而日志里指不到那儿', () => {
    // 引擎那段注释写着：第一版这里写的是 720（÷32 = 22.5），单元测试当场
    // 抓住了。前端这份是同样六个数，同样的约束。
    for (const q of VIDEO_QUALITIES) {
      expect(q.short % 32, `${q.value} 的短边 ${q.short}`).toBe(0)
      expect(q.long % 32, `${q.value} 的长边 ${q.long}`).toBe(0)
    }
  })

  it('横屏竖屏两个方向的摆法和引擎一样', () => {
    // 引擎：landscape → {long, short}，其余 → {short, long}
    expect(qualitySize('hd', 'landscape')).toBe('1280×704')
    expect(qualitySize('hd', 'portrait')).toBe('704×1280')
    // 方向是别的值（老项目里没写）时按竖屏，和引擎的 else 分支一致
    expect(qualitySize('hd', '')).toBe('704×1280')
    expect(qualitySize('hd', undefined)).toBe('704×1280')
  })

  it('认不出的档位回空串，不是 undefined×undefined', () => {
    expect(qualitySize('4k', 'portrait')).toBe('')
    expect(qualitySize('', 'portrait')).toBe('')
  })

  it('档位取值就是引擎校验放行的那三个', () => {
    // settings.cpp: `if (quality != "720p" && quality != "hd" && quality != "2k")`
    const src = fs.readFileSync(CPP, 'utf8')
    const m = src.match(/quality != "(\w+)" && quality != "(\w+)" && quality != "(\w+)"/)
    if (!m) throw new Error('settings.cpp 里找不到画质档的校验')
    expect(VIDEO_QUALITIES.map((q) => q.value).sort()).toEqual([m[1], m[2], m[3]].sort())
  })
})
