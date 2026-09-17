/**
 * 闲着的时候别去问引擎。
 *
 * 顶栏那块思考徽标原来每 6 秒问一次 `/api/tasks`，**哪怕什么都没在跑**。
 * 而"在不在跑"这件事系统表两秒一拍早就推过来了（`stat.jobs`，和这本账
 * 是同一个 `running_work` 拼的）——空着的时候那一趟一定问回空手。
 */
import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const tasks = vi.fn()
const taskThinking = vi.fn()
vi.mock('@/api', () => ({
  api: {
    tasks: () => tasks(),
    taskThinking: (id, from) => taskThinking(id, from),
  },
}))

const { useThinking } = await import('./thinking.js')

describe('思考账本对拍子', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    tasks.mockReset()
    taskThinking.mockReset()
    tasks.mockResolvedValue({ running: [] })
  })

  it('说了闲着就一个请求都不发', async () => {
    const t = useThinking()
    await t.syncFromServer({ idle: true })
    expect(tasks).not.toHaveBeenCalled()
  })

  it('没说闲着就照问', async () => {
    const t = useThinking()
    await t.syncFromServer()
    expect(tasks).toHaveBeenCalledTimes(1)
  })

  it('闲着那一拍要把上一件的残留清掉', async () => {
    // 上一件刚干完：账上已经空了，而徽标里还挂着它的思考。
    const t = useThinking()
    tasks.mockResolvedValue({
      running: [{ id: 7, title: '正在出大纲', thinking: true, seconds: 3 }],
    })
    taskThinking.mockResolvedValue({ start: 0, end: 6, thinking: '想了几个字' })
    await t.syncFromServer()
    expect(t.items.length).toBe(1)

    await t.syncFromServer({ idle: true })
    expect(t.items.length).toBe(0)
    expect(tasks).toHaveBeenCalledTimes(1)   // 第二趟没发请求
  })

  it('socket 上有东西的时候本来就不看账本', async () => {
    const t = useThinking()
    t.start('s1', '写大纲')
    t.push('s1', '在想')
    await t.syncFromServer()
    expect(tasks).not.toHaveBeenCalled()
    expect(t.latest).toBe('在想')
  })
})
