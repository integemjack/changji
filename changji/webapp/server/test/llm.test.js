// 大模型客户端。
//
// 两件事是这一层的全部价值：约束解码（模型编不出不存在的 id），
// 和把失败翻成人话（地址错 / 模型没拉 / 密钥过期）。两样都测。
//
// 全程不碰真网络：fetch 是注入的。跑测试要先起一个 Ollama 的话，
// 这套测试在 CI 上就永远是黄的。

import { describe, expect, it, vi } from 'vitest'

import { LLMConfigSchema } from '../src/engine/config.js'
import {
  LLMError,
  complete,
  completeJson,
  explainHttpError,
  extractJson,
  listModels,
} from '../src/engine/llm.js'

const config = LLMConfigSchema.parse({
  base_url: 'http://127.0.0.1:11434/v1',
  model: 'qwen3:14b',
})

const reply = (content) => ({
  status: 200,
  json: async () => ({ choices: [{ message: { content } }] }),
  text: async () => JSON.stringify({ choices: [{ message: { content } }] }),
})

const fail = (status, body) => ({
  status,
  json: async () => (typeof body === 'string' ? JSON.parse(body) : body),
  text: async () => (typeof body === 'string' ? body : JSON.stringify(body)),
})

describe('抠 JSON', () => {
  it('干净的 JSON 直接解析', () => {
    expect(extractJson('{"a":1}')).toEqual({ a: 1 })
  })

  it('包了代码块也认', () => {
    // 说了「只输出 JSON」它还是常包一层
    expect(extractJson('```json\n{"a":1}\n```')).toEqual({ a: 1 })
    expect(extractJson('```\n{"a":1}\n```')).toEqual({ a: 1 })
  })

  it('前后带闲话也认', () => {
    expect(extractJson('好的，这是结果：\n{"a":1}\n希望有帮助')).toEqual({ a: 1 })
  })

  it('数组也认', () => {
    expect(extractJson('结果如下 [1,2,3] 完毕')).toEqual([1, 2, 3])
  })

  it('嵌套对象取到完整的那一个', () => {
    expect(extractJson('说明 {"a":{"b":[1,2]}} 结束')).toEqual({ a: { b: [1, 2] } })
  })

  it('真的没有 JSON 就报错并带上原文', () => {
    expect(() => extractJson('我不知道怎么回答')).toThrow(/找不到合法 JSON/)
  })
})

describe('错误翻译', () => {
  it('模型没拉下来时别去怪地址', () => {
    // Ollama 地址对、模型没拉，也回 404。一律说「地址填错了」
    // 会把人支到错误的地方查，比不给提示更费时间
    const msg = explainHttpError(config, 404, {
      error: { message: "model 'qwen3:14b' not found", type: 'not_found_error' },
    })
    expect(msg).toContain('ollama pull qwen3:14b')
    expect(msg).not.toContain('地址填错')
  })

  it('接口不存在才说地址', () => {
    const msg = explainHttpError(config, 404, { detail: 'Not Found' })
    expect(msg).toContain('/v1')
    expect(msg).toContain('地址')
  })

  it('密钥不对', () => {
    expect(explainHttpError(config, 401, {})).toContain('API Key')
    expect(explainHttpError(config, 403, {})).toContain('API Key')
  })

  it('名字写得不合法和模型没拉下来分开说', () => {
    // Ollama 前者回 400「invalid model name」，后者回 404「model not found」。
    // 两种的处理办法不一样：一个是改字，一个是去 pull
    const msg = explainHttpError(config, 400, { error: { message: 'invalid model name' } })
    expect(msg).toContain('不认')
    expect(msg).not.toContain('ollama pull')
  })

  it('限流和服务端错误分开说', () => {
    expect(explainHttpError(config, 429, {})).toContain('太频繁')
    expect(explainHttpError(config, 503, {})).toContain('503')
  })

  it('总是带上当前模型名', () => {
    // 同一个地址上换个模型就好了的情况很常见
    expect(explainHttpError(config, 500, {})).toContain('qwen3:14b')
  })

  it('返回的不是 JSON 也不能自己炸', () => {
    expect(explainHttpError(config, 502, '<html>bad gateway</html>')).toContain('502')
  })
})

describe('补全', () => {
  it('带 schema 时用 json_schema 约束解码', async () => {
    const fetchImpl = vi.fn(async () => reply('{"ok":true}'))
    await complete(config, '写点什么', { schema: { type: 'object' }, fetchImpl })
    const body = JSON.parse(fetchImpl.mock.calls[0][1].body)
    expect(body.response_format.type).toBe('json_schema')
    expect(body.response_format.json_schema.strict).toBe(true)
    expect(body.model).toBe('qwen3:14b')
  })

  it('服务端不认 json_schema 就退回 json_object 再试', async () => {
    // 自建服务和老版本常常只认后者。不退这一步的话，
    // 一个能用的服务会被判成完全不可用
    const fetchImpl = vi
      .fn()
      .mockResolvedValueOnce(fail(400, { error: 'unsupported response_format' }))
      .mockResolvedValueOnce(reply('{"ok":true}'))
    const out = await complete(config, 'x', { schema: { type: 'object' }, fetchImpl })
    expect(out).toBe('{"ok":true}')
    expect(fetchImpl).toHaveBeenCalledTimes(2)
    expect(JSON.parse(fetchImpl.mock.calls[1][1].body).response_format.type).toBe(
      'json_object',
    )
  })

  it('没给 schema 就不重试', async () => {
    const fetchImpl = vi.fn(async () => fail(500, { error: 'boom' }))
    await expect(complete(config, 'x', { fetchImpl })).rejects.toThrow(LLMError)
    expect(fetchImpl).toHaveBeenCalledTimes(1)
  })

  it('404 不重试，免得把「模型没拉」这条线索盖掉', async () => {
    // 真踩过：模型名写错 Ollama 回 404「model not found」，带着
    // json_object 重试撞上另一个 400，最后报出来的是「返回 400」——
    // 唯一有用的那句话没了
    const fetchImpl = vi
      .fn()
      .mockResolvedValueOnce(fail(404, { error: { message: "model 'qwen3:14b' not found" } }))
      .mockResolvedValueOnce(fail(400, { error: 'something else' }))
    await expect(
      complete(config, 'x', { schema: { type: 'object' }, fetchImpl }),
    ).rejects.toThrow(/ollama pull/)
    expect(fetchImpl).toHaveBeenCalledTimes(1)
  })

  it('退回之后还失败就报第一次的错', async () => {
    // 第一次的错更接近真正的原因
    const fetchImpl = vi
      .fn()
      .mockResolvedValueOnce(fail(400, { error: 'response_format not supported' }))
      .mockResolvedValueOnce(fail(400, { error: '别的毛病' }))
    await expect(
      complete(config, 'x', { schema: { type: 'object' }, fetchImpl }),
    ).rejects.toThrow(/response_format not supported/)
  })

  it('连不上时说清是哪个地址', async () => {
    const fetchImpl = vi.fn(async () => {
      throw new Error('ECONNREFUSED')
    })
    await expect(complete(config, 'x', { fetchImpl })).rejects.toThrow(
      /连不上大模型服务（http:\/\/127\.0\.0\.1:11434\/v1）/,
    )
  })

  it('超时单独说，并指路去调超时', async () => {
    const fetchImpl = vi.fn(async () => {
      const err = new Error('aborted')
      err.name = 'AbortError'
      throw err
    })
    await expect(complete(config, 'x', { fetchImpl })).rejects.toThrow(/超时|还没回话/)
  })

  it('返回结构不对时带上原文好排查', async () => {
    const fetchImpl = vi.fn(async () => ({
      status: 200,
      json: async () => ({ nope: 1 }),
      text: async () => '{"nope":1}',
    }))
    await expect(complete(config, 'x', { fetchImpl })).rejects.toThrow(/格式异常/)
  })

  it('completeJson 一步到位', async () => {
    const fetchImpl = vi.fn(async () => reply('```json\n{"shots":[]}\n```'))
    expect(await completeJson(config, 'x', { fetchImpl })).toEqual({ shots: [] })
  })
})

describe('模型列表', () => {
  it('去重并排序', async () => {
    // 同一个模型报两遍，下拉框里会出现两条一样的
    const fetchImpl = vi.fn(async () => ({
      status: 200,
      json: async () => ({ data: [{ id: 'b' }, { id: 'a' }, { id: 'b' }] }),
    }))
    const got = await listModels(config, { fetchImpl })
    expect(got.models).toEqual(['a', 'b'])
    expect(got.current).toBe('qwen3:14b')
  })

  it('连不上时给空列表和原因，不抛异常', async () => {
    // 列不出来不该让设置页整个不能用，退回手打就行
    const fetchImpl = vi.fn(async () => {
      throw new Error('ECONNREFUSED')
    })
    const got = await listModels(config, { fetchImpl })
    expect(got.models).toEqual([])
    expect(got.error).toContain('连不上')
  })

  it('返回 4xx 也给空列表', async () => {
    const fetchImpl = vi.fn(async () => ({ status: 401 }))
    expect((await listModels(config, { fetchImpl })).models).toEqual([])
  })
})
