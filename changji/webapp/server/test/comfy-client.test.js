// ComfyUI 客户端。
//
// 这一层最要紧的是「失败分两类」：提交时的校验失败是确定性的，重试没有
// 意义；执行中的失败可能是偶发的，值得重试。分错了的后果是——本该立刻
// 报错的卡了三轮重试，或者本该重试的一次就放弃。
//
// 全程不碰真的 ComfyUI：fetch 和 WebSocket 都是注入的。

import { describe, expect, it, vi } from 'vitest'

import { ComfyConfigSchema } from '../src/engine/config.js'
import {
  ComfyClient,
  ComfyUnavailable,
  ExecutionError,
  PromptValidationError,
  firstFile,
  jobFiles,
} from '../src/engine/comfy/client.js'

const config = ComfyConfigSchema.parse({
  base_url: 'http://127.0.0.1:8188',
  max_retries: 2,
  timeout_s: 5,
  job_timeout_s: 5,
})

const json = (status, body) => ({
  status,
  json: async () => body,
  text: async () => JSON.stringify(body),
})

/** 一个够用的假 WebSocket：手动把消息推给监听者。 */
class FakeSocket {
  constructor() {
    this.listeners = {}
    setTimeout(() => this.emit('open', {}), 0)
  }

  addEventListener(type, fn) {
    ;(this.listeners[type] ??= []).push(fn)
  }

  emit(type, event) {
    for (const fn of this.listeners[type] ?? []) fn(event)
  }

  send(data) {
    this.emit('message', { data })
  }

  close() {}
}

describe('产出文件', () => {
  it('从各节点的输出里汇总', () => {
    const result = {
      outputs: {
        9: { images: [{ filename: 'a.png' }] },
        12: { images: [{ filename: 'b.mp4', animated: [true] }] },
      },
    }
    expect(jobFiles(result).map((f) => f.filename)).toEqual(['a.png', 'b.mp4'])
    expect(firstFile(result).filename).toBe('a.png')
  })

  it('没有产出时给空', () => {
    expect(jobFiles({ outputs: {} })).toEqual([])
    expect(firstFile({ outputs: {} })).toBeNull()
  })
})

describe('校验失败', () => {
  it('400 抛校验错误，不重试', () => {
    // 工作流引用的模型在服务端不存在，重试三次也还是不存在，
    // 白等三轮退避只是让人更晚看到原因
    const fetchImpl = vi.fn(async () =>
      json(400, { error: { message: '工作流校验失败' }, node_errors: {} }),
    )
    const client = new ComfyClient(config, { fetchImpl })
    return expect(client.run({})).rejects.toThrow(PromptValidationError).then(() => {
      expect(fetchImpl).toHaveBeenCalledTimes(1)
    })
  })

  it('把 not in list 翻译成「服务端上没有这个文件」', async () => {
    // 原样抛出 not in list 的话，用户看不出是自己少下了一个模型
    const fetchImpl = vi.fn(async () =>
      json(400, {
        error: { message: 'Prompt outputs failed validation' },
        node_errors: {
          10: {
            class_type: 'CheckpointLoaderSimple',
            errors: [
              {
                message: "Value not in list: ckpt_name: 'x.safetensors' not in [...]",
                extra_info: { input_name: 'ckpt_name', received_value: 'x.safetensors' },
              },
            ],
          },
        },
      }),
    )
    const client = new ComfyClient(config, { fetchImpl })
    await client.run({}).catch((err) => {
      expect(err.humanSummary()).toContain('服务端上没有这个文件')
      expect(err.humanSummary()).toContain('CheckpointLoaderSimple')
    })
  })

  it('400 但不是 JSON 也当校验失败', async () => {
    const fetchImpl = vi.fn(async () => ({
      status: 400,
      text: async () => '<html>Bad Request</html>',
      json: async () => {
        throw new Error('not json')
      },
    }))
    const client = new ComfyClient(config, { fetchImpl })
    await expect(client.run({})).rejects.toThrow(/提交被拒绝/)
  })
})

describe('连不上', () => {
  it('说清是哪个地址', async () => {
    const fetchImpl = vi.fn(async () => {
      throw new Error('ECONNREFUSED')
    })
    const client = new ComfyClient(config, { fetchImpl })
    await expect(client.systemStats()).rejects.toThrow(ComfyUnavailable)
    await expect(client.systemStats()).rejects.toThrow(/127\.0\.0\.1:8188/)
  })

  it('ping 不抛异常只给真假', async () => {
    const client = new ComfyClient(config, {
      fetchImpl: async () => {
        throw new Error('down')
      },
    })
    expect(await client.ping()).toBe(false)
  })

  it('连不上会重试到配置次数', async () => {
    const fetchImpl = vi.fn(async () => {
      throw new Error('ECONNREFUSED')
    })
    const waited = []
    const client = new ComfyClient(config, {
      fetchImpl,
      sleepImpl: async (ms) => waited.push(ms),
    })
    await expect(client.run({})).rejects.toThrow(/重试 2 次后仍失败/)
    expect(fetchImpl).toHaveBeenCalledTimes(3) // 首次 + 2 次重试
    expect(waited).toEqual([1000, 2000]) // 指数退避
  })
})

describe('节点清单', () => {
  it('查得到某个下拉框的可选值', async () => {
    // 用来在提交之前就发现模型缺失，而不是等流水线跑到那一步才炸
    const fetchImpl = vi.fn(async () =>
      json(200, {
        CheckpointLoaderSimple: {
          input: { required: { ckpt_name: [['a.safetensors', 'b.safetensors']] } },
        },
      }),
    )
    const client = new ComfyClient(config, { fetchImpl })
    expect(await client.availableModels('CheckpointLoaderSimple', 'ckpt_name')).toEqual([
      'a.safetensors',
      'b.safetensors',
    ])
  })

  it('节点不存在时给空数组', async () => {
    const client = new ComfyClient(config, { fetchImpl: async () => json(200, {}) })
    expect(await client.availableModels('NotThere', 'x')).toEqual([])
  })

  it('节点定义会缓存', async () => {
    const fetchImpl = vi.fn(async () => json(200, {}))
    const client = new ComfyClient(config, { fetchImpl })
    await client.objectInfo()
    await client.objectInfo()
    expect(fetchImpl).toHaveBeenCalledTimes(1)
  })
})

describe('等待任务', () => {
  const runWith = async (handle) => {
    const sockets = []
    const fetchImpl = vi.fn(async (url) => {
      if (String(url).includes('/prompt')) return json(200, { prompt_id: 'p1' })
      if (String(url).includes('/history/')) {
        return json(200, { p1: { status: { completed: true }, outputs: { 9: { images: [{ filename: 'a.png' }] } } } })
      }
      return json(200, {})
    })
    const client = new ComfyClient(config, {
      fetchImpl,
      WebSocketImpl: class extends FakeSocket {
        constructor() {
          super()
          sockets.push(this)
          setTimeout(() => handle(this), 5)
        }
      },
    })
    return { client, fetchImpl, sockets }
  }

  it('收到 executing:null 就去历史取产出', async () => {
    const { client } = await runWith((ws) => {
      ws.send(JSON.stringify({ type: 'executing', data: { prompt_id: 'p1', node: null } }))
    })
    const result = await client.run({})
    expect(firstFile(result).filename).toBe('a.png')
  })

  it('进度回调带上百分比', async () => {
    const { client } = await runWith((ws) => {
      ws.send(JSON.stringify({ type: 'progress', data: { prompt_id: 'p1', value: 5, max: 20 } }))
      setTimeout(
        () => ws.send(JSON.stringify({ type: 'executing', data: { prompt_id: 'p1', node: null } })),
        5,
      )
    })
    const seen = []
    await client.run({}, { onProgress: (p) => seen.push(p) })
    expect(seen[0].fraction).toBeCloseTo(0.25, 6)
    expect(seen[0].step).toBe(5)
  })

  it('执行失败带上是哪个节点', async () => {
    const { client } = await runWith((ws) => {
      ws.send(
        JSON.stringify({
          type: 'execution_error',
          data: { prompt_id: 'p1', node_type: 'KSampler', exception_message: 'CUDA OOM' },
        }),
      )
    })
    await expect(client.run({})).rejects.toThrow(/KSampler.*CUDA OOM/)
  })

  it('别人的任务消息不理会', async () => {
    const { client } = await runWith((ws) => {
      ws.send(JSON.stringify({ type: 'executing', data: { prompt_id: '别人的', node: null } }))
      setTimeout(
        () => ws.send(JSON.stringify({ type: 'executing', data: { prompt_id: 'p1', node: null } })),
        5,
      )
    })
    const result = await client.run({})
    expect(result.prompt_id).toBe('p1')
  })

  it('二进制预览图忽略掉', async () => {
    const { client } = await runWith((ws) => {
      ws.emit('message', { data: new Uint8Array([1, 2, 3]) })
      setTimeout(
        () => ws.send(JSON.stringify({ type: 'executing', data: { prompt_id: 'p1', node: null } })),
        5,
      )
    })
    await expect(client.run({})).resolves.toBeTruthy()
  })

  it('WebSocket 连不上就退回轮询', async () => {
    // WebSocket 不可用不是致命问题。有些反向代理不转发 upgrade，
    // 因此而整条流水线不能用是不合理的
    const fetchImpl = vi.fn(async (url) => {
      if (String(url).includes('/prompt')) return json(200, { prompt_id: 'p1' })
      return json(200, { p1: { status: { completed: true }, outputs: {} } })
    })
    const client = new ComfyClient(config, {
      fetchImpl,
      WebSocketImpl: class {
        constructor() {
          setTimeout(() => this.onError?.({ message: 'no upgrade' }), 0)
        }
        addEventListener(type, fn) {
          if (type === 'error') this.onError = fn
        }
        close() {}
      },
    })
    await expect(client.run({})).resolves.toMatchObject({ prompt_id: 'p1' })
  })
})

describe('每个任务独立的 clientId', () => {
  it('提交时用的不是客户端自己的 id', async () => {
    // ComfyUI 按 clientId 记订阅。同一个 id 上并发跑两个任务，
    // 后连的会把先连的挤下线，先连的永远等不到完成消息，
    // 白等到 job_timeout_s——默认半小时
    const bodies = []
    const fetchImpl = vi.fn(async (url, init) => {
      if (String(url).includes('/prompt')) {
        bodies.push(JSON.parse(init.body))
        return json(200, { prompt_id: 'p1' })
      }
      return json(200, { p1: { status: { completed: true }, outputs: {} } })
    })
    const client = new ComfyClient(config, {
      clientId: 'base',
      fetchImpl,
      WebSocketImpl: class extends FakeSocket {
        constructor() {
          super()
          setTimeout(
            () =>
              this.send(
                JSON.stringify({ type: 'executing', data: { prompt_id: 'p1', node: null } }),
              ),
            5,
          )
        }
      },
    })
    await client.run({})
    expect(bodies[0].client_id).not.toBe('base')
    expect(bodies[0].client_id.startsWith('base-')).toBe(true)
  })
})
