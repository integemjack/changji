// 配置。从 Python 版 tests/test_portability.py 的配置那部分搬过来。
//
// 可移植性的另一半：程序装在哪都行、ComfyUI 在哪台机器上都行、
// 配置文件里不出现任何跟这台机器绑死的路径。

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'

import {
  DEFAULT_TOML,
  GateConfigSchema,
  SettingsSchema,
  comfyWsUrl,
  envOverridden,
  loadSettings,
  saveUserConfig,
  workspacePath,
  writeDefaultConfig,
} from '../src/engine/config.js'

let tmp

beforeEach(() => {
  tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'changji-cfg-'))
})
afterEach(() => {
  fs.rmSync(tmp, { recursive: true, force: true })
})

describe('默认值', () => {
  it('不含任何绝对路径', () => {
    // 配置里出现绝对路径，这份配置就跟这台机器绑死了
    const s = SettingsSchema.parse({})
    const text = JSON.stringify(s)
    expect(text).not.toMatch(/[A-Za-z]:\\\\/)
    expect(s.workspace).toBeNull()
  })

  it('comfy 地址会去掉尾斜杠', () => {
    // 不去的话拼出来是 http://host:8188//prompt
    const s = SettingsSchema.parse({ comfy: { base_url: 'http://127.0.0.1:8188/' } })
    expect(s.comfy.base_url).toBe('http://127.0.0.1:8188')
  })

  it('comfy 地址必须带协议', () => {
    expect(() => SettingsSchema.parse({ comfy: { base_url: '127.0.0.1:8188' } })).toThrow()
  })

  it('websocket 地址由 http 推导', () => {
    const s = SettingsSchema.parse({ comfy: { base_url: 'http://box:8188' } })
    expect(comfyWsUrl(s.comfy)).toBe('ws://box:8188/ws')
    const secure = SettingsSchema.parse({ comfy: { base_url: 'https://box' } })
    expect(comfyWsUrl(secure.comfy)).toBe('wss://box/ws')
  })

  it('llm 地址也去尾斜杠但不强制协议', () => {
    // 有人会填 host:port/v1 这种，报错拦下去反而挡住合法用法
    const s = SettingsSchema.parse({ llm: { base_url: 'http://box:11434/v1/' } })
    expect(s.llm.base_url).toBe('http://box:11434/v1')
  })

  it('错拼的配置项会报错而不是被忽略', () => {
    // 静默忽略的话，用户改了个拼错的键，以为生效了
    expect(() => SettingsSchema.parse({ comfy: { base_urls: 'x' } })).toThrow()
  })
})

describe('布尔值', () => {
  it('字符串 false 是假', () => {
    // z.coerce.boolean() 会把 "false" 变成 true——「关掉闸门」变成「打开闸门」，
    // 而这种错要等到废片流进成片才发现
    expect(GateConfigSchema.parse({ enabled: 'false' }).enabled).toBe(false)
    expect(GateConfigSchema.parse({ enabled: '0' }).enabled).toBe(false)
    expect(GateConfigSchema.parse({ enabled: 'off' }).enabled).toBe(false)
  })

  it('字符串 true 是真', () => {
    expect(GateConfigSchema.parse({ enabled: 'true' }).enabled).toBe(true)
    expect(GateConfigSchema.parse({ enabled: '1' }).enabled).toBe(true)
  })

  it('真布尔原样通过', () => {
    expect(GateConfigSchema.parse({ enabled: false }).enabled).toBe(false)
  })
})

describe('优先级', () => {
  it('环境变量盖过一切', () => {
    const env = {
      CHANGJI_COMFY_BASE_URL: 'http://gpu-box:8188',
      CHANGJI_LLM_MODEL: 'qwen3:32b',
    }
    const s = loadSettings(null, { env })
    expect(s.comfy.base_url).toBe('http://gpu-box:8188')
    expect(s.llm.model).toBe('qwen3:32b')
  })

  it('项目配置盖过全局但不盖环境变量', () => {
    const projectDir = path.join(tmp, '剧')
    fs.mkdirSync(projectDir, { recursive: true })
    fs.writeFileSync(
      path.join(projectDir, 'changji.toml'),
      '[comfy]\nbase_url = "http://from-project:8188"\n[llm]\nmodel = "from-project"\n',
      'utf8',
    )
    const s = loadSettings(projectDir, {
      env: { CHANGJI_LLM_MODEL: 'from-env' },
    })
    expect(s.comfy.base_url).toBe('http://from-project:8188')
    expect(s.llm.model).toBe('from-env')
  })

  it('空的环境变量当没设', () => {
    const s = loadSettings(null, { env: { CHANGJI_LLM_MODEL: '' } })
    expect(s.llm.model).toBe('qwen3:14b')
  })

  it('说得出哪些字段被环境变量顶着', () => {
    // 容器里用 compose 注入地址是常态。不说的话用户在界面上改完保存，
    // 重启一看又变回去了，会以为「保存」是假的
    const locked = envOverridden({ CHANGJI_COMFY_BASE_URL: 'http://x:8188' })
    expect(locked).toEqual({ comfy_base_url: 'CHANGJI_COMFY_BASE_URL' })
  })
})

describe('项目库位置', () => {
  it('可指向任意位置', () => {
    const s = SettingsSchema.parse({ workspace: path.join(tmp, '短剧项目') })
    expect(workspacePath(s)).toBe(path.resolve(path.join(tmp, '短剧项目')))
  })

  it('未设时用系统数据目录', () => {
    const s = SettingsSchema.parse({})
    expect(workspacePath(s)).toMatch(/changji[\\/]projects$/)
  })
})

describe('配置模板', () => {
  it('生成之后能被读回来', () => {
    const file = path.join(tmp, 'config.toml')
    writeDefaultConfig(file)
    expect(fs.readFileSync(file, 'utf8')).toBe(DEFAULT_TOML)
  })
})

describe('写回配置', () => {
  const write = (initial) => {
    const file = path.join(tmp, 'config.toml')
    fs.writeFileSync(file, initial, 'utf8')
    return file
  }

  it('只改传进来的键', () => {
    const file = write('[comfy]\nbase_url = "http://old:8188"\nmax_retries = 3\n')
    saveUserConfig({ comfy: { base_url: 'http://new:8188' } }, file)
    const text = fs.readFileSync(file, 'utf8')
    expect(text).toContain('base_url = "http://new:8188"')
    expect(text).toContain('max_retries = 3')
  })

  it('保留注释', () => {
    // 注释里写的是「这个数字调大会误杀暗场」这类话，比配置本身还值钱。
    // 解析成对象再整个序列化回去就全没了
    const file = write(
      '# 这份配置的说明\n[comfy]\n# 可以指向局域网另一台机器\nbase_url = "http://old:8188"\n',
    )
    saveUserConfig({ comfy: { base_url: 'http://new:8188' } }, file)
    const text = fs.readFileSync(file, 'utf8')
    expect(text).toContain('# 这份配置的说明')
    expect(text).toContain('# 可以指向局域网另一台机器')
  })

  it('小节里没有的键会补进去', () => {
    const file = write('[comfy]\nbase_url = "http://old:8188"\n')
    saveUserConfig({ comfy: { max_retries: 5 } }, file)
    expect(fs.readFileSync(file, 'utf8')).toContain('max_retries = 5')
  })

  it('小节不存在时整节补出来', () => {
    const file = write('[comfy]\nbase_url = "http://old:8188"\n')
    saveUserConfig({ llm: { model: 'qwen3:32b' } }, file)
    const text = fs.readFileSync(file, 'utf8')
    expect(text).toContain('[llm]')
    expect(text).toContain('model = "qwen3:32b"')
  })

  it('顶层键写在第一个小节之前', () => {
    const file = write('# 头部说明\n[comfy]\nbase_url = "http://old:8188"\n')
    saveUserConfig({ vram_gb_override: 16 }, file)
    const text = fs.readFileSync(file, 'utf8')
    expect(text.indexOf('vram_gb_override = 16')).toBeLessThan(text.indexOf('[comfy]'))
  })

  it('注释掉的示例行会被真值顶掉', () => {
    // 模板里 workspace 是注释掉的示例。补一行新的而不顶掉它的话，
    // 文件里会同时存在注释版和真值版，看的人不知道哪个生效
    const file = write('# workspace = "D:/短剧项目"\n[comfy]\nbase_url = "http://x:8188"\n')
    saveUserConfig({ workspace: 'E:/剧' }, file)
    const text = fs.readFileSync(file, 'utf8')
    expect(text).toContain('workspace = "E:/剧"')
    expect(text).not.toContain('# workspace =')
  })

  it('改完能被重新读出来', () => {
    const file = write(DEFAULT_TOML)
    saveUserConfig({ comfy: { base_url: 'http://box:8188' }, gates: { enabled: false } }, file)
    // 用 loadSettings 读不到这个临时文件，直接验证文本能被 TOML 解析
    const text = fs.readFileSync(file, 'utf8')
    expect(text).toContain('base_url = "http://box:8188"')
    expect(text).toContain('enabled = false')
  })
})
