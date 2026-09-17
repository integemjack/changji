import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'

import { api } from '@/api'
import { useThinking } from '@/stores/thinking'

describe('模型在想什么', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('增量拼起来，完了就清掉', () => {
    const t = useThinking()
    expect(t.busy).toBe(false)

    t.start('job-1', '写大纲')
    expect(t.busy).toBe(true)
    t.push('job-1', '先想想')
    t.push('job-1', '这一章')
    expect(t.latest).toBe('先想想这一章')
    expect(t.items[0].label).toBe('写大纲')

    t.finish('job-1')
    expect(t.busy).toBe(false)
    expect(t.latest).toBe('')
  })

  it('没 start 过也收——socket 上的消息可能比 start 先到', () => {
    // 丢掉的话这一步就永远不显示了，而这是个时序问题，不稳定复现。
    const t = useThinking()
    t.push('job-2', '已经在想了')
    expect(t.busy).toBe(true)
    expect(t.latest).toBe('已经在想了')
  })

  it('只留最后几千字，不无限长', () => {
    // 一段思考能有上万字，十几步攒下来会把内存和渲染都吃掉。
    // 浮层里要看的本来就是"它现在在想什么"。
    const t = useThinking()
    t.start('job-3')
    t.push('job-3', 'a'.repeat(3000))
    t.push('job-3', 'b'.repeat(3000))
    expect(t.latest.length).toBe(4000)
    // 丢的是前面那段，留的是最新的
    expect(t.latest.endsWith('b')).toBe(true)
    expect(t.latest.startsWith('a')).toBe(true)
  })

  it('几件同时在想，新的排前面', () => {
    const t = useThinking()
    t.start('job-a', '拆分镜')
    t.start('job-b', '写正文')
    expect(t.items).toHaveLength(2)
    expect(t.items[0].id).toBe('job-b')

    t.finish('job-b')
    expect(t.items).toHaveLength(1)
    expect(t.items[0].id).toBe('job-a')
  })

  it('清一条不存在的不报错', () => {
    // finally 里会无条件清一次，同步那条路根本没 start 过。
    const t = useThinking()
    expect(() => t.finish('没有这条')).not.toThrow()
    expect(() => t.finish('')).not.toThrow()
    expect(t.busy).toBe(false)
  })

  it('空串不算一段', () => {
    const t = useThinking()
    t.push('job-4', '')
    expect(t.busy).toBe(false)
  })
})

describe('刷新之后靠引擎那本账接回来', () => {
  beforeEach(() => setActivePinia(createPinia()))
  afterEach(() => vi.restoreAllMocks())

  it('socket 空的时候用账本那份', async () => {
    // ⚠️ `live` 按点击生成的 streamId 存，**刷新一下就没了**（用户
    // 2026-09-17：「顶部的思考刷新后就再也不显示」）。账本按任务 id 索引，
    // 那个数刷新之后照样在。
    vi.spyOn(api, 'tasks').mockResolvedValue({
      running: [{ id: 7, title: '写正文 · 还缺的 7 章', thinking: true, seconds: 12 }],
    })
    vi.spyOn(api, 'taskThinking').mockResolvedValue({
      thinking: '先想这个',
      start: 0,
      end: 4,
    })
    const t = useThinking()
    await t.syncFromServer()
    expect(t.busy).toBe(true)
    expect(t.latest).toBe('先想这个')
    expect(t.items[0].label).toBe('写正文 · 还缺的 7 章')
  })

  it('socket 上有东西就一行都不看账本', async () => {
    // 本标签页自己点的那次 socket 快一拍半，两边同时显示会成双份。
    const spy = vi.spyOn(api, 'tasks').mockResolvedValue({ running: [] })
    const t = useThinking()
    t.start('ref-abc', '出图')
    t.push('ref-abc', '这是 socket 那份')
    await t.syncFromServer()
    expect(spy).not.toHaveBeenCalled()
    expect(t.latest).toBe('这是 socket 那份')
  })

  it('只取新增，中间断了就丢掉重接', async () => {
    const t = useThinking()
    vi.spyOn(api, 'tasks').mockResolvedValue({
      running: [{ id: 9, title: '拆镜头', thinking: true, seconds: 1 }],
    })
    const think = vi.spyOn(api, 'taskThinking')
    think.mockResolvedValueOnce({ thinking: 'AAA', start: 0, end: 3 })
    await t.syncFromServer()
    expect(t.latest).toBe('AAA')
    // 接着取：start 正好接上，拼起来
    think.mockResolvedValueOnce({ thinking: 'BBB', start: 3, end: 6 })
    await t.syncFromServer()
    expect(t.latest).toBe('AAABBB')
    // start 跳过去了 = 引擎那头从头截过，手上这份作废
    think.mockResolvedValueOnce({ thinking: 'CCC', start: 99, end: 102 })
    await t.syncFromServer()
    expect(t.latest).toBe('CCC')
  })
})
