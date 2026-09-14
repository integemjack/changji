/**
 * 剧本「几个字」这条规则。
 *
 * **为什么值得单写一条用例。** 这个数原来两处各算各的，而且同时印在同一
 * 屏上：分集标签「剧本 157 字」，正下方工具条「129 字 · 目标 60 秒」——
 * 同一份稿子差 28。两边各对了一半，下面每一条都钉住其中一半：
 *
 *   · 空白不算（旧的 EpisodeView 连换行一起数）；
 *   · 按码位数（旧的 EpScript 用 .length，基本平面外的汉字算两个）。
 *
 * 合起来才是这条规则。哪一半被改回去，这儿当场红。
 */
import { describe, expect, it } from 'vitest'

import { countScriptChars } from './labels.js'

describe('剧本字数', () => {
  it('空白一律不算——空行是排版，不是字', () => {
    // 修的就是这一条：多空两行不该让「写了多少」涨两个
    expect(countScriptChars('一二三')).toBe(3)
    expect(countScriptChars('一二三\n')).toBe(3)
    expect(countScriptChars('一二\n\n\n三')).toBe(3)
    expect(countScriptChars('  一 二\t三 \r\n')).toBe(3)
    // 全角空格也是空白（\s 在 JS 里含 U+3000）
    expect(countScriptChars('一　二')).toBe(2)
  })

  it('按码位数，不按 UTF-16 码元——生僻字一个就是一个', () => {
    // 𠮷（U+20BB7）在基本平面外，人名里真的有。旧的 .length 数成 2
    expect('𠮷'.length).toBe(2) // 先说明问题确实存在
    expect(countScriptChars('𠮷')).toBe(1)
    expect(countScriptChars('林𠮷渊')).toBe(3)
  })

  it('没有稿子时是 0，不是 NaN，也不抛', () => {
    // 分集标签按真假决定显不显示这一段，NaN 会一直显示成「NaN 字」
    for (const v of [undefined, null, '']) expect(countScriptChars(v)).toBe(0)
    expect(countScriptChars('   \n\n  ')).toBe(0)
  })

  it('一份像样的剧本：只数正文，不数分幕之间的空行', () => {
    const s = ['【第一幕】', '', '林渊：你还是来了。', '', '苏晚：我以为你不会等。'].join('\n')
    // 【第一幕】5 + 林渊：你还是来了。9 + 苏晚：我以为你不会等。11 = 25
    // （冒号和句号都算——见下面那条）
    expect(countScriptChars(s)).toBe(25)
    // 换一种排版（多空一行），字数不该变——这正是旧实现会变的地方
    expect(countScriptChars(s.replace(/\n/g, '\n\n'))).toBe(25)
  })

  it('标点算字——中文稿子的字数一向包含标点', () => {
    expect(countScriptChars('你好。')).toBe(3)
    expect(countScriptChars('「你好」')).toBe(4)
  })
})
