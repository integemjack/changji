/**
 * 下模型那条轮询。
 *
 * 下几个 GB 的权重是几十分钟的事，而这条轮询是界面上**唯一**知道下到哪儿
 * 的来源。它停了不会报错——进度条就冻在某个数上，而下载还在跑。
 */
import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const setupProgress = vi.fn()
const setupState = vi.fn()
vi.mock('@/api', () => ({
  api: {
    setupProgress: () => setupProgress(),
    setupState: () => setupState(),
    connections: () => Promise.resolve({}),
    llmModels: () => Promise.resolve({ models: [] }),
    llmProviders: () => Promise.resolve({ providers: [] }),
  },
}))

const { useModels } = await import('./models.js')

const running = (n) => ({ state: 'running', items: [{ downloaded: n, total: 100 }] })

describe('下载轮询', () => {
  beforeEach(() => {
    vi.useFakeTimers()
    setActivePinia(createPinia())
    setupProgress.mockReset()
    setupState.mockReset()
    setupState.mockResolvedValue({
      download: { state: 'running' },
      selected: {},
      recommended: {},
    })
  })

  it('刷新之后也接得上：引擎说在下，load 就把轮询起起来', async () => {
    setupProgress.mockResolvedValue(running(10))
    const m = useModels()
    await m.load()
    await vi.advanceTimersByTimeAsync(1000)
    expect(setupProgress).toHaveBeenCalledTimes(1)
  })

  it('丢一拍不撒手', async () => {
    let n = 0
    setupProgress.mockImplementation(() => {
      n += 1
      return n === 2 ? Promise.reject(new Error('引擎重启中')) : Promise.resolve(running(n))
    })
    const m = useModels()
    await m.load()
    await vi.advanceTimersByTimeAsync(5000)
    // 第二拍砸了；要是一砸就停，这儿会停在 2
    expect(n).toBeGreaterThan(3)
  })

  it('连丢三拍才放手', async () => {
    setupProgress.mockRejectedValue(new Error('连不上'))
    const m = useModels()
    await m.load()
    await vi.advanceTimersByTimeAsync(10000)
    expect(setupProgress).toHaveBeenCalledTimes(3)
  })

  it('下完了就停，并且重读一次状态', async () => {
    setupProgress.mockResolvedValue({ state: 'done', items: [] })
    // 第一次读说还在下（于是起轮询），之后读到的是下完了——真实系统里这
    // 两个接口读的是同一份状态，不会一个说在下、一个说完了。
    let first = true
    setupState.mockImplementation(() => {
      const was = first
      first = false
      return Promise.resolve({
        download: { state: was ? 'running' : 'done' },
        selected: {},
        recommended: {},
      })
    })

    const m = useModels()
    await m.load()
    expect(setupState).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(1000)
    expect(setupState).toHaveBeenCalledTimes(2) // 停下来那一下会再读一遍
    await vi.advanceTimersByTimeAsync(5000)
    expect(setupProgress).toHaveBeenCalledTimes(1) // 真的停了
  })
})
