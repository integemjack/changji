/**
 * 项目库那一份。
 *
 * 为什么单给它写用例：`load()` 没有参数，好几个地方都会叫它（挂载、建完、
 * 删完、改完名、跑完一轮），叠起来很常见——而两趟回来的差别只有"新旧"。
 * 旧那趟后落地的话，**刚删掉的项目会在栏里回来**，而且不会自己消失，要等
 * 下一次触发。引擎那边读每个项目的 project.json 和 story.json（注释写着
 * 「可能有几百 KB」），慢到足以叠上。
 */
import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const projects = vi.fn()
vi.mock('@/api', () => ({
  api: { projects: () => projects() },
}))

const { useProjects } = await import('./projects.js')

const listOf = (...names) => ({
  workspace: '/ws',
  projects: names.map((n) => ({ path: `/ws/${n}`, name: n })),
})

describe('项目库', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    projects.mockReset()
  })

  it('读回来就有了，并且记下工作目录', async () => {
    projects.mockResolvedValue(listOf('a', 'b'))
    const s = useProjects()
    await s.load()
    expect(s.count).toBe(2)
    expect(s.workspace).toBe('/ws')
    expect(s.byPath('/ws/b').name).toBe('b')
    expect(s.loaded).toBe(true)
  })

  it('回来晚了的那趟不许写：删掉的项目不能在栏里回来', async () => {
    const s = useProjects()
    let landOld
    projects.mockImplementationOnce(
      () => new Promise((r) => {
        landOld = () => r(listOf('a', 'b'))
      }),
    )
    const slow = s.load() // 删之前那一趟，卡在路上

    projects.mockResolvedValueOnce(listOf('a')) // b 删掉了
    await s.load()
    expect(s.count).toBe(1)

    landOld()
    await slow
    // 不判过期的话这里会变回 2——而且不会自己消失，要等下一次触发
    expect(s.count).toBe(1)
    expect(s.byPath('/ws/b')).toBe(null)
  })

  it('读不到：手里那份留着，但要把原因说出来', async () => {
    projects.mockResolvedValue(listOf('a'))
    const s = useProjects()
    await s.load()

    projects.mockRejectedValue(new Error('引擎连不上'))
    await s.load()

    // 列表不清空：它是全局的一份，上一份仍然是这台机器上真有的那几个项目
    expect(s.count).toBe(1)
    expect(s.error).toBe('引擎连不上')
    // 转圈要停，否则那条栏永远写着「读取中…」
    expect(s.loading).toBe(false)
    expect(s.loaded).toBe(true)
  })

  it('过期那趟的报错也不算数', async () => {
    const s = useProjects()
    let failOld
    projects.mockImplementationOnce(
      () => new Promise((_, reject) => {
        failOld = () => reject(new Error('上一趟的错'))
      }),
    )
    const slow = s.load()

    projects.mockResolvedValueOnce(listOf('a'))
    await s.load()

    failOld()
    await slow
    expect(s.error).toBe('')
    expect(s.count).toBe(1)
  })
})
