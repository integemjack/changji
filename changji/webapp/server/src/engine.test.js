/**
 * 引擎客户端的测试。
 *
 * 重点是**错误消息**。这一层是前端和引擎之间唯一的翻译，翻坏了用户就只能
 * 看到一句没有信息的话，而引擎那边其实说得很清楚。
 */

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

let tmpDir
let savedFetch
let savedEnv

beforeEach(() => {
  savedEnv = {
    cfg: process.env.CHANGJI_WEBAPP_CONFIG,
    url: process.env.CHANGJI_ENGINE_URL,
  }
  tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'changji-engine-test-'))
  process.env.CHANGJI_WEBAPP_CONFIG = path.join(tmpDir, 'webapp.json')
  process.env.CHANGJI_ENGINE_URL = 'http://engine.test:8080'
  savedFetch = globalThis.fetch
  vi.resetModules()
})

afterEach(() => {
  globalThis.fetch = savedFetch
  if (savedEnv.cfg === undefined) delete process.env.CHANGJI_WEBAPP_CONFIG
  else process.env.CHANGJI_WEBAPP_CONFIG = savedEnv.cfg
  if (savedEnv.url === undefined) delete process.env.CHANGJI_ENGINE_URL
  else process.env.CHANGJI_ENGINE_URL = savedEnv.url
  fs.rmSync(tmpDir, { recursive: true, force: true })
})

/** 让 fetch 回一个指定状态码和 body 的响应。 */
function stubFetch(status, bodyObj) {
  globalThis.fetch = vi.fn(async () => ({
    ok: status >= 200 && status < 300,
    status,
    text: async () => JSON.stringify(bodyObj),
  }))
}

describe('错误消息', () => {
  it('422 的 detail 是数组，要翻成人话，不能是 [object Object]', async () => {
    // **引擎的 422 里 detail 是结构化数组**（对齐 FastAPI 的 pydantic 报错），
    // readonly.hpp 里写着"detail 是一个结构化数组"。
    // 而 `new Error([{...}])` 的 message 是字符串化的 "[object Object]"——
    // 用户在界面上看到的就是这七个字，等于什么都没说。
    //
    // 表单填错是最常撞到的一类错误，这条路必须说得清楚。
    stubFetch(422, {
      detail: [
        {
          loc: ['body', 'patch.tts_backend'],
          msg: 'Extra inputs are not permitted',
          type: 'extra_forbidden',
        },
      ],
    })
    const { callEngine, EngineError } = await import('./engine.js')
    await expect(callEngine('/api/settings', { method: 'POST', body: {} }))
      .rejects.toThrow(EngineError)

    let caught
    try {
      await callEngine('/api/settings', { method: 'POST', body: {} })
    } catch (err) {
      caught = err
    }
    expect(caught.message).not.toContain('[object Object]')
    // 该说清楚是哪个字段、什么毛病。
    expect(caught.message).toContain('patch.tts_backend')
    expect(caught.message).toContain('Extra inputs are not permitted')
    expect(caught.status).toBe(422)
  })

  it('多条 detail 都要带上，不能只报第一条', async () => {
    // 一次提交填错三个字段，只说第一个的话，用户改完再提交又被打回来。
    stubFetch(422, {
      detail: [
        { loc: ['body', 'a'], msg: '第一个毛病' },
        { loc: ['body', 'b'], msg: '第二个毛病' },
      ],
    })
    const { callEngine } = await import('./engine.js')
    let caught
    try {
      await callEngine('/api/x')
    } catch (err) {
      caught = err
    }
    expect(caught.message).toContain('第一个毛病')
    expect(caught.message).toContain('第二个毛病')
  })

  it('detail 是字符串时原样用', async () => {
    stubFetch(404, { detail: '没有角色 c_no_such' })
    const { callEngine } = await import('./engine.js')
    await expect(callEngine('/api/x')).rejects.toThrow('没有角色 c_no_such')
  })

  it('引擎回的不是 JSON 时，把原文带上一截', async () => {
    globalThis.fetch = vi.fn(async () => ({
      ok: false,
      status: 500,
      text: async () => '<html>502 Bad Gateway</html>',
    }))
    const { callEngine } = await import('./engine.js')
    await expect(callEngine('/api/x')).rejects.toThrow(/不是 JSON/)
  })

  it('连不上要说清楚是哪个地址', async () => {
    // 只说"连不上引擎"的话，用户不知道它在找哪台机器——
    // 而这个地址可能是环境变量顶进来的，界面上看不到。
    globalThis.fetch = vi.fn(async () => {
      throw new Error('ECONNREFUSED')
    })
    const { callEngine } = await import('./engine.js')
    await expect(callEngine('/api/x')).rejects.toThrow(/engine\.test:8080/)
  })
})

describe('URL 拼接', () => {
  it('不产生双斜杠', async () => {
    process.env.CHANGJI_ENGINE_URL = 'http://engine.test:8080/'
    stubFetch(200, { ok: true })
    const { callEngine } = await import('./engine.js')
    await callEngine('/api/health')
    expect(globalThis.fetch.mock.calls[0][0]).toBe(
      'http://engine.test:8080/api/health')
  })
})

describe('引擎连不上时那句提示', () => {
  // 这句话是用户排查时唯一的线索。它错了没有任何别的测试会红——
  // 用户 2026-09-10 就问过"为什么我这里引擎显示是连不上的"，
  // 而当时页面上只有一句"引擎离线"，看不出试的是哪个地址。
  it('要把试的那个地址说出来', async () => {
    const { offlineHint } = await import('./engine.js')
    const s = offlineHint('http://127.0.0.1:8080', 'fetch failed')
    expect(s).toMatch(/127\.0\.0\.1:8080/)
    expect(s).toMatch(/fetch failed/)
  })

  it('本机地址：给"起引擎"和"打隧道"两条路', async () => {
    const { offlineHint } = await import('./engine.js')
    const s = offlineHint('http://localhost:8080', 'ECONNREFUSED')
    expect(s).toMatch(/changji --port/)
    expect(s).toMatch(/ssh -L/)
  })

  it('远程地址：不该叫人去本机起引擎', async () => {
    const { offlineHint } = await import('./engine.js')
    const s = offlineHint('http://192.168.1.9:8080', 'timeout')
    expect(s).not.toMatch(/changji --port/)
    expect(s).toMatch(/端口/)
  })

  it('地址没填也要有话说，不能是空的', async () => {
    const { offlineHint } = await import('./engine.js')
    const s = offlineHint('', 'no url')
    expect(s.length).toBeGreaterThan(10)
    expect(s).toMatch(/没填/)
  })
})
