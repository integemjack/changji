/**
 * 接口层的报错翻译。
 *
 * **每一页的红字都从这儿出来**，而它原来一条测试都没有。引擎照 pydantic
 * 的形状发 422（`{type, loc, msg, input}`），字段名在 `loc` 里——只取 msg
 * 的话用户看到的是一句没有主语的英文。
 */
import { afterEach, describe, expect, it, vi } from 'vitest'

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
