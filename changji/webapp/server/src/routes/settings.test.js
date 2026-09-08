/**
 * 设置页那两个归 Node 管的字段。
 *
 * 引擎地址归 Node 管（不然连不上引擎就什么都改不了），其余归引擎管。
 * 所以这两条要是放行了坏值，用户会陷进"改不了、也连不上"的死角——
 * 那时候只能去手改配置文件，而那个文件在哪他多半不知道。
 */

import { describe, expect, it } from 'vitest'
import { planConfigPatch } from './settings.js'

describe('planConfigPatch', () => {
  it('正常的地址和超时都收下', () => {
    const { patch, error } = planConfigPatch({
      engineBaseUrl: 'http://127.0.0.1:8080',
      engineTimeoutMs: 60000,
    })
    expect(error).toBeUndefined()
    expect(patch).toEqual({
      engineBaseUrl: 'http://127.0.0.1:8080',
      engineTimeoutMs: 60000,
    })
  })

  it('地址两头的空白和结尾的斜杠要去掉', () => {
    // 用户从浏览器地址栏复制过来的，十有八九带着结尾的斜杠。
    // 不去掉的话后面拼出来是 //api/xxx。
    const { patch } = planConfigPatch({ engineBaseUrl: '  http://a:8080///  ' })
    expect(patch.engineBaseUrl).toBe('http://a:8080')
  })

  it('地址必须带 http(s) 前缀', () => {
    for (const bad of ['127.0.0.1:8080', 'ftp://a', '', '   ']) {
      expect(planConfigPatch({ engineBaseUrl: bad }).error)
        .toMatch(/http:\/\/ 或 https:\/\//)
    }
  })

  it('超时要是个数，而且不能小于 1000 毫秒', () => {
    for (const bad of ['abc', NaN, Infinity, -1, 0, 999, null]) {
      expect(planConfigPatch({ engineTimeoutMs: bad }).error)
        .toMatch(/至少要 1000 毫秒/)
    }
    expect(planConfigPatch({ engineTimeoutMs: 1000 }).patch.engineTimeoutMs)
      .toBe(1000)
  })

  it('超时是小数就取整', () => {
    expect(planConfigPatch({ engineTimeoutMs: 1500.7 }).patch.engineTimeoutMs)
      .toBe(1501)
  })

  it('什么都没传要说清楚，而不是写一份空的进去', () => {
    expect(planConfigPatch({}).error).toMatch(/没有要改的项/)
    expect(planConfigPatch(undefined).error).toMatch(/没有要改的项/)
  })

  it('引擎那一层的字段当没看见，不报错', () => {
    // 设置页会把整份配置回传，里面本来就带着 ComfyUI 地址、闸门阈值这些。
    // 对它们报错的话，用户改一个引擎地址会被告知"有不认识的字段"。
    const { patch, error } = planConfigPatch({
      engineBaseUrl: 'http://a:1',
      comfyBaseUrl: 'http://b:2',
      gates: { enabled: true },
    })
    expect(error).toBeUndefined()
    expect(patch).toEqual({ engineBaseUrl: 'http://a:1' })
  })
})
