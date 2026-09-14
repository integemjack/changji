/**
 * 接口层的报错翻译。
 *
 * **每一页的红字都从这儿出来**，而它原来一条测试都没有。引擎照 pydantic
 * 的形状发 422（`{type, loc, msg, input}`），字段名在 `loc` 里——只取 msg
 * 的话用户看到的是一句没有主语的英文。
 */
import { afterEach, describe, expect, it, vi } from 'vitest'

import { readFileSync } from 'node:fs'

import { api } from './index'

function reply(status, body) {
  vi.stubGlobal('fetch', async () => new Response(JSON.stringify(body), { status }))
}
const boom = async () => {
  try {
    await api.projects()
    return null
  } catch (e) {
    return e
  }
}

afterEach(() => vi.unstubAllGlobals())

describe('422 的字段名要说出来', () => {
  it('多传了一个字段，说清是哪个', async () => {
    reply(422, {
      detail: [{
        type: 'extra_forbidden',
        loc: ['body', 'patch.preset'],
        msg: 'Extra inputs are not permitted',
        input: 'x',
      }],
    })
    expect((await boom()).message).toBe('patch.preset：Extra inputs are not permitted')
  })

  it('loc 里的 body / query / path 这一段不给用户看', async () => {
    reply(422, { detail: [{ loc: ['body', 'premise'], msg: 'Field required' }] })
    expect((await boom()).message).toBe('premise：Field required')
  })

  it('几条一起报就用分号串起来', async () => {
    reply(422, { detail: [{ loc: ['body', 'a'], msg: 'one' }, { loc: ['body', 'b'], msg: 'two' }] })
    expect((await boom()).message).toBe('a：one；b：two')
  })
})

describe('别的形状照旧', () => {
  it('detail 是一句话就原样用（引擎的 400 都是这种）', async () => {
    reply(400, { detail: '改动不合法：硬切的转场时长必须为 0' })
    expect((await boom()).message).toBe('改动不合法：硬切的转场时长必须为 0')
  })

  it('没有 loc 就只说那句话', async () => {
    reply(422, { detail: [{ msg: '就一句话' }] })
    expect((await boom()).message).toBe('就一句话')
  })

  it('形状完全陌生时退回状态码，不抛出 [object Object]', async () => {
    reply(500, { detail: { weird: 1 } })
    expect((await boom()).message).toBe('请求失败（500）')
  })

  it('状态码和原始 payload 都带在异常上，调用方要用', async () => {
    reply(409, { detail: '正在跑' })
    const e = await boom()
    expect(e.status).toBe(409)
    expect(e.payload).toEqual({ detail: '正在跑' })
  })
})

/**
 * 拿引擎**录下来的真实错误**跑一遍。
 *
 * `cpp/tests/golden/endpoints_*.json` 是对拍语料，每条都记着某个接口在某种
 * 输入下真正回了什么。这一组把里面所有 4xx / 5xx 的回包喂给这一层，盯一件
 * 事：**引擎明明说了原因，别在界面上退化成「请求失败（422）」**。
 *
 * 这是跨着前后端的契约测试：引擎改了报错形状，或者这一层改了渲染，都会在
 * 这儿断。
 */
describe('引擎录下来的错误都要说得出原因', () => {
  const dir = new URL('../../../../cpp/tests/golden/', import.meta.url)
  const files = [
    'endpoints_planning.json', 'endpoints_episodes.json', 'endpoints_scripting.json',
    'endpoints_shot_edit.json', 'endpoints_asset_edit.json', 'endpoints_readonly.json',
    'endpoints_upload.json', 'endpoints_batch_edit.json',
  ]

  /** 语料里同时记着 Python 基线（status）和 C++ 现在的行为（cpp_status）。 */
  const statusOf = (c) => c.cpp_status ?? c.status ?? 200

  const cases = []
  for (const f of files) {
    let data
    try {
      data = JSON.parse(readFileSync(new URL(f, dir), 'utf-8'))
    } catch {
      continue // 语料没了就跳过，别把前端的测试卡在 cpp 那边
    }
    for (const c of data.cases ?? []) {
      const body = c.response ?? c.body ?? {}
      if (statusOf(c) < 400) continue
      if (!body || typeof body !== 'object' || !('detail' in body)) continue
      cases.push({ name: `${f} / ${c.name}`, status: statusOf(c), body })
    }
  }

  it('语料里确实有错误用例可跑', () => {
    expect(cases.length).toBeGreaterThan(20)
  })

  it('每一条都渲染成人话，不退回「请求失败（N）」', async () => {
    const bad = []
    for (const c of cases) {
      reply(c.status, c.body)
      const err = await boom()
      if (!err.message || /^请求失败（\d+）$/.test(err.message)) {
        bad.push(`${c.name} -> ${err.message}`)
      }
      vi.unstubAllGlobals()
    }
    expect(bad).toEqual([])
  })

  it('422 那些要带上字段名', async () => {
    const withLoc = cases.filter(
      (c) => Array.isArray(c.body.detail) && c.body.detail.some((d) => Array.isArray(d.loc)),
    )
    expect(withLoc.length).toBeGreaterThan(0)
    for (const c of withLoc) {
      reply(c.status, c.body)
      const err = await boom()
      // 和渲染那边剥一样的三段：body / query / path 是 pydantic 用来分
      // 请求哪一部分的，对用户没意义
      const field = c.body.detail[0].loc
        .filter((x) => x !== 'body' && x !== 'query' && x !== 'path')
        .join('.')
      expect(err.message, c.name).toContain(field)
      vi.unstubAllGlobals()
    }
  })
})
