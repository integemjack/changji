import { describe, expect, it } from 'vitest'

import { projectStage } from './project-stage'

const P = (o = {}) => ({
  episodes: 0,
  shots: 0,
  done_shots: 0,
  outputs: 0,
  chapters: 0,
  written_chapters: 0,
  ...o,
})

describe('projectStage', () => {
  it('空项目和写完故事的项目不该长得一样', () => {
    // 这是这个函数存在的理由：原来两个都是 0%
    const empty = projectStage(P())
    const outlined = projectStage(P({ chapters: 8 }))
    expect(empty.key).toBe('empty')
    expect(outlined.key).toBe('outline')
    expect(outlined.percent).toBeGreaterThan(empty.percent)
  })

  it('故事层那三档', () => {
    expect(projectStage(P({ chapters: 8 })).label).toContain('8 章大纲')
    expect(projectStage(P({ chapters: 8, written_chapters: 3 })).label).toContain('3/8')
    expect(projectStage(P({ chapters: 8, written_chapters: 8, episodes: 12 })).label).toContain(
      '12 集',
    )
  })

  it('一镜都没跑，不能说成「出片 0%」', () => {
    // 那读起来像在进行中，而实际是分镜出好了、还没按开始
    const s = projectStage(P({ episodes: 1, shots: 8 }))
    expect(s.key).toBe('ready')
    expect(s.label).toBe('8 镜待出片')
  })

  it('出片中报百分比', () => {
    const s = projectStage(P({ episodes: 1, shots: 10, done_shots: 4 }))
    expect(s.key).toBe('shooting')
    expect(s.label).toBe('出片 40%')
  })

  it('镜头出完但没装配，不能说成完事了', () => {
    // 装配要 ffmpeg，缺它的机器会停在这儿。写成 100% 会让人以为好了。
    const s = projectStage(P({ episodes: 1, shots: 10, done_shots: 10 }))
    expect(s.key).toBe('assemble')
    expect(s.percent).toBeLessThan(100)
    expect(s.tone).toBe('warn')
  })

  it('有成片就是有成片', () => {
    const s = projectStage(P({ episodes: 1, shots: 10, done_shots: 10, outputs: 2 }))
    expect(s.key).toBe('film')
    expect(s.percent).toBe(100)
  })

  it('从后往前判：出完片之后又加了大纲，不该倒退回「还没展开正文」', () => {
    const s = projectStage(
      P({ outputs: 3, shots: 10, done_shots: 10, episodes: 2, chapters: 20, written_chapters: 2 }),
    )
    expect(s.key).toBe('film')
  })

  it('坏项目优先说读不了', () => {
    expect(projectStage({ broken: '文件损坏', outputs: 5 }).key).toBe('broken')
  })

  it('字段缺了也不能炸', () => {
    expect(projectStage(undefined).key).toBe('empty')
    expect(projectStage({}).key).toBe('empty')
  })
})
