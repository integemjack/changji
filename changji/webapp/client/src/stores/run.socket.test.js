/**
 * 进度那两条 WebSocket 的连接生命周期。
 *
 * **和 run.test.js 分开一个文件**：那边把 WebSocket 桩成"构造就抛"，测的是
 * 收到消息之后怎么并状态；这边要的恰恰相反——把真正那条路跑起来。
 * （node 环境没有 `window`，不补桩的话 `jobSocketUrl()` 当场抛，
 * openJobSocket 会把它当成"连不上"。见 useAsyncJob.test.js 开头。）
 *
 * 盯的是一件事：**断了之后还会不会自己接回来**。单镜那一块的进度、采样
 * 中途的预览小图、落定一镜立刻重拉，只从这条 socket 来；断了不接回来的
 * 症状是"总进度看着是对的，只有单镜那一块不动"。
 */
import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'

let born = []
class FakeSocket {
  constructor() { born.push(this); this.closed = false }
  send() {}
  close() { this.closed = true }
  open() { this.onopen?.() }
  push(msg) { if (!this.closed) this.onmessage?.({ data: JSON.stringify(msg) }) }
  drop() { this.onclose?.() }
}
vi.stubGlobal('WebSocket', FakeSocket)
vi.stubGlobal('window', { location: { protocol: 'http:', host: 'x' } })

const runStatus = vi.fn()
const seriesStatus = vi.fn()
vi.mock('@/api', () => ({
  api: { runStatus: () => runStatus(), seriesStatus: () => seriesStatus() },
}))

const { useRun, useWriter } = await import('./run.js')

describe('断线之后要自己接回来', () => {
  beforeEach(() => {
    vi.useFakeTimers()
    setActivePinia(createPinia())
    born = []
    runStatus.mockResolvedValue({ running: true, events: [] })
    seriesStatus.mockResolvedValue({ running: true })
  })

  it('出片那条：断了五秒后重连', () => {
    const r = useRun()
    r.start()
    expect(born.length).toBe(1)
    born[0].open()
    born[0].drop()

    vi.advanceTimersByTime(4000)
    expect(born.length).toBe(1) // 还没到点
    vi.advanceTimersByTime(1500)
    expect(born.length).toBe(2) // 接回来了
  })

  it('写作那条也一样', () => {
    const w = useWriter()
    w.start()
    born[0].open()
    born[0].drop()
    vi.advanceTimersByTime(5000)
    expect(born.length).toBe(2)
  })

  it('stop() 之后排着的那次不许再爬起来', () => {
    const r = useRun()
    r.start()
    born[0].open()
    born[0].drop() // 排了一个五秒后的重连
    r.stop()
    vi.advanceTimersByTime(30000)
    expect(born.length).toBe(1)
  })

  it('重连不会叠加：排着的同时又 start()，只该有一条', () => {
    const r = useRun()
    r.start()
    born[0].open()
    born[0].drop()

    // 这几秒里页面重新进来一次（stop 之后再 start）
    r.stop()
    r.start()
    expect(born.length).toBe(2)

    // 排着的那次到点了，不该再多一条
    vi.advanceTimersByTime(30000)
    expect(born.length).toBe(2)
  })

  it('连着的时候不会重复连', () => {
    const r = useRun()
    r.start()
    r.start()
    r.start()
    expect(born.length).toBe(1)
  })
})
