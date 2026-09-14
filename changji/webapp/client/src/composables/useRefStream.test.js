/**
 * 参考图那条固定频道。
 *
 * 模块级只有一条连接，三个格子（角色 / 场景 / 分集）都订着它的 `finished`
 * 去重拉资产库——**所以这条连接多出一根，代价是三个页面各多打一遍
 * `/api/assets`**，而且不会有任何报错。
 */
import { beforeEach, describe, expect, it, vi } from 'vitest'

/** 造出来的所有假 socket，按顺序记着。 */
let born = []
class FakeSocket {
  constructor() {
    born.push(this)
    this.closed = false
  }
  send() {}
  close() { this.closed = true }
  open() { this.onopen?.() }
  push(msg) { if (!this.closed) this.onmessage?.({ data: JSON.stringify(msg) }) }
  drop() { this.onclose?.() }
}
vi.stubGlobal('WebSocket', FakeSocket)
// 没有 window 的话 `jobSocketUrl()` 当场抛，openJobSocket 会把它当成
// "连不上"——那样测到的永远不是真正这条路。见 useAsyncJob.test.js 开头。
vi.stubGlobal('window', { location: { protocol: 'http:', host: 'x' } })

/** 每条用例都要一份干净的模块级状态。 */
async function fresh() {
  vi.resetModules()
  born = []
  return (await import('./useRefStream')).useRefStream
}

describe('useRefStream', () => {
  beforeEach(() => vi.useFakeTimers())

  it('只连一条，来回用不会多连', async () => {
    const useRefStream = await fresh()
    useRefStream()
    useRefStream()
    useRefStream()
    expect(born.length).toBe(1)
  })

  it('断线之后有人挂载，排着的那次重连不能再连一条', async () => {
    const useRefStream = await fresh()
    const { finished } = useRefStream()
    born[0].open()

    born[0].drop() // 断了：模块排一个 5 秒后的重连
    useRefStream() // 这 5 秒里换到设定页，三个格子都会调它
    expect(born.length).toBe(2)

    vi.advanceTimersByTime(5000) // 排着的那次到点
    expect(born.length).toBe(2) // ← 修之前这里是 3

    const alive = born.filter((s) => !s.closed && s !== born[0])
    expect(alive.length).toBe(1)

    // 两条都活着的话，一张图画完 finished 会加两次，三个页面各重拉两遍
    const before = finished.value
    for (const s of alive) s.push({ type: 'ref_done', target: 'c_lao_wang_front' })
    expect(finished.value - before).toBe(1)
  })

  it('断线要把"正在画"全清掉，别让那一格永远转着', async () => {
    const useRefStream = await fresh()
    const { live, pct } = useRefStream()
    born[0].open()
    born[0].push({ type: 'ref_progress', target: 'c_1_front', current: 3, total: 10 })
    expect(live.c_1_front).toBe(true)
    expect(pct.c_1_front).toBe(30)

    born[0].drop()
    expect(live.c_1_front).toBeUndefined()
    expect(pct.c_1_front).toBeUndefined()
  })

  it('画完一张：忘掉这一格，并且叫页面重拉', async () => {
    const useRefStream = await fresh()
    const { live, preview, finished } = useRefStream()
    born[0].open()
    born[0].push({ type: 'ref_preview', target: 'loc_1_empty', image: 'data:x' })
    expect(preview.loc_1_empty).toBe('data:x')

    const before = finished.value
    born[0].push({ type: 'ref_done', target: 'loc_1_empty' })
    expect(live.loc_1_empty).toBeUndefined()
    expect(preview.loc_1_empty).toBeUndefined()
    expect(finished.value - before).toBe(1)
  })
})
