/**
 * 「当场回 202、结果从 WebSocket 回来」那条路。
 *
 * **每一个要大模型的动作都从这儿走**：照故事定妆、AI 出分镜、写这一集、
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
import { beforeEach, describe, expect, it, vi } from 'vitest'

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
