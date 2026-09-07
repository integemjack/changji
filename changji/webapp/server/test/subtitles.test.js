// 中文字幕断行。
//
// 断行自己算而不是交给 libass：它对中文只按字符断不按语义断，
// 一句话会在词中间折断。这里的规则和 Python 版逐字比对过。

import { describe, expect, it } from 'vitest'

import { displayWidth, findBreak, wrapChinese } from '../src/engine/subtitles.js'

describe('显示宽度', () => {
  it('全角算一半角算半', () => {
    expect(displayWidth('中文')).toBe(2)
    expect(displayWidth('abcd')).toBe(2)
    expect(displayWidth('中a')).toBe(1.5)
  })

  it('全角标点算一', () => {
    expect(displayWidth('，')).toBe(1)
    expect(displayWidth('？')).toBe(1)
    expect(displayWidth('。')).toBe(1)
  })

  it('破折号和省略号算半', () => {
    // 它们在 Unicode 里是 Ambiguous 不是 Wide。这条看着像细节，
    // 但两边算法不一致的话同一句话会断在不同位置
    expect(displayWidth('—')).toBe(0.5)
    expect(displayWidth('…')).toBe(0.5)
  })

  it('空串是零', () => {
    expect(displayWidth('')).toBe(0)
  })
})

describe('断行', () => {
  it('放得下就不断', () => {
    expect(wrapChinese('你到底想说什么')).toEqual(['你到底想说什么'])
  })

  it('优先断在标点之后', () => {
    const lines = wrapChinese('深夜的便利店里只剩下，收银台那一盏灯还亮着')
    expect(lines).toEqual(['深夜的便利店里只剩下，', '收银台那一盏灯还亮着'])
  })

  it('标点离断点太远就不硬凑，按宽度断', () => {
    // 只往回找 8 个字。再远的标点凑过去会让第一行短得离谱，
    // 两行长度差太多比断在词中间更难看
    expect(wrapChinese('你到底想说什么？我等了整整五年时间了啊')).toEqual([
      '你到底想说什么？我等了整整五年',
      '时间了啊',
    ])
  })

  it('放不下就断成两行', () => {
    const lines = wrapChinese('深夜的便利店里只剩下收银台那一盏灯还亮着')
    expect(lines.length).toBe(2)
    expect(lines.join('')).toBe('深夜的便利店里只剩下收银台那一盏灯还亮着')
  })

  it('超出行数上限时并进最后一行而不是丢字', () => {
    // 宁可最后一行长一点。丢字是绝对不能接受的——观众看到的字幕
    // 和听到的话对不上，比字幕难看严重得多
    const long = '这是一句很长很长很长很长很长很长很长很长很长的台词，中间还带着标点，看看断在哪里'
    const lines = wrapChinese(long, 15, 2)
    expect(lines.length).toBeLessThanOrEqual(2)
    expect(lines.join('').replace(/\s/g, '')).toBe(long.replace(/\s/g, ''))
  })

  it('空串给空数组', () => {
    expect(wrapChinese('')).toEqual([])
    expect(wrapChinese('   ')).toEqual([])
  })

  it('标点不落在行首', () => {
    const lines = wrapChinese('一二三四五六七八九十一二三四五六七八九十', 10, 2)
    for (const line of lines.slice(1)) {
      expect('，。？！；：、）】》」』'.includes(line[0])).toBe(false)
    }
  })
})

describe('找断点', () => {
  it('宽度范围内有标点就断在标点后', () => {
    const text = '你好，世界你好世界你好世界'
    expect(findBreak(text, 6)).toBe(3) // 「你好，」之后
  })

  it('没有标点就在宽度上限处断', () => {
    expect(findBreak('一二三四五六七八九十', 5)).toBe(5)
  })

  it('至少切一个字，不会返回零', () => {
    expect(findBreak('，，，，，，', 2)).toBeGreaterThanOrEqual(1)
  })
})
