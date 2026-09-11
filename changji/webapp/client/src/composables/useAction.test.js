// run() 的并发规矩。
//
// **这一组是补一个用户报出来的 bug。** 2026-09-11：设定页上点了一个「画」，
// 再点另一个位置的「画」，什么都不会发生——没反应、不报错、按钮也不变灰。
// 根子是 run() 开头那句 `if (busy.value) return undefined`：整页同时只能干
// 一件事，而 `isBusy(key)` 分的只是"哪个按钮显示成忙"，拦人的那一句不认 key。
//
// 出图还是同步的时候看不出来（那几十秒整页本来就动不了），改成异步之后
// 界面是活的，这一下就暴露成"点了没用"。

import { describe, expect, it, vi } from 'vitest'

// **把两个 store 换掉。** 这一组测的是 run() 的并发规矩，一个 DOM 都不碰；
// 而真的 ui store 一开张就要读 localStorage、动 document.documentElement，
// 在 node 环境下直接抛。为这个去装 jsdom 是拿一个几十兆的浏览器实现，
// 去换两个用不上的字段。
vi.mock('@/stores/ui', () => ({
  useUi: () => ({ ok() {}, info() {}, warn() {}, error() {} }),
}))
vi.mock('@/stores/session', () => ({
  useSession: () => ({ async refresh() {} }),
}))

import { useAction } from './useAction'

function deferred() {
  let resolve
  let reject
  const promise = new Promise((res, rej) => {
    resolve = res
    reject = rej
  })
  return { promise, resolve, reject }
}

describe('useAction', () => {
  it('不同的 key 同时跑，互不相干', async () => {
    const { run, isBusy } = useAction()
    const a = deferred()
    const b = deferred()

    const ra = run(() => a.promise, { key: 'gen:front' })
    const rb = run(() => b.promise, { key: 'gen:back' })

    expect(isBusy('gen:front')).toBe(true)
    expect(isBusy('gen:back')).toBe(true)

    a.resolve('甲')
    b.resolve('乙')
    // 第二下**必须真的跑起来**：以前它当场回 undefined，用户看到的是
    // "点了没用"。
    expect(await ra).toBe('甲')
    expect(await rb).toBe('乙')
    expect(isBusy('gen:front')).toBe(false)
    expect(isBusy('gen:back')).toBe(false)
  })

  it('同一个 key 再点还是拦，防手抖连点', async () => {
    const { run } = useAction()
    const a = deferred()
    const fn = vi.fn(() => a.promise)

    const first = run(fn, { key: 'same' })
    const second = run(fn, { key: 'same' })

    expect(await second).toBeUndefined()
    expect(fn).toHaveBeenCalledTimes(1)
    a.resolve('好了')
    expect(await first).toBe('好了')
  })

  it('干完一件之后同一个 key 还能再点', async () => {
    const { run } = useAction()
    expect(await run(() => Promise.resolve(1), { key: 'k' })).toBe(1)
    expect(await run(() => Promise.resolve(2), { key: 'k' })).toBe(2)
  })

  it('一件砸了不影响另一件，而且 key 要放开', async () => {
    const { run, isBusy } = useAction()
    const bad = run(
      () => Promise.reject(new Error('砸了')),
      { key: 'bad', quiet: true },
    )
    const good = run(() => Promise.resolve('好'), { key: 'good' })
    expect(await bad).toBeUndefined()
    expect(await good).toBe('好')
    // 抛异常也要从"在跑"里划掉，不然那个按钮永远点不动了
    expect(isBusy('bad')).toBe(false)
  })

  it('busy 是"有没有任何东西在跑"', async () => {
    const { run, busy } = useAction()
    const a = deferred()
    expect(busy.value).toBe(false)
    const r = run(() => a.promise, { key: 'x' })
    expect(busy.value).toBe(true)
    a.resolve()
    await r
    expect(busy.value).toBe(false)
  })

  it('不给 key 的那些共用一个格子，行为和以前一样', async () => {
    // 页面上还有一批 run() 没带 key。它们之间照旧串行——这是老行为，
    // 刻意留着：没 key 就没法判断两件事是不是同一件。
    const { run } = useAction()
    const a = deferred()
    const fn = vi.fn(() => Promise.resolve('第二件'))
    const first = run(() => a.promise)
    expect(await run(fn)).toBeUndefined()
    expect(fn).not.toHaveBeenCalled()
    a.resolve('第一件')
    expect(await first).toBe('第一件')
  })
})
