/**
 * Node 侧的配置。
 *
 * 这里只放「Web 平台自己」的事：引擎在哪、上传到哪些平台。
 * 引擎内部的参数（ComfyUI 地址、大模型地址、闸门阈值）一律不在这里存，
 * 那些由 Python 引擎的 changji.toml 管，设置页透传过去改。
 * 两边各存一份的话，改了一边另一边不动，排查起来没完没了。
 *
 * 优先级：环境变量 > 配置文件 > 内置默认值。
 */

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const DEFAULTS = {
  // Python 引擎（changji web）的地址。可以指向局域网另一台机器。
  engineBaseUrl: 'http://127.0.0.1:8000',
  // 引擎单次请求的等待上限。出分镜这类活儿要几分钟，不能按秒算。
  engineTimeoutMs: 600000,
  // 上传目标。凭据不落这里，只存一个引用名，见 routes/publish.js
  publishTargets: [],
  // 上传记录
  publishRecords: [],
}

export function configPath() {
  if (process.env.CHANGJI_WEBAPP_CONFIG) {
    return path.resolve(process.env.CHANGJI_WEBAPP_CONFIG)
  }
  const base =
    process.env.APPDATA ||
    (process.platform === 'darwin'
      ? path.join(os.homedir(), 'Library', 'Application Support')
      : path.join(os.homedir(), '.config'))
  return path.join(base, 'changji', 'webapp.json')
}

function readFile() {
  const file = configPath()
  if (!fs.existsSync(file)) return {}
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'))
  } catch (err) {
    // 配置坏了要说清楚是哪个文件，不然用户只看到一堆默认值不知道为什么
    throw new Error(`配置文件解析失败：${file}\n${err.message}`)
  }
}

function envOverrides() {
  const out = {}
  if (process.env.CHANGJI_ENGINE_URL) {
    out.engineBaseUrl = process.env.CHANGJI_ENGINE_URL
  }
  if (process.env.CHANGJI_ENGINE_TIMEOUT_MS) {
    out.engineTimeoutMs = Number(process.env.CHANGJI_ENGINE_TIMEOUT_MS)
  }
  return out
}

/** 哪些字段正被环境变量顶着。界面要说出来，否则用户以为没保存上。 */
export function envLocked() {
  const out = {}
  if (process.env.CHANGJI_ENGINE_URL) out.engineBaseUrl = 'CHANGJI_ENGINE_URL'
  if (process.env.CHANGJI_ENGINE_TIMEOUT_MS) {
    out.engineTimeoutMs = 'CHANGJI_ENGINE_TIMEOUT_MS'
  }
  return out
}

let cache = null

export function loadConfig() {
  if (cache) return cache
  cache = { ...DEFAULTS, ...readFile(), ...envOverrides() }
  cache.engineBaseUrl = String(cache.engineBaseUrl || '').replace(/\/+$/, '')
  return cache
}

/** 只写传进来的键，其余保持原样。 */
export function saveConfig(patch) {
  const file = configPath()
  const current = { ...DEFAULTS, ...readFile() }
  const next = { ...current, ...patch }
  // 环境变量顶着的字段照样写盘：用户可能是先在界面上改好，
  // 之后把环境变量去掉重启。写了才有意义。
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, JSON.stringify(next, null, 2), 'utf8')
  cache = { ...next, ...envOverrides() }
  cache.engineBaseUrl = String(cache.engineBaseUrl || '').replace(/\/+$/, '')
  return cache
}
