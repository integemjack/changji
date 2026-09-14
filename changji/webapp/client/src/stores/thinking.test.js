import { beforeEach, describe, expect, it } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'

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
