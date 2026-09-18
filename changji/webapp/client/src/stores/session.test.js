/**
 * 「现在在做哪部电影的哪一章」这一份。
 *
 * 为什么单给它写用例：整个界面的每一页都从这儿取项目路径和章号，而
 * `refresh()` 是全应用被叫得最勤的一个函数（换项目、换章、每个带
 * `refresh: true` 的动作、还有好几处手动调）。它里面那道**过期闸**一旦漏
 * 掉，后果不是画错一个数——上一部电影的 flow 会连着 `selectEpisode` 一起
 * 落到新这一部头上，还写进 localStorage，于是新这一部的每一页都拿着一个不
 * 属于它的章号去问引擎。这种错在界面上长得像"引擎抽风"，很难联想到这里。
 *
 * ⚠️ 这里要手搓几个浏览器全局：这个项目没装 jsdom（见 useShots.test.js
 * 开头那段），而 session 读 localStorage、报错时往 window 上广播。
 */
import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const flow = vi.fn()
vi.mock('@/api', () => ({
  api: { flow: (...a) => flow(...a) },
}))

const mem = new Map()
vi.stubGlobal('localStorage', {
  getItem: (k) => (mem.has(k) ? mem.get(k) : null),
  setItem: (k, v) => mem.set(k, String(v)),
  removeItem: (k) => mem.delete(k),
})
const dispatched = []
vi.stubGlobal('window', {
  dispatchEvent: (e) => dispatched.push(e),
  addEventListener() {},
  removeEventListener() {},
})
vi.stubGlobal('CustomEvent', class {
  constructor(type, init) {
    this.type = type
    this.detail = init?.detail
  }
})

const { useSession } = await import('./session.js')

const ok = (title, episodeId) => ({
  project: { title, episodes: [{ episode_id: episodeId, title: '', shots: 3 }] },
  episodeId,
  done: {},
  counters: {},
})

describe('session', () => {
  beforeEach(() => {
    mem.clear()
    dispatched.length = 0
    flow.mockReset()
    setActivePinia(createPinia())
  })

  it('换项目就把章号丢掉：旧章号在新项目里一定对不上', () => {
    const s = useSession()
    s.selectProject('/a')
    s.selectEpisode('ep03')
    expect(mem.get('changji.episode')).toBe('ep03')

    s.selectProject('/b')
    expect(s.episodeId).toBe('')
    expect(mem.has('changji.episode')).toBe(false)
    // 手里那份也要一起松开，不然新这一部读回来之前画的是上一部
    expect(s.project).toBe(null)
    expect(s.flow).toBe(null)
  })

  it('引擎挑了哪一章就跟着它，并且记住', async () => {
    const s = useSession()
    s.selectProject('/a')
    flow.mockResolvedValue(ok('A', 'ep01'))
    await s.refresh()
    expect(s.episodeId).toBe('ep01')
    expect(mem.get('changji.episode')).toBe('ep01')
    expect(s.project.title).toBe('A')
  })

  it('回来晚了的那趟不许写：上一部电影的 flow 不能盖到这一部上', async () => {
    const s = useSession()
    let landA
    flow.mockImplementationOnce(
      () => new Promise((r) => {
        landA = () => r(ok('A', 'ep09'))
      }),
    )
    s.selectProject('/a')
    const slow = s.refresh() // A 的那一趟，卡在路上

    s.selectProject('/b')
    flow.mockResolvedValueOnce(ok('B', 'ep01'))
    await s.refresh() // B 的先落地
    expect(s.project.title).toBe('B')
    expect(s.episodeId).toBe('ep01')

    landA()
    await slow
    // 不判过期的话，这三条会全变成 A 的——章号还会被写进 localStorage，
    // 于是 B 的每一页都拿着 ep09 去问引擎。
    expect(s.project.title).toBe('B')
    expect(s.episodeId).toBe('ep01')
    expect(mem.get('changji.episode')).toBe('ep01')
  })

  it('项目被删了（404）：手里那份要倒掉，而且要说出来', async () => {
    const s = useSession()
    s.selectProject('/a')
    flow.mockResolvedValue(ok('A', 'ep01'))
    await s.refresh()

    const boom = new Error('没有这个项目')
    boom.status = 404
    flow.mockRejectedValue(boom)
    await s.refresh()

    expect(s.project).toBe(null)
    expect(s.flow).toBe(null)
    // 只写进一个没人读的 ref 的话，这条路整个是哑的：每一页退成
    // 「还没选到某一章」，而真正的原因一个字都没有。
    expect(dispatched.at(-1).type).toBe('changji:error')
    expect(dispatched.at(-1).detail).toContain('没有这个项目')
  })

  it('引擎抽风（5xx）不倒手里那份：它仍然是这部电影的，只是旧了一点', async () => {
    const s = useSession()
    s.selectProject('/a')
    flow.mockResolvedValue(ok('A', 'ep01'))
    await s.refresh()

    const boom = new Error('引擎忙')
    boom.status = 503
    flow.mockRejectedValue(boom)
    await s.refresh()

    expect(s.project.title).toBe('A')
    expect(dispatched.at(-1).detail).toContain('引擎忙')
  })

  it('过期那趟的报错也不算数：不能把新这一部清掉', async () => {
    const s = useSession()
    let failA
    flow.mockImplementationOnce(
      () => new Promise((_, reject) => {
        const boom = new Error('上一部电影没了')
        boom.status = 404
        failA = () => reject(boom)
      }),
    )
    s.selectProject('/a')
    const slow = s.refresh()

    s.selectProject('/b')
    flow.mockResolvedValueOnce(ok('B', 'ep01'))
    await s.refresh()

    failA()
    await slow
    expect(s.project.title).toBe('B')
    expect(dispatched.some((e) => e.detail?.includes('上一部电影没了'))).toBe(false)
  })
})
