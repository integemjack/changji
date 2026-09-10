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

  it('正在跑的镜头：进来、跑完出去、失败出去', () => {
    // 多卡之后同一时刻好几镜在出，界面要列得出"哪几镜、各到第几步"。
    const s = useRun()
    const at = (shot, kind, step = 1) =>
      s.applyMessage({ type: 'progress', kind, stage: 'draft', shot_id: shot,
                       step, total: 12, message: `${shot} ${kind}` })
    at('sh1', 'progress')
    at('sh2', 'progress')
    at('sh3', 'progress', 3)
    expect(s.inflight.map((x) => x.shot_id)).toEqual(['sh1', 'sh2', 'sh3'])

    at('sh2', 'shot_done')             // 跑完了
    at('sh3', 'warn')                  // 这一轮失败了
    expect(s.inflight.map((x) => x.shot_id)).toEqual(['sh1'])

    at('sh3', 'progress', 4)           // 重试又进来
    expect(s.inflight.map((x) => x.shot_id)).toEqual(['sh1', 'sh3'])
    expect(s.inflight.at(-1).step).toBe(4)

    // 事件日志里的 kind 也要是引擎给的，视图按它标颜色
    expect(s.events.find((e) => e.kind === 'shot_done')?.shot_id).toBe('sh2')
  })

  it('换阶段时，上一阶段留下的全清掉', () => {
    // **这一条是被一个真 bug 逼出来的。** 配音和首帧两个阶段只报 progress、
    // 不报 shot_done，于是跑过的镜头全部永久挂在表里——一集跑完配音之后
    // 整面墙都写着「配音」，包括那些其实只是在等的。用户报的就是这个。
    //
    // 引擎那边已经补了 shot_done。这里是兜底：流水线是严格分阶段的
    // （所有镜头先配音、再所有镜头出首帧），所以一收到新阶段的消息，
    // 上一阶段还挂着的必然已经跑完了。
    const s = useRun()
    const at = (shot, stage) =>
      s.applyMessage({ type: 'progress', kind: 'progress', stage,
                       shot_id: shot, step: 1, total: 3 })
    at('sh1', 'audio')
    at('sh2', 'audio')
    at('sh3', 'audio')
    expect(s.inflight).toHaveLength(3)

    // 进首帧阶段了，配音那三条不该再挂着
    at('sh1', 'frames')
    expect(s.inflight.map((x) => x.shot_id)).toEqual(['sh1'])
    expect(s.inflight[0].stage).toBe('frames')
  })

  it('老引擎不带 kind：全当 progress，表只进不出', () => {
    const s = useRun()
    s.applyMessage({ type: 'progress', stage: 'draft', shot_id: 'sh1', step: 1, total: 3 })
    expect(s.inflight).toHaveLength(1)
    expect(s.events[0].kind).toBe('progress')
  })

  it('一镜落定就把 settled 加一，progress 不算', () => {
    // 镜头墙靠它重拉。原来靠六秒定时器，而 Chrome 把后台标签页的
    // setInterval 压到一分钟一次（实测两分半只拉了两次）：首帧出来了，
    // 牌子上要等一分钟才变。WebSocket 不受这个限制，shot_done 就是信号。
    const s = useRun()
    const at = (kind, shot = 'sh1') =>
      s.applyMessage({ type: 'progress', kind, stage: 'frames', shot_id: shot, step: 1, total: 3 })
    expect(s.settled).toBe(0)
    at('progress')
    at('progress')
    expect(s.settled).toBe(0)          // 走着的步子不算落定
    at('shot_done')
    expect(s.settled).toBe(1)
    at('warn', 'sh2')                   // 失败也是落定——那一镜不会再动了
    expect(s.settled).toBe(2)
    // 不带 shot_id 的（整体 warn / eta）不算
    s.applyMessage({ type: 'progress', kind: 'eta', stage: 'frames', step: 1, total: 3 })
    expect(s.settled).toBe(2)
  })

  it('预览图：只换那一格的画面，落定就删，不进事件流', () => {
    // 引擎每一步推一张潜空间投影的小图，牌子上放大显示，由糊到清。
    // 它不是"一步"——不能进事件流、不能动进度；落定之后真图落盘了，
    // 预览再留着只会盖住真图。
    const s = useRun()
    const pv = (shot, data) =>
      s.applyMessage({ type: 'progress', kind: 'preview', shot_id: shot, step: 4, preview: data })
    pv('sh1', 'data:image/png;base64,AAA')
    pv('sh1', 'data:image/png;base64,BBB')     // 新的一步盖掉旧的
    expect(s.previewOf('sh1')).toBe('data:image/png;base64,BBB')
    expect(s.previewOf('sh2')).toBe('')
    expect(s.events).toHaveLength(0)             // 不进事件流
    expect(s.inflight).toHaveLength(0)           // 也不算"正在跑"的一步

    s.applyMessage({ type: 'progress', kind: 'shot_done', stage: 'frames',
                     shot_id: 'sh1', step: 1, total: 3 })
    expect(s.previewOf('sh1')).toBe('')          // 落定就删
  })

  it('停下轮询时也清空正在跑的表', () => {
    // **它是一份快照，停下之后没有任何东西再更新它。**
    // 留着的话，切到别的页面再切回来，那几镜的进度条冻在离开那一刻的
    // 位置上——而它们多半早就跑完了。用户报的原话是
    // "切换之前完成的会卡在原来的位置上"。
    const s = useRun()
    s.applyMessage({ type: 'progress', kind: 'progress', stage: 'frames',
                     shot_id: 'sh1', step: 1, total: 3 })
    expect(s.inflight).toHaveLength(1)
    s.stop()
    expect(s.inflight).toHaveLength(0)
  })

  it('任务结束清空正在跑的表', async () => {
    const s = useRun()
    s.applyMessage({ type: 'progress', kind: 'progress', stage: 'draft',
                     shot_id: 'sh1', step: 1, total: 3 })
    expect(s.inflight).toHaveLength(1)
    s.applyMessage({ type: 'done' })
    expect(s.inflight).toHaveLength(0)
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
