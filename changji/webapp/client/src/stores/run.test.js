/**
 * 进度状态的测试。
 *
 * 重点是**推上来的增量和快照的字段名不一样**：引擎推的是 `step`，
 * 快照里叫 `current`。直接把消息铺进 state 的话进度条读到 undefined，
 * 表现是"跑着但进度条一直是 0"——而那和任务真的卡住看起来一模一样。
 */

import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const runStatus = vi.fn()
const seriesStatus = vi.fn()
vi.mock('@/api', () => ({
  api: { runStatus: () => runStatus(), seriesStatus: () => seriesStatus() },
}))
// WebSocket 在 node 环境里没有。这几条测的是收到消息之后怎么并状态，
// 不测连接本身——中转那一段在服务端的 ws.test.js 里用真 socket 测过。
vi.stubGlobal('WebSocket', class { constructor() { throw new Error('无') } })

const { useRun, useWriter } = await import('./run.js')

describe('applyMessage', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    runStatus.mockReset()
    runStatus.mockResolvedValue({ running: false, events: [] })
  })

  it('step 要落到 current 上，不是原样铺进去', () => {
    const s = useRun()
    s.applyMessage({ type: 'progress', job_id: 'run-1', stage: 'audio',
                     step: 3, total: 10, message: '第三句' })
    expect(s.state.current).toBe(3)
    expect(s.state.total).toBe(10)
    expect(s.percent).toBe(30)
    expect(s.stageLabel).toBe('配音')
  })

  it('推上来的进度也进事件日志，不用等跑完才出现', () => {
    const s = useRun()
    s.applyMessage({ type: 'progress', job_id: 'run-1', stage: 'audio',
                     step: 1, total: 4, message: '开始', shot_id: 'sh1' })
    expect(s.events).toHaveLength(1)
    // 视图按 shot_id 分组、按 kind 标严重程度，这两个键必须在
    expect(s.events[0].shot_id).toBe('sh1')
    expect(s.events[0].kind).toBe('progress')
  })

  it('hello 不动状态', () => {
    // 连上时服务端先发一条问候。把它当进度处理的话，
    // 会凭空造出一个 running=true 的状态。
    const s = useRun()
    s.applyMessage({ type: 'hello', service: 'changji' })
    expect(s.state).toBeNull()
    expect(s.running).toBe(false)
  })

  it('done 要再拉一次全量——产出列表只有快照里有', () => {
    const s = useRun()
    s.applyMessage({ type: 'done', job_id: 'run-1', outputs: ['a.mp4'] })
    expect(runStatus).toHaveBeenCalledTimes(1)
  })

  it('error 也要拉全量', () => {
    const s = useRun()
    s.applyMessage({ type: 'error', job_id: 'run-1', message: '炸了' })
    expect(runStatus).toHaveBeenCalledTimes(1)
  })

  it('缺字段的消息不覆盖已有进度', () => {
    // 引擎的日志类事件 total 是 0，不该把进度条清零——
    // 快照那边有同样的规则（jobs.cpp 里 if (ev.total)）。
    const s = useRun()
    s.applyMessage({ type: 'progress', stage: 'audio', step: 5, total: 10 })
    s.applyMessage({ type: 'progress', message: '一句日志' })
    expect(s.state.current).toBe(5)
    expect(s.state.total).toBe(10)
    expect(s.state.stage).toBe('audio')
  })

  it('乱七八糟的消息不掀翻这一屏', () => {
    const s = useRun()
    expect(() => s.applyMessage(null)).not.toThrow()
    expect(() => s.applyMessage('字符串')).not.toThrow()
    expect(() => s.applyMessage(123)).not.toThrow()
    expect(s.state).toBeNull()
  })

  it('事件日志有上限，长任务不会把内存吃光', () => {
    const s = useRun()
    for (let i = 0; i < 250; i += 1) {
      s.applyMessage({ type: 'progress', stage: 'audio', step: i, total: 250 })
    }
    expect(s.events.length).toBeLessThanOrEqual(200)
    // 留的是最后那些——用户要看的是刚发生的
    expect(s.events.at(-1).current).toBe(249)
  })
})

describe('useWriter 的 applyMessage', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    seriesStatus.mockReset()
    seriesStatus.mockResolvedValue({ running: false })
  })

  it('step 要落到 done 上——和流水线那边不是同一个字段', () => {
    // 写作任务的快照是 {running, done, total, ...}，进度条算 done/total。
    // 照抄流水线那边的 current 映射，进度条会一直是 0。
    const w = useWriter()
    w.applyMessage({ type: 'progress', job_id: 'write-1', step: 4, total: 8 })
    expect(w.state.done).toBe(4)
    expect(w.state.total).toBe(8)
    expect(w.percent).toBe(50)
  })

  it('done 之后要拉全量——episodes 只有快照里有', () => {
    const w = useWriter()
    w.applyMessage({ type: 'done', job_id: 'write-1' })
    expect(seriesStatus).toHaveBeenCalledTimes(1)
  })

  it('hello 和坏消息都不动状态', () => {
    const w = useWriter()
    w.applyMessage({ type: 'hello', service: 'changji' })
    expect(() => w.applyMessage(null)).not.toThrow()
    expect(w.state).toBeNull()
  })
})
