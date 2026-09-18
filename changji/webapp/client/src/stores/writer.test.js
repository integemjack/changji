/**
 * 全片剧本那一趟**整个炸了**的时候，屏幕上到底说不说话。
 *
 * 引擎分得很清：单章写砸记在 `episodes[]` 里每一条自己的 error（故事页
 * 左栏那一章后面写「砸了」），而整趟活儿挂掉——盘满了、story.json 写不
 * 进去、引擎半路重启——记在 job 级那个 `error` 上。后一种在 2026-09-15
 * 之前**一处都没人读**：章节列表照常重读一遍、看着一切正常，而十六章
 * 一章都没写。一趟全片是一个钟头起步的活儿。
 *
 * 这条用例盯的是那句话到底发没发出来，以及**发几遍**——见下面「两拍撞
 * 上」那两条。ToastStack 订的是 window 上的 `changji:error`，所以这儿
 * 直接数那个事件，不碰 ui store（两个 store 本来就不该互相认识）。
 */
import { createPinia, setActivePinia } from 'pinia'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const seriesStatus = vi.fn()
vi.mock('@/api', () => ({
  api: {
    seriesStatus: () => seriesStatus(),
  },
}))
// WebSocket 那条在 node 里没有，而 poll 自己就是全量来源，测不到它
vi.mock('@/composables/useJobSocket', () => ({
  openJobSocket: () => ({ close() {} }),
}))

const { useWriter } = await import('./run.js')

/**
 * 这个仓库没装 jsdom，node 里没有 `window`——而 announceFatal 发的正是
 * window 上的事件。给一个只有事件那一小块的替身就够：node 自己的
 * EventTarget 和浏览器同一套语义。做法同 local-storage.test.js 里那两个
 * 假 storage。
 */
const stage = new EventTarget()
Object.defineProperty(globalThis, 'window', { configurable: true, value: stage })

/** 收本轮发出去的那几句话。 */
let said = []
const onSaid = (e) => said.push(e.detail)

const busy = (done = 3) => ({ running: true, done, total: 16, message: '', episodes: [], error: '' })
const dead = (error) => ({ running: false, done: 3, total: 16, message: '', episodes: [], error })

beforeEach(() => {
  setActivePinia(createPinia())
  seriesStatus.mockReset()
  said = []
  window.addEventListener('changji:error', onSaid)
})

afterEach(() => {
  window.removeEventListener('changji:error', onSaid)
})

/** 先让它看见"跑着"，再让它看见 next，中间隔着一整拍。 */
async function runThen(w, next) {
  seriesStatus.mockResolvedValueOnce(busy())
  await w.poll()
  said = []
  seriesStatus.mockResolvedValueOnce(next)
  await w.poll()
}

describe('整批写砸了', () => {
  it('整趟活儿挂掉要说出来', async () => {
    const w = useWriter()
    await runThen(w, dead('磁盘满了，story.json 写不进去'))
    expect(said).toEqual(['批量那一趟没跑完：磁盘满了，story.json 写不进去'])
  })

  it('正常跑完不吭声', async () => {
    const w = useWriter()
    await runThen(w, { running: false, done: 16, total: 16, message: '', episodes: [], error: '' })
    expect(said).toEqual([])
  })

  it('单章写砸不走这条路——那种引擎记在 episodes 里，左栏自己会标', async () => {
    const w = useWriter()
    await runThen(w, {
      running: false, done: 16, total: 16, message: '', error: '',
      episodes: [{ chapter_id: 'ch02', error: '这一章模型没回话' }],
    })
    expect(said).toEqual([])
  })

  it('人自己按的停不算出事：引擎把停的话也写进同一个 error', async () => {
    const w = useWriter()
    seriesStatus.mockResolvedValueOnce(busy())
    await w.poll()
    said = []
    w.markStopped()
    seriesStatus.mockResolvedValueOnce(dead('已手动停止'))
    await w.poll()
    expect(said).toEqual([])
  })

  it('停过一次之后，下一趟真炸了还是要说', async () => {
    const w = useWriter()
    seriesStatus.mockResolvedValueOnce(busy())
    await w.poll()
    w.markStopped()
    seriesStatus.mockResolvedValueOnce(dead('已手动停止'))
    await w.poll()
    said = []
    await runThen(w, dead('引擎半路重启了'))
    expect(said).toEqual(['批量那一趟没跑完：引擎半路重启了'])
  })
})

/**
 * **两拍撞上是常态，不是巧合。**
 *
 * socket 推来终止消息时 applyMessage 会立刻叫一次 poll（episodes 和
 * error 只有全量里才有），而 1.5 秒那个定时器同时也在拉——引擎翻
 * running 的那一瞬正好是两条路一起动的时候。
 *
 * 判据取在发请求之前的话，两拍都会看到"之前跑着、现在停了"。
 */
describe('两拍撞上', () => {
  /** 让两趟请求同时在路上，落地时都是 next。 */
  async function collide(w, next) {
    seriesStatus.mockResolvedValueOnce(busy())
    await w.poll()
    said = []
    let release
    const gate = new Promise((r) => { release = r })
    seriesStatus.mockImplementation(() => gate.then(() => next))
    const a = w.poll()
    const b = w.poll()
    release()
    await Promise.all([a, b])
  }

  it('整批炸了，同一句话只说一遍', async () => {
    const w = useWriter()
    await collide(w, dead('磁盘满了'))
    expect(said).toEqual(['批量那一趟没跑完：磁盘满了'])
  })

  it('人按的停，一个字都不许红', async () => {
    // 判据取快照的写法下这条是红的：第一拍把 stoppedByHand 吃掉了，
    // 第二拍看不见那面旗子，就把「已手动停止」当成事故报出来——
    // 旁边还并排站着 stopWriting() 那句绿的「已停」。
    const w = useWriter()
    seriesStatus.mockResolvedValueOnce(busy())
    await w.poll()
    said = []
    w.markStopped()
    let release
    const gate = new Promise((r) => { release = r })
    seriesStatus.mockImplementation(() => gate.then(() => dead('已手动停止')))
    const a = w.poll()
    const b = w.poll()
    release()
    await Promise.all([a, b])
    expect(said).toEqual([])
  })
})
