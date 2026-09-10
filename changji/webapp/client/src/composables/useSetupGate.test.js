/**
 * 初始化页的两件事：字节数怎么显示、什么时候把人拦到那一页去。
 *
 * 拦错了的两个方向都很难看：
 *   拦松了——用户进到首页，点什么都跑不了，报的是"本地模型一个都没配"；
 *   拦紧了——引擎没起来时把人钉在初始化页上，而那一页自己也读不到清单，
 *           他连"引擎没起来"这件事都看不到。
 */

import { beforeEach, describe, expect, it, vi } from 'vitest'

import { humanBytes, humanRate } from './useAction.js'

const setupState = vi.fn()
vi.mock('@/api', () => ({ api: { setupState: (...a) => setupState(...a) } }))

// node 环境里没有 localStorage。这几条测的是"跳过"这个标记怎么用，
// 不测浏览器的存储本身，所以拿一个 Map 顶上就够。
const store = new Map()
vi.stubGlobal('localStorage', {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => store.set(k, String(v)),
  removeItem: (k) => store.delete(k),
  clear: () => store.clear(),
})

/** 每条用例都要一份干净的模块状态：那个"问过没有"的标记是模块级的。 */
async function freshGate() {
  vi.resetModules()
  return import('./useSetupGate.js')
}

describe('字节数怎么显示', () => {
  it('用 1000 进制，和硬盘属性、HuggingFace 上写的对得上', () => {
    // 换成 1024 进制的话同一个文件这里写 17.5、别处写 18.8，
    // 用户会以为下漏了一块。
    expect(humanBytes(18779848448)).toBe('18.8 GB')
    expect(humanBytes(9001752960)).toBe('9.0 GB')
    // 三位数就不带小数了：「254 MB」比「253.8 MB」好读，
    // 而这一页上真正要看清的是 GB 那一档。
    expect(humanBytes(253806246)).toBe('254 MB')
    expect(humanBytes(43590826592)).toBe('43.6 GB')
  })

  it('小单位不带小数，0 和脏输入不显示成 NaN', () => {
    expect(humanBytes(512)).toBe('512 B')
    expect(humanBytes(4096)).toBe('4 KB')
    expect(humanBytes(0)).toBe('0 B')
    expect(humanBytes(undefined)).toBe('0 B')
    expect(humanBytes('不是数字')).toBe('0 B')
  })

  it('速度为 0 时返回空串，不是「0 B/s」', () => {
    // 刚开跑那一两秒速度就是 0，显示「0 B/s」会让人以为卡住了。
    expect(humanRate(0)).toBe('')
    expect(humanRate(12400000)).toBe('12.4 MB/s')
  })
})

describe('什么时候把人拦到初始化页', () => {
  beforeEach(() => {
    localStorage.clear()
    setupState.mockReset()
  })

  it('引擎说缺模型就拦', async () => {
    setupState.mockResolvedValue({ needed: true })
    const gate = await freshGate()
    expect(await gate.shouldSetup()).toBe(true)
  })

  it('引擎说不缺就放行', async () => {
    setupState.mockResolvedValue({ needed: false })
    const gate = await freshGate()
    expect(await gate.shouldSetup()).toBe(false)
  })

  it('问不到引擎一律放行', async () => {
    // 引擎没起来、接口 404（跑在老的 Node 层后面）都走这一条。
    // 拦住的话用户看不到任何解释，那一页自己也是空的。
    setupState.mockRejectedValue(new Error('连不上'))
    const gate = await freshGate()
    expect(await gate.shouldSetup()).toBe(false)
  })

  it('一个会话只问一次', async () => {
    // 每次切页面都问的话，八步来回走一遍就是十几个请求。
    setupState.mockResolvedValue({ needed: true })
    const gate = await freshGate()
    await gate.shouldSetup()
    await gate.shouldSetup()
    await gate.shouldSetup()
    expect(setupState).toHaveBeenCalledTimes(1)
  })

  it('在那一页上做过决定之后就不再拦', async () => {
    // 「先跳过」和「下完一轮」都算。第二种不算的话，故意只下出图那一套的人
    // 每次打开都会被拦一遍，每次都要再点一次「先跳过」。
    setupState.mockResolvedValue({ needed: true })
    const gate = await freshGate()
    gate.markSetupHandled()
    expect(await gate.shouldSetup()).toBe(false)
  })

  it('正在下的时候一律拦回去，跳过标记也不管用', async () => {
    // 43 GB 要下几个小时，这期间用户多半会关掉浏览器。第二天打开时
    // 最该看到的就是那条进度，而不是一个什么都跑不了的首页。
    setupState.mockResolvedValue({ needed: false, download: { state: 'running' } })
    const gate = await freshGate()
    gate.markSetupHandled()
    expect(await gate.shouldSetup()).toBe(true)
  })

  it('下完之后清一次，下一次导航重新问', async () => {
    // 不清的话，下完点「进入首页」会被缓存下来的"还缺模型"又弹回来。
    setupState.mockResolvedValue({ needed: true })
    const gate = await freshGate()
    expect(await gate.shouldSetup()).toBe(true)
    setupState.mockResolvedValue({ needed: false })
    gate.clearSetupCheck()
    expect(await gate.shouldSetup()).toBe(false)
  })
})
