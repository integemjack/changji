/**
 * 不给用存储的浏览器上，这一层要一声不吭地退化，不能往外抛。
 *
 * **这条用例盯的是整个界面的生死。** 读存储最早的两处（stores/session.js、
 * stores/ui.js 的 setup 顶上）跑在 App.vue 挂载之前、ErrorBoundary 外面，
 * 那儿抛一下就是一片白：没有 ToastStack、没有提示、控制台一行。
 * 所以这四个函数一条抛出去的路都不能留——包括**取 storage 那一下本身**，
 * 那正是 Chrome 把 cookie 设成「全部阻止」时抛的地方。
 */
import { afterEach, describe, expect, it, vi } from 'vitest'

import { dropLocal, readLocal, store, writeLocal } from './local-storage.js'

/** 装一个"碰一下就炸"的 localStorage：属性访问那一下就抛。 */
function poison(name = 'localStorage') {
  Object.defineProperty(globalThis, name, {
    configurable: true,
    get() {
      throw new DOMException('Access is denied for this document.', 'SecurityError')
    },
  })
}

/** 装一个能用的假存储。 */
function fake(name = 'localStorage') {
  const map = new Map()
  const s = {
    getItem: (k) => (map.has(k) ? map.get(k) : null),
    setItem: (k, v) => map.set(k, String(v)),
    removeItem: (k) => map.delete(k),
  }
  Object.defineProperty(globalThis, name, { configurable: true, value: s })
  return map
}

afterEach(() => {
  for (const name of ['localStorage', 'sessionStorage']) {
    Object.defineProperty(globalThis, name, { configurable: true, value: undefined })
  }
  vi.unstubAllGlobals()
})

describe('存储不给用的时候', () => {
  it('取 storage 那一下抛，也只是拿到 null', () => {
    poison()
    expect(() => store('local')).not.toThrow()
    expect(store('local')).toBe(null)
  })

  it('读写删三个都不抛，读回 null', () => {
    poison()
    expect(() => readLocal('changji.theme')).not.toThrow()
    expect(readLocal('changji.theme')).toBe(null)
    expect(() => writeLocal('changji.theme', 'dark')).not.toThrow()
    expect(() => dropLocal('changji.theme')).not.toThrow()
  })

  it('sessionStorage 同样', () => {
    poison('sessionStorage')
    expect(store('session')).toBe(null)
  })

  it('整个没有这个属性（Firefox 关掉 dom.storage）也一样', () => {
    expect(store('local')).toBe(null)
    expect(readLocal('x')).toBe(null)
    expect(() => writeLocal('x', '1')).not.toThrow()
  })

  it('扩展塞了个不是 Storage 的东西进来，当没有', () => {
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: { nope: true },
    })
    expect(store('local')).toBe(null)
    expect(readLocal('x')).toBe(null)
  })

  it('只有 setItem 抛（无痕窗口写配额为 0）时，读还是好的', () => {
    const map = fake()
    map.set('changji.theme', 'dark')
    globalThis.localStorage.setItem = () => {
      throw new DOMException('quota', 'QuotaExceededError')
    }
    expect(readLocal('changji.theme')).toBe('dark')
    expect(() => writeLocal('changji.theme', 'light')).not.toThrow()
    // 写不进去就是写不进去，读回来还是老值——这正是"退化成不记得"
    expect(readLocal('changji.theme')).toBe('dark')
  })
})

describe('存储能用的时候，行为和裸着用一模一样', () => {
  it('没存过读回 null，不是空串——调用方拿 null 区分"没存过"和"存了空"', () => {
    fake()
    expect(readLocal('changji.rail')).toBe(null)
    writeLocal('changji.rail', '')
    expect(readLocal('changji.rail')).toBe('')
  })

  it('写了能读回来，删了就没了', () => {
    fake()
    writeLocal('changji.project', '/a/b')
    expect(readLocal('changji.project')).toBe('/a/b')
    dropLocal('changji.project')
    expect(readLocal('changji.project')).toBe(null)
  })

  it('store() 回的就是那个对象，chunk-error 那两个函数直接收', () => {
    fake('sessionStorage')
    expect(store('session')).toBe(globalThis.sessionStorage)
  })
})
