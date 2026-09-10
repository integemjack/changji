/**
 * 换版之后懒加载的页面拿不到了，得整页重载一次，不能停在白屏上。
 * 这里测的是真代码（router/chunk-error.js），不是抄一份。
 */
import { beforeEach, describe, expect, it } from 'vitest'

import {
  clearReloadMark,
  isChunkLoadError,
  RELOADED_KEY,
  shouldAutoReload,
} from './chunk-error'

/** 一个能用的假 storage；`throws` 打开就模拟隐私模式。 */
function fakeStorage({ throws = false } = {}) {
  const m = new Map()
  return {
    getItem: (k) => { if (throws) throw new Error('隐私模式'); return m.has(k) ? m.get(k) : null },
    setItem: (k, v) => { if (throws) throw new Error('隐私模式'); m.set(k, String(v)) },
    removeItem: (k) => { if (throws) throw new Error('隐私模式'); m.delete(k) },
  }
}

describe('认不认得出"资源丢了"', () => {
  it('各家浏览器的原话都要认', () => {
    expect(isChunkLoadError(new TypeError(
      'Failed to fetch dynamically imported module: http://x/assets/ShotsView-a1.js'))).toBe(true)
    expect(isChunkLoadError(new TypeError(
      'error loading dynamically imported module'))).toBe(true)
    expect(isChunkLoadError(new TypeError('Importing a module script failed.'))).toBe(true)
    expect(isChunkLoadError(new Error('Unable to preload CSS for /assets/x.css'))).toBe(true)
  })

  it('别的错不能当成资源丢了', () => {
    // 判宽了的话，一个普通接口错误会触发整页重载，
    // 用户正填着的东西就没了。
    expect(isChunkLoadError(new Error('读不到项目'))).toBe(false)
    expect(isChunkLoadError(new Error('Network request failed'))).toBe(false)
    expect(isChunkLoadError(undefined)).toBe(false)
    expect(isChunkLoadError(null)).toBe(false)
  })
})

describe('一次会话只自动重载一次', () => {
  let store
  beforeEach(() => { store = fakeStorage() })

  it('第一次重载，第二次就老实报错', () => {
    // 不设这道闸的话，资源真没了会一直刷，用户连那句提示都看不到。
    expect(shouldAutoReload(store)).toBe(true)
    expect(shouldAutoReload(store)).toBe(false)
    expect(shouldAutoReload(store)).toBe(false)
  })

  it('导航成功清掉之后，下次换版还能再自动重载', () => {
    expect(shouldAutoReload(store)).toBe(true)
    clearReloadMark(store)
    expect(store.getItem(RELOADED_KEY)).toBe(null)
    expect(shouldAutoReload(store)).toBe(true)
  })

  it('隐私模式下 storage 会抛：当没重载过，宁可多刷一次也别停在白屏', () => {
    const bad = fakeStorage({ throws: true })
    expect(shouldAutoReload(bad)).toBe(true)
    expect(() => clearReloadMark(bad)).not.toThrow()
  })

  it('压根没有 storage 也不能炸', () => {
    expect(shouldAutoReload(undefined)).toBe(true)
    expect(() => clearReloadMark(undefined)).not.toThrow()
  })
})
