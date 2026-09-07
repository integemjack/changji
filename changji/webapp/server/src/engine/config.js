/**
 * 配置。
 *
 * 可移植性的另一半。三条硬规则：
 *
 * 一，安装目录和数据目录彻底分开。程序装在哪都行，项目数据跟着项目走。
 * 二，ComfyUI 是一个 URL，不是一个假设。它可以在本机，也可以在局域网
 *     另一台有显卡的机器上。
 * 三，任何路径都不写死。配置文件里的相对路径一律相对项目根解析。
 *
 * 优先级从高到低：环境变量、项目配置、用户全局配置、内置默认值。
 */

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { parse as parseToml } from 'smol-toml'
import { z } from 'zod'

export const APP_NAME = 'changji'
export const ENV_PREFIX = 'CHANGJI_'

const httpUrl = (v) => {
  const s = String(v).trim().replace(/\/+$/, '')
  if (!/^https?:\/\//.test(s)) {
    throw new Error('ComfyUI 地址必须以 http:// 或 https:// 开头')
  }
  return s
}

/**
 * 布尔值。
 *
 * 不能用 z.coerce.boolean()：它按 JS 的真值规则来，字符串 "false" 会变成
 * true。TOML 里是真布尔没问题，但环境变量和界面提交上来的都是字符串，
 * 「关掉质量闸门」会变成「打开质量闸门」——而这种错要等到废片流到成片里
 * 才会被发现。
 */
const boolish = z.preprocess((v) => {
  if (typeof v === 'string') {
    const s = v.trim().toLowerCase()
    if (['false', '0', 'no', 'off', ''].includes(s)) return false
    if (['true', '1', 'yes', 'on'].includes(s)) return true
  }
  return v
}, z.boolean())

/** ComfyUI 连接。默认本机，但可以指向任意一台机器。 */
export const ComfyConfigSchema = z
  .object({
    base_url: z.preprocess(
      (v) => (v === undefined ? undefined : httpUrl(v)),
      z.string().default('http://127.0.0.1:8188'),
    ),
    timeout_s: z.coerce.number().gt(0).default(60),
    // 提交后等待单个任务完成的上限。成片档一个镜头可能要好几分钟
    job_timeout_s: z.coerce.number().gt(0).default(1800),
    max_retries: z.coerce.number().int().min(0).default(3),
  })
  .strict()

export const comfyWsUrl = (comfy) =>
  `${comfy.base_url.replace(/^http:/, 'ws:').replace(/^https:/, 'wss:')}/ws`

/** 剧本和分镜用的大模型。默认走本地 Ollama。 */
export const LLMConfigSchema = z
  .object({
    base_url: z.preprocess(
      (v) => (v === undefined ? undefined : String(v).trim().replace(/\/+$/, '')),
      z.string().default('http://127.0.0.1:11434/v1'),
    ),
    model: z.string().default('qwen3:14b'),
    api_key: z.string().default('ollama'), // 本地服务通常不校验
    timeout_s: z.coerce.number().gt(0).default(300),
    temperature: z.coerce.number().min(0).max(2).default(0.7),
  })
  .strict()

/** 配音。默认假设通过 ComfyUI 节点调用，也可以指向独立的 HTTP 服务。 */
export const TTSConfigSchema = z
  .object({
    backend: z.string().default('comfy'), // comfy 或 http
    base_url: z.string().nullable().default(null), // backend 为 http 时必填
    engine: z.string().default('cosyvoice3'),
    // 台词时长与镜头时长的允许偏差。超出就要靠尾帧冻结或音频微调吸收
    tolerance_s: z.coerce.number().min(0).default(0.25),
    // 音频变速的安全区。有口型的镜头收得更紧
    max_tempo_shift: z.coerce.number().min(0).max(0.2).default(0.03),
  })
  .strict()

/** 质量闸门的阈值。全自动模式下这些数字决定了废片能不能被拦住。 */
export const GateConfigSchema = z
  .object({
    enabled: boolish.default(true),
    // 闸门一：画面不能是纯色或噪点
    min_pixel_std: z.coerce.number().min(0).default(12),
    min_pixel_mean: z.coerce.number().min(0).default(8),
    max_pixel_mean: z.coerce.number().min(0).default(247),
    // 闸门二：与首帧的结构相似度下限
    min_frame_similarity: z.coerce.number().min(0).max(1).default(0.55),
    // 闸门三：台词落点与分镜的最大偏差
    max_audio_drift_s: z.coerce.number().gt(0).default(0.15),
    target_lufs: z.coerce.number().default(-16),
    max_true_peak_db: z.coerce.number().default(-1.5),
    // 重试策略
    max_attempts_per_shot: z.coerce.number().int().min(1).default(3),
    // 重试超限时降级为静帧加运镜，保证整集能出片
    fallback_on_exhausted: boolish.default(true),
  })
  .strict()

/** 成片装配。 */
export const AssemblyConfigSchema = z
  .object({
    fps: z.coerce.number().int().min(1).max(120).default(24),
    // 统一编码规格。拼接环节最容易踩的坑就是各镜头规格不齐
    pix_fmt: z.string().default('yuv420p'),
    video_codec: z.string().default('libx264'),
    crf: z.coerce.number().int().min(0).max(51).default(18),
    audio_codec: z.string().default('aac'),
    audio_bitrate: z.string().default('192k'),
    // loudnorm 内部按 192k 跑，不显式收回来的话编码器会挑一个 96k
    // 之类的怪采样率。文件白白变大，有些平台还不收。
    audio_sample_rate: z.coerce.number().int().min(8000).max(192000).default(48000),
    audio_channels: z.coerce.number().int().min(1).max(2).default(2),
    // 只在场景切换处用溶解，同场景内一律硬切
    scene_transition_s: z.coerce.number().min(0).max(2).default(0.4),
    // 中文字幕单行上限，全角字符数
    subtitle_max_chars_per_line: z.coerce.number().int().min(6).max(30).default(15),
    subtitle_max_lines: z.coerce.number().int().min(1).max(3).default(2),
    subtitle_font: z.string().default('Source Han Sans SC'),
    ffmpeg_path: z.string().default('ffmpeg'),
    ffprobe_path: z.string().default('ffprobe'),
  })
  .strict()

/**
 * 注意这里用的是 prefault 不是 default。
 *
 * Zod 4 的 .default(值) 在输入缺失时把那个值原样吐出来，不再过一遍 schema。
 * 写成 .default({}) 的话，配置里没写 [assembly] 这一节，settings.assembly
 * 就是一个空对象——settings.assembly.fps 是 undefined，最后传给 ffmpeg 的
 * 是 `-r undefined`。这类空洞不会在解析时报错，要到真跑起来才炸，
 * 而且报的错和配置八竿子打不着。
 *
 * prefault 会把默认值再过一遍 schema，各字段的默认值才填得进去。
 */
export const SettingsSchema = z
  .object({
    comfy: ComfyConfigSchema.prefault({}),
    llm: LLMConfigSchema.prefault({}),
    tts: TTSConfigSchema.prefault({}),
    gates: GateConfigSchema.prefault({}),
    assembly: AssemblyConfigSchema.prefault({}),
    // 显存覆盖。ComfyUI 在别的机器上时本机探测不到，用它手动指定
    vram_gb_override: z.coerce.number().gt(0).nullable().default(null),
    // 项目库根目录。为空则用系统标准数据目录
    workspace: z.string().nullable().default(null),
  })
  .strict()

/** 跨平台的用户配置目录。和 platformdirs 的选择保持一致。 */
export function userConfigDir() {
  if (process.platform === 'win32') {
    return path.join(process.env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming'), APP_NAME)
  }
  if (process.platform === 'darwin') {
    return path.join(os.homedir(), 'Library', 'Application Support', APP_NAME)
  }
  return path.join(
    process.env.XDG_CONFIG_HOME || path.join(os.homedir(), '.config'),
    APP_NAME,
  )
}

function userDataDir() {
  if (process.platform === 'win32') {
    return path.join(
      process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'),
      APP_NAME,
    )
  }
  if (process.platform === 'darwin') {
    return path.join(os.homedir(), 'Library', 'Application Support', APP_NAME)
  }
  return path.join(
    process.env.XDG_DATA_HOME || path.join(os.homedir(), '.local', 'share'),
    APP_NAME,
  )
}

export const userConfigPath = () => path.join(userConfigDir(), 'config.toml')

/** 项目库根目录。没设就用系统标准数据目录。 */
export function workspacePath(settings) {
  if (settings.workspace) {
    return path.resolve(expandHome(settings.workspace))
  }
  return path.join(userDataDir(), 'projects')
}

function expandHome(p) {
  return p.startsWith('~') ? path.join(os.homedir(), p.slice(1)) : p
}

/**
 * 环境变量到配置项的映射。
 *
 * CHANGJI_COMFY_BASE_URL 映射到 comfy.base_url，以此类推。
 * 容器化部署和 CI 里这是最方便的注入方式。
 */
const ENV_MAPPING = {
  COMFY_BASE_URL: ['comfy', 'base_url'],
  COMFY_TIMEOUT_S: ['comfy', 'timeout_s'],
  LLM_BASE_URL: ['llm', 'base_url'],
  LLM_MODEL: ['llm', 'model'],
  LLM_API_KEY: ['llm', 'api_key'],
  TTS_BASE_URL: ['tts', 'base_url'],
  WORKSPACE: ['workspace'],
  VRAM_GB: ['vram_gb_override'],
  FFMPEG_PATH: ['assembly', 'ffmpeg_path'],
}

function envOverrides(env = process.env) {
  const out = {}
  for (const [suffix, keyPath] of Object.entries(ENV_MAPPING)) {
    const raw = env[ENV_PREFIX + suffix]
    if (raw === undefined || raw === '') continue
    let cursor = out
    for (const key of keyPath.slice(0, -1)) {
      cursor[key] = cursor[key] ?? {}
      cursor = cursor[key]
    }
    cursor[keyPath[keyPath.length - 1]] = raw
  }
  return out
}

/**
 * 哪些设置正被环境变量顶着。
 *
 * 环境变量优先级最高。容器里用 compose 注入地址是常态，这时候在界面上
 * 改配置文件是没用的，重启还是环境变量那一套。界面必须把这件事说出来，
 * 否则用户会以为程序没保存。
 */
export function envOverridden(env = process.env) {
  const out = {}
  for (const [suffix, keyPath] of Object.entries(ENV_MAPPING)) {
    const name = ENV_PREFIX + suffix
    if (env[name]) out[keyPath.join('_')] = name
  }
  return out
}

function deepMerge(base, overlay) {
  const out = { ...base }
  for (const [key, value] of Object.entries(overlay)) {
    if (value && typeof value === 'object' && !Array.isArray(value) &&
        out[key] && typeof out[key] === 'object' && !Array.isArray(out[key])) {
      out[key] = deepMerge(out[key], value)
    } else {
      out[key] = value
    }
  }
  return out
}

function readToml(file) {
  if (!fs.existsSync(file)) return {}
  try {
    return parseToml(fs.readFileSync(file, 'utf8'))
  } catch (err) {
    // 配置坏了要说清楚是哪个文件
    throw new Error(`配置文件解析失败：${file}\n${err.message}`)
  }
}

/** 按优先级合并配置：环境变量 > 项目配置 > 用户全局配置 > 默认值。 */
export function loadSettings(projectDir = null, { env = process.env } = {}) {
  let data = {}
  data = deepMerge(data, readToml(userConfigPath()))
  if (projectDir) {
    data = deepMerge(data, readToml(path.join(projectDir, 'changji.toml')))
  }
  data = deepMerge(data, envOverrides(env))
  return SettingsSchema.parse(data)
}

export const DEFAULT_TOML = `# 场记配置文件
# 优先级：环境变量 > 项目目录下的 changji.toml > 本文件 > 内置默认值

# 项目库根目录。留空则用系统标准数据目录。
# 换机器时把项目目录整个拷走即可，程序装在哪都不影响。
# workspace = "D:/短剧项目"

# 显存覆盖。ComfyUI 跑在另一台机器时本机探测不到显卡，用它手动指定。
# vram_gb_override = 16

[comfy]
# ComfyUI 地址。可以是本机，也可以是局域网里任意一台有显卡的机器。
base_url = "http://127.0.0.1:8188"
job_timeout_s = 1800
max_retries = 3

[llm]
# 剧本和分镜用的大模型。默认走本地 Ollama。
# 也可以填任何兼容 OpenAI 接口的服务。
base_url = "http://127.0.0.1:11434/v1"
model = "qwen3:14b"

[tts]
# backend 填 comfy 表示通过 ComfyUI 的 TTS 节点调用，填 http 表示独立服务。
backend = "comfy"
engine = "cosyvoice3"

[gates]
# 质量闸门。全自动模式下这些阈值决定废片能不能被拦住。
enabled = true
max_attempts_per_shot = 3
# 重试超限时降级为静帧加运镜，保证整集能出片而不是卡死。
fallback_on_exhausted = true

[assembly]
fps = 24
crf = 18
# 只在场景切换处用溶解，同场景内一律硬切。
scene_transition_s = 0.4
# 中文字幕单行上限，全角字符数。
subtitle_max_chars_per_line = 15
subtitle_font = "Source Han Sans SC"
`

/** 生成一份带注释的配置模板。首次安装时用。 */
export function writeDefaultConfig(target = null) {
  const file = target ?? userConfigPath()
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, DEFAULT_TOML, 'utf8')
  return file
}

function tomlValue(value) {
  if (typeof value === 'boolean') return value ? 'true' : 'false'
  if (typeof value === 'number') return String(value)
  return JSON.stringify(String(value))
}

/**
 * 把改动写回用户全局配置，保留文件里已有的注释和其它项。
 *
 * 做的是逐行的定点替换，不是「解析成对象再整个序列化回去」。
 * 配置文件里的注释是给人看的——哪个数字调大会误杀暗场、为什么不能换
 * 某个 TTS 引擎——重写一遍就全没了，而那些话比配置本身还值钱。
 */
export function saveUserConfig(patch, target = null) {
  const file = target ?? userConfigPath()
  fs.mkdirSync(path.dirname(file), { recursive: true })
  const text = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : DEFAULT_TOML
  const lines = text.split('\n')

  for (const [section, values] of Object.entries(patch)) {
    if (values === null || typeof values !== 'object' || Array.isArray(values)) {
      setKey(lines, null, section, values)
      continue
    }
    for (const [key, value] of Object.entries(values)) {
      setKey(lines, section, key, value)
    }
  }

  fs.writeFileSync(file, lines.join('\n'), 'utf8')
  return file
}

/** 在指定小节里改一个键；小节或键不存在就补出来。 */
function setKey(lines, section, key, value) {
  const rendered = `${key} = ${tomlValue(value)}`
  const isHeader = (line) => /^\s*\[/.test(line)
  const headerFor = (line) => line.trim().replace(/^\[|\]$/g, '')

  let start = 0
  let end = lines.length
  if (section !== null) {
    const at = lines.findIndex((l) => isHeader(l) && headerFor(l) === section)
    if (at < 0) {
      // 小节不存在，补在文件末尾
      if (lines[lines.length - 1] !== '') lines.push('')
      lines.push(`[${section}]`, rendered)
      return
    }
    start = at + 1
    end = lines.findIndex((l, i) => i > at && isHeader(l))
    if (end < 0) end = lines.length
  } else {
    // 顶层键只能落在第一个小节头之前
    const firstHeader = lines.findIndex(isHeader)
    end = firstHeader < 0 ? lines.length : firstHeader
  }

  const keyRe = new RegExp(`^\\s*#?\\s*${key}\\s*=`)
  for (let i = start; i < end; i += 1) {
    if (keyRe.test(lines[i])) {
      lines[i] = rendered // 注释掉的示例行也会被这一行顶掉，正是想要的
      return
    }
  }
  // 这一节里没有这个键，插在小节末尾
  let insert = end
  while (insert > start && lines[insert - 1].trim() === '') insert -= 1
  lines.splice(insert, 0, rendered)
}
