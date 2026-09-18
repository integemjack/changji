/**
 * 「当场回 202、结果从 WebSocket 回来」那条路。
 *
 * **每一个要大模型的动作都从这儿走**：照故事定妆、AI 出分镜、写这一章、
 * 剪预告、出参考图。而它原来一条测试都没有——不是没人写，是**跑不起来**：
 * vitest 默认的 node 环境里没有 `window`，`jobSocketUrl()` 读
 * `window.location` 当场抛，`openJobSocket` 的 try/catch 把它当成"连不上"，
 * 于是不管怎么摆，测到的永远是退回同步那条。run.test.js 里那句
 * 「WebSocket 在 node 环境里没有」说的就是这件事。
 *
 * 补一个 `window` 桩就能把真正的那条路跑起来。这里钉住六种收尾，其中两条
 * 是竞态——它们错了不会报错，只会表现成"这一步永远转着"或者"结果串台"。
 */
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { useThinking } from '@/stores/thinking'

/** 信箱那两条接口。socket 开着的那几条用例一次都不会碰到它们。 */
const watchJob = vi.fn()
const jobEvents = vi.fn()
vi.mock('@/api', () => ({ api: { watchJob: (...a) => watchJob(...a), jobEvents: (...a) => jobEvents(...a) } }))

/** 一条能手工驱动的假 socket。最后造出来的那个记在 `live` 上。 */
let live = null
class FakeSocket {
  constructor() {
    live = this
    this.sent = []
  }
  send(raw) { this.sent.push(JSON.parse(raw)) }
  close() { this.closed = true }
  open() { this.onopen?.() }
  push(msg) { this.onmessage?.({ data: JSON.stringify(msg) }) }
  drop() { this.onclose?.() }
}
/** 连都连不上的那种。公司代理把 Upgrade 掐了就是这个样子。 */
class DeadSocket {
  constructor() { throw new Error('代理把 Upgrade 掐了') }
}
vi.stubGlobal('WebSocket', FakeSocket)
vi.stubGlobal('window', { location: { protocol: 'http:', host: 'localhost:8080' } })

const { runAsyncJob } = await import('./useAsyncJob')

const tick = () => new Promise((r) => setTimeout(r, 0))

/**
 * 起一件活，等它把订阅发出去。
 *
 * 回的 `done` **已经接住了异常**：调用方只看 `{ok, value}` / `{ok, message}`，
 * 免得某一条用例漏了 catch 就变成一个和它无关的未处理拒绝。
 */
async function launch(send) {
  const p = runAsyncJob(send, { prefix: 'p' })
  const done = p.then(
    (value) => ({ ok: true, value }),
    (err) => ({ ok: false, message: err.message }),
  )
  await tick()
  live.open()
  await tick()
  return { done, ws: live, id: live.sent[0]?.job_id }
}

describe('runAsyncJob', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    live = null
  })

  it('socket 开着就带上 stream + async，订的是同一个 id', async () => {
    let got = null
    const r = await launch((extra) => {
      got = extra
      return Promise.resolve({ started: true })
    })
    expect(r.ws.sent[0]).toEqual({ type: 'subscribe', job_id: r.id })
    expect(got).toEqual({ stream: r.id, async: true })
    r.ws.push({ type: 'job_done', job_id: r.id, result: { ok: 1 } })
    expect(await r.done).toEqual({ ok: true, value: { ok: 1 } })
  })

  it('干完之后要把 socket 关掉', async () => {
    const r = await launch(() => Promise.resolve({ started: true }))
    r.ws.push({ type: 'job_done', job_id: r.id, result: {} })
    await r.done
    expect(r.ws.closed).toBe(true)
  })

  it('job_error 原样抛出引擎那句话', async () => {
    const r = await launch(() => Promise.resolve({ started: true }))
    r.ws.push({ type: 'job_error', job_id: r.id, message: '显存不够加载 LLM' })
    expect(await r.done).toEqual({ ok: false, message: '显存不够加载 LLM' })
  })

  it('中途断线也要把等的人放出来，不能一直挂着', async () => {
    const r = await launch(() => Promise.resolve({ started: true }))
    r.ws.drop()
    const fin = await r.done
    expect(fin.ok).toBe(false)
    expect(fin.message).toContain('连接断了')
  })

  it('回包里没有 started 就是同步那条，直接拿它当结果', async () => {
    const r = await launch(() => Promise.resolve({ shots: 7 }))
    expect(await r.done).toEqual({ ok: true, value: { shots: 7 } })
  })

  // ---- 两条竞态：错了不会报错，只会"永远转着"或者"结果串台" ----

  it('job_done 比 send 的回包还快，结果不能丢', async () => {
    const r = await launch(
      () => new Promise((res) => setTimeout(() => res({ started: true }), 5)),
    )
    r.ws.push({ type: 'job_done', job_id: r.id, result: { early: 1 } })
    expect(await r.done).toEqual({ ok: true, value: { early: 1 } })
  })

  it('别的活在同一条连接上完事，不能被当成自己的', async () => {
    const r = await launch(() => Promise.resolve({ started: true }))
    r.ws.push({ type: 'job_done', job_id: '别人的', result: { wrong: 1 } })
    r.ws.push({ type: 'job_done', job_id: r.id, result: { mine: 1 } })
    expect(await r.done).toEqual({ ok: true, value: { mine: 1 } })
  })
})

/**
 * 连不上 WebSocket 的那条退路。
 *
 * 原来这条是"不带 async，让 HTTP 一直等到干完"——**进度、思考、停下三样
 * 一起没有**，所以单补一个取消也没用：顶栏那块徽标是 thinking.start 才
 * 出现的，没有它就没有按钮。现在改成开个信箱、照样异步跑、拿 HTTP 把同一
 * 批消息取回来。
 *
 * 这几条钉的都是"错了不报错"的那种：走错路只会表现成少一块 UI 或者
 * "永远转着"。
 */
describe('runAsyncJob：socket 连不上就走信箱轮询', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    live = null
    watchJob.mockReset()
    jobEvents.mockReset()
    vi.stubGlobal('WebSocket', DeadSocket)
  })
  afterEach(() => vi.stubGlobal('WebSocket', FakeSocket))

  it('开信箱、照样带 async，进度和思考都还在，结果从轮询回来', async () => {
    watchJob.mockResolvedValue({ watching: true })
    let extra = null
    let id = null
    const seen = []
    jobEvents.mockImplementation((stream, since) => {
      id = stream
      return Promise.resolve({
        exists: true,
        next: 3,
        done: true,
        dropped: 0,
        events: since > 0 ? [] : [
          { type: 'job_progress', job_id: stream, current: 2, total: 5, message: '画着' },
          { type: 'job_thinking', job_id: stream, text: '先想想' },
          { type: 'job_done', job_id: stream, result: { shots: 7 } },
        ],
      })
    })
    const got = await runAsyncJob(
      (e) => { extra = e; return Promise.resolve({ started: true }) },
      { prefix: 'p', label: '写大纲', onProgress: (c, t, m) => seen.push([c, t, m]) },
    )
    expect(watchJob).toHaveBeenCalledTimes(1)
    // **必须还是异步那条**：同步跑的话引擎压根不挂 JobScope，
    // 顶栏那个「停下」按下去找不到这件活。
    expect(extra).toEqual({ stream: id, async: true })
    expect(seen).toEqual([[2, 5, '画着']])
    expect(got).toEqual({ shots: 7 })
  })

  it('那条连不上的 socket 已经喊过「断了」，不能拿它当结论', async () => {
    // DeadSocket 的 onDrop 在开信箱之前就把"连接断了"settle 掉了。
    // 不清掉的话轮询第一轮就拿那句话收场——而活跑得好好的。
    watchJob.mockResolvedValue({ watching: true })
    jobEvents.mockImplementation((stream) => Promise.resolve({
      exists: true, next: 1, done: true, dropped: 0,
      events: [{ type: 'job_done', job_id: stream, result: { ok: 1 } }],
    }))
    await expect(runAsyncJob(() => Promise.resolve({ started: true }), { prefix: 'p' }))
      .resolves.toEqual({ ok: 1 })
  })

  it('轮询这条也要上顶栏——不然没有「停下」可按', async () => {
    watchJob.mockResolvedValue({ watching: true })
    const thinking = useThinking()
    let stream = null
    jobEvents.mockImplementation((id) => Promise.resolve({
      exists: true, next: 1, done: true, dropped: 0,
      events: [{ type: 'job_done', job_id: id, result: {} }],
    }))
    // **请求发出去的那一刻顶栏就该有它**：这件活从这一刻起就能跑几分钟，
    // 而「停下」是按 stream 认的，顶栏上那一条的 id 必须就是它。
    let up = null
    await runAsyncJob(
      (extra) => {
        stream = extra.stream
        up = Object.keys(thinking.live)
        return Promise.resolve({ started: true })
      },
      { prefix: 'p', label: '写大纲' },
    )
    expect(up).toEqual([stream])
    expect(thinking.live[stream]).toBeUndefined()   // 干完要收干净
    expect(Object.keys(thinking.live)).toHaveLength(0)
  })

  it('信箱没了要收场，不能一直转', async () => {
    watchJob.mockResolvedValue({ watching: true })
    jobEvents.mockResolvedValue({ exists: false, events: [], next: 0, done: false })
    await expect(runAsyncJob(() => Promise.resolve({ started: true }), { prefix: 'p' }))
      .rejects.toThrow(/连接断了/)
  })

  it('连信箱都开不出来，才退回真同步那条', async () => {
    watchJob.mockRejectedValue(new Error('404'))
    let extra = null
    const got = await runAsyncJob(
      (e) => { extra = e; return Promise.resolve({ shots: 3 }) },
      { prefix: 'p' },
    )
    expect(extra).toEqual({})
    expect(got).toEqual({ shots: 3 })
    expect(jobEvents).not.toHaveBeenCalled()
  })
})
