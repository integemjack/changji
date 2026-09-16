import { describe, expect, it } from 'vitest'

// 纯逻辑那一半单独测：名字 → 第几章。composable 本身要 Pinia + 网络，
// 这里只钉住反向索引的规矩。
function buildIndex(chapters) {
  const out = new Map()
  chapters.forEach((c, i) => {
    const entry = { no: i + 1, title: c.title ?? '' }
    for (const name of [...(c.characters ?? []), ...(c.locations ?? [])]) {
      const key = String(name ?? '').trim()
      if (!key) continue
      if (!out.has(key)) out.set(key, [])
      const rows = out.get(key)
      if (!rows.some((r) => r.no === entry.no)) rows.push(entry)
    }
  })
  return out
}
const label = (idx, name) => {
  const rows = idx.get(name) ?? []
  return rows.length ? '第 ' + rows.map((r) => r.no).join('、') + ' 章' : ''
}

describe('章节反向索引', () => {
  const chapters = [
    { title: '背叛', characters: ['曾老板', '宋律师'], locations: ['城南酒吧'] },
    { title: '追踪', characters: ['曾老板'], locations: ['废弃工厂'] },
    { title: '线人', characters: ['曾老板', '郑哥'], locations: ['城南酒吧'] },
  ]
  const idx = buildIndex(chapters)

  it('人和地方都索引，章号从 1 数起', () => {
    expect(label(idx, '曾老板')).toBe('第 1、2、3 章')
    expect(label(idx, '郑哥')).toBe('第 3 章')
    expect(label(idx, '城南酒吧')).toBe('第 1、3 章')
    expect(label(idx, '废弃工厂')).toBe('第 2 章')
  })
  it('没出现过的回空串，调用方好据此不摆这一栏', () => {
    expect(label(idx, '谁也不是')).toBe('')
    expect(label(idx, '')).toBe('')
  })
  it('同一章里重复出现只算一次', () => {
    const dup = buildIndex([{ title: 'x', characters: ['A', 'A'], locations: ['A'] }])
    expect(label(dup, 'A')).toBe('第 1 章')
  })
  it('空名字和空白名字不进索引', () => {
    const blank = buildIndex([{ title: 'x', characters: ['', '  ', 'B'] }])
    expect(blank.has('')).toBe(false)
    expect(label(blank, 'B')).toBe('第 1 章')
  })
})
