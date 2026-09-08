/**
 * Node 侧配置的测试。
 *
 * **这是 webapp 的第一批测试。** 在这之前 `npm test` 是绿的，
 * 但它绿得没有意义——vitest 带着 `--passWithNoTests`，而一个测试文件都没有，
 * 所以它永远退出 0。那种绿和真绿在 CI 里长得一模一样。
 *
 * 先测 config.js，因为它有一个和 C++ 侧一模一样的隐患：
 * **同一个环境变量要在两个地方各写一遍**——`envOverrides()`（决定值生不生效）
 * 和 `envLocked()`（决定界面把不把输入框置灰）。漏一处不会报错：
 *   只加 envOverrides → 值覆盖了，界面还让人编辑，改完悄悄丢掉
 *   只加 envLocked   → 界面说"被锁住了"，而值其实没被覆盖
 * C++ 那边同样的坑已经踩过一次（见 test_models_config.cpp）。
 */

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const ENV_KEYS = [
  'CHANGJI_WEBAPP_CONFIG',
  'CHANGJI_ENGINE_URL',
  'CHANGJI_ENGINE_TIMEOUT_MS',
]

let saved
let tmpDir

beforeEach(() => {
  saved = {}
  for (const k of ENV_KEYS) saved[k] = process.env[k]
  // 每个用例一个临时配置文件，互不干扰，也不碰用户真正的那一份。
  tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'changji-webapp-test-'))
  process.env.CHANGJI_WEBAPP_CONFIG = path.join(tmpDir, 'webapp.json')
  delete process.env.CHANGJI_ENGINE_URL
  delete process.env.CHANGJI_ENGINE_TIMEOUT_MS
  // config.js 里有个模块级的 cache，不重置的话第二个用例读到第一个的值。
  vi.resetModules()
})

afterEach(() => {
  for (const k of ENV_KEYS) {
    if (saved[k] === undefined) delete process.env[k]
    else process.env[k] = saved[k]
  }
  fs.rmSync(tmpDir, { recursive: true, force: true })
})

describe('环境变量覆盖', () => {
  // 表驱动：**加一个新的环境变量时，只要往这张表里加一行**，
  // 两处漏掉哪一处都会被下面两条断言分别抓出来。
  const CASES = [
    { env: 'CHANGJI_ENGINE_URL', field: 'engineBaseUrl', value: 'http://10.0.0.2:9000', expect: 'http://10.0.0.2:9000' },
    { env: 'CHANGJI_ENGINE_TIMEOUT_MS', field: 'engineTimeoutMs', value: '12345', expect: 12345 },
  ]

  for (const c of CASES) {
    it(`${c.env} 要生效，而且要在 envLocked 里报出来`, async () => {
      process.env[c.env] = c.value
      const { loadConfig, envLocked } = await import('./config.js')

      // 一、值真的被覆盖了（漏在 envOverrides 里的话这条红）
      expect(loadConfig()[c.field]).toBe(c.expect)
      // 二、界面知道它被锁住了（漏在 envLocked 里的话这条红）
      expect(envLocked()[c.field]).toBe(c.env)
    })
  }

  it('没设环境变量时 envLocked 是空的', async () => {
    const { envLocked } = await import('./config.js')
    expect(Object.keys(envLocked())).toHaveLength(0)
  })
})

describe('loadConfig', () => {
  it('引擎地址结尾的斜杠要去掉', async () => {
    // 不去掉的话拼出来的是 http://host//api/xxx。多数服务能容忍，
    // 但反向代理和签名校验不一定，而症状是"某些请求 404"，很难往这儿想。
    process.env.CHANGJI_ENGINE_URL = 'http://127.0.0.1:8080///'
    const { loadConfig } = await import('./config.js')
    expect(loadConfig().engineBaseUrl).toBe('http://127.0.0.1:8080')
  })

  it('配置文件不存在时用默认值，不抛', async () => {
    const { loadConfig } = await import('./config.js')
    expect(loadConfig().engineBaseUrl).toBe('http://127.0.0.1:8080')
    expect(loadConfig().publishTargets).toEqual([])
  })

  it('配置文件坏了要说清楚是哪个文件', async () => {
    // 只说"JSON 解析失败"的话，用户不知道去改哪一个文件——
    // 而这个文件的位置是按平台算出来的，他多半也不知道在哪。
    fs.writeFileSync(process.env.CHANGJI_WEBAPP_CONFIG, '{ 坏的', 'utf8')
    const { loadConfig } = await import('./config.js')
    expect(() => loadConfig()).toThrow(/配置文件解析失败/)
    expect(() => loadConfig()).toThrow(/webapp\.json/)
  })
})

describe('saveConfig', () => {
  it('只写传进来的键，其余保持原样', async () => {
    const { saveConfig, loadConfig } = await import('./config.js')
    saveConfig({ publishTargets: [{ name: '抖音' }] })
    saveConfig({ engineTimeoutMs: 999 })

    const got = loadConfig()
    expect(got.engineTimeoutMs).toBe(999)
    // 第一次写进去的不该被第二次抹掉。
    expect(got.publishTargets).toEqual([{ name: '抖音' }])
  })

  it('环境变量顶着的字段照样写盘', async () => {
    // 用户可能是先在界面上改好，之后把环境变量去掉重启。写了才有意义。
    process.env.CHANGJI_ENGINE_URL = 'http://env-wins:1'
    const { saveConfig } = await import('./config.js')
    const after = saveConfig({ engineBaseUrl: 'http://saved:2' })

    // 当前这一份仍然由环境变量说了算……
    expect(after.engineBaseUrl).toBe('http://env-wins:1')
    // ……但盘上存的是用户填的那个。
    const onDisk = JSON.parse(
      fs.readFileSync(process.env.CHANGJI_WEBAPP_CONFIG, 'utf8'))
    expect(onDisk.engineBaseUrl).toBe('http://saved:2')
  })
})
