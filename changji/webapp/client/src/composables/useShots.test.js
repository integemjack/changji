/**
 * 镜头墙上那个「排队中」。
 *
 * 正在跑的时候点某一镜的「重出成片」，不会当场发出去——引擎一次只跑一轮，
 * 发了也是 409。它先记在 `waiting` 里，等这一轮跑完再交。**这中间任何
 * 一步把它丢了，用户那一下点击就悄无声息地没了**：格子上的「排队中」消失，
 * 不报错，也不会再试。
 *
 * ⚠️ **这里要手搓一堆浏览器全局。** 这个项目没装 jsdom（devDeps 里只有
 * vitest / vite / plugin-vue / eslint / globals），而 useShots 一路带出
 * session（读 localStorage）、ui（碰 documentElement）这几个 store。
 * 补齐这几个桩比装一个 jsdom 轻。
 */
import { createPinia, setActivePinia } from 'pinia'
import { nextTick } from 'vue'
import { describe, expect, it, vi } from 'vitest'

const run = vi.fn()
const shots = vi.fn()
const runStatus = vi.fn()
vi.mock('@/api', () => ({
  api: {
    run: (body) => run(body),
    shots: () => shots(),
    runStatus: () => runStatus(),
    seriesStatus: () => Promise.resolve({ running: false }),
    stopRun: () => Promise.resolve({}),
    runPending: () => Promise.resolve({ shots: [] }),
  },
  mediaUrl: () => '',
}))

vi.stubGlobal('WebSocket', class { constructor() { throw new Error('这几条不测连接') } })
const mem = new Map()
vi.stubGlobal('localStorage', {
  getItem: (k) => (mem.has(k) ? mem.get(k) : null),
  setItem: (k, v) => mem.set(k, String(v)),
  removeItem: (k) => mem.delete(k),
})
vi.stubGlobal('document', {
  addEventListener() {},
  removeEventListener() {},
  querySelector: () => null,
  documentElement: { setAttribute() {}, removeAttribute() {} },
})
vi.stubGlobal('window', {
  location: { protocol: 'http:', host: 'x' },
  addEventListener() {},
  removeEventListener() {},
  // useShots 跑完会 session.refresh()，这一趟在测试里必然失败（api 那边
  // 没桩 flow），而 session 的 catch 会广播 changji:error 给 ToastStack。
  // 少这一个方法，vitest 报「2 unhandled errors」——用例还是绿的，噪声
  // 却盖在真失败上面。
  dispatchEvent() {},
})

const { useShots } = await import('./useShots.js')
const { useRun } = await import('@/stores/run.js')
const { useSession } = await import('@/stores/session.js')

/**
 * 每条用例换一部剧。
 *
 * **`sent` / `waiting` 是模块级的**（有意的：切页面回来时那些「排队中」
 * 还要在），只有「项目::集号」变了才清空。两条用例都用空串的话，第一条
 * 留下的东西会飘到第二条上——我第一版就栽在这儿，单独跑过、一起跑挂。
 */
let n = 0
function freshWall() {
  setActivePinia(createPinia())
  n += 1
  const session = useSession()
  session.selectProject(`/p${n}`)
  session.selectEpisode('ep01')
  return { runStore: useRun(), wall: useShots() }
}

/** 等 watch 回调和它里面那串 await 走完。 */
const settle = async () => {
  await nextTick()
  await nextTick()
  await Promise.resolve()
}

describe('排队中的重出', () => {
  it('交不出去的时候要放回队列，不能悄悄丢掉', async () => {
    shots.mockResolvedValue({ shots: [{ shot_id: 's1', status: 'planned', order: 0 }] })
    runStatus.mockResolvedValue({ running: true, events: [] })
    run.mockReset()

    const { runStore, wall } = freshWall()
    await runStore.poll()
    expect(runStore.running).toBe(true)

    // 跑着的时候点「重出成片」——不发出去，先排上
    await wall.shotAction({ shot_id: 's1' }, 'final')
    expect(wall.isWaiting('s1', 'final')).toBe(true)
    expect(run).not.toHaveBeenCalled()

    // 这一轮跑完，交出去——而引擎回 409（别处又起了一轮）
    run.mockRejectedValue(Object.assign(new Error('已经在跑了'), { status: 409 }))
    runStatus.mockResolvedValue({ running: false, events: [] })
    await runStore.poll()
    await settle()
    expect(run).toHaveBeenCalledTimes(1)
    // ← 修之前这里是 false：摘出来交出去，砸了就再也没人管
    expect(wall.isWaiting('s1', 'final')).toBe(true)

    // 下一轮跑完再交一次，这回成了，才出队
    run.mockReset()
    run.mockResolvedValue({ started: true })
    runStatus.mockResolvedValue({ running: true, events: [] })
    await runStore.poll()
    runStatus.mockResolvedValue({ running: false, events: [] })
    await runStore.poll()
    await settle()
    expect(run).toHaveBeenCalledTimes(1)
    expect(wall.isWaiting('s1', 'final')).toBe(false)
  })

  /**
   * 一镜可以**同时**在队列里、又被引擎接手：跑着别的镜头时点了它的「重出
   * 成片」（进 waiting），跑着跑着引擎自己走到了它（进 inflight）。
   *
   * 那时候两条分支都成立，而 `shotAction` 第一条判的是"正在跑"——按下去
   * 是 `stop()`，停的是**整轮**。按钮要是还写着「取消排队」，人就是照着
   * 一句"取消一次重出"把一轮几十分钟到几小时的渲染停了。
   */
  it('既在排队又被引擎接手：按钮得说「停下这一轮」，那才是按下去会发生的事', async () => {
    shots.mockResolvedValue({ shots: [{ shot_id: 's1', status: 'planned', order: 0 }] })
    runStatus.mockResolvedValue({ running: true, events: [] })
    run.mockReset()

    const { runStore, wall } = freshWall()
    await runStore.poll()

    await wall.shotAction({ shot_id: 's1' }, 'final')
    expect(wall.isWaiting('s1', 'final')).toBe(true)

    // 引擎走到了这一镜
    runStore.applyMessage({
      type: 'progress', job_id: 'run-1', stage: 'final',
      shot_id: 's1', step: 1, total: 20, message: '出成片',
    })
    expect(wall.shotRunning('s1')).toBe(true)

    const btn = wall.stepBtn({ shot_id: 's1' }, { id: 'final', label: '成片', icon: 'film' })
    // ← 改之前这里是「取消排队」：先判的排队，而按下去走的是 stop()
    expect(btn.label).toBe('停下这一轮')
    expect(btn.icon).toBe('pause')
  })

  it('再点一下就是取消排队', async () => {
    shots.mockResolvedValue({ shots: [{ shot_id: 's1', status: 'planned', order: 0 }] })
    runStatus.mockResolvedValue({ running: true, events: [] })
    run.mockReset()

    const { runStore, wall } = freshWall()
    await runStore.poll()

    await wall.shotAction({ shot_id: 's1' }, 'frames')
    expect(wall.isWaiting('s1', 'frames')).toBe(true)
    await wall.shotAction({ shot_id: 's1' }, 'frames')
    expect(wall.isWaiting('s1', 'frames')).toBe(false)
  })
})
