import { describe, expect, it } from 'vitest'

import { chapterWord } from './useChapterMode'

// 章模式只换两个叫法：步骤条的「这一集」和设定页的「分集」。别的原样。
describe('chapterWord', () => {
  it('章模式关着：一个字不改', () => {
    expect(chapterWord('这一集', false)).toBe('这一集')
    expect(chapterWord('分集', false)).toBe('分集')
    expect(chapterWord('设定', false)).toBe('设定')
  })
  it('章模式开着：这一集 → 这一章，分集 → 章节', () => {
    expect(chapterWord('这一集', true)).toBe('这一章')
    expect(chapterWord('分集', true)).toBe('章节')
  })
  it('别的页签不受影响', () => {
    for (const t of ['故事', '设定', '设置', '镜头', '成片']) {
      expect(chapterWord(t, true)).toBe(t)
    }
  })
})
