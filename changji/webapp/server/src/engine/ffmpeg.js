/**
 * ffmpeg 与 ffprobe 的封装。
 *
 * 解析那部分单独拆出来做成纯函数（parseProbe / parseSignalstats 等），
 * 不需要真的装 ffmpeg 就能测——而这些解析恰恰是最容易出错的地方：
 * 字段可能缺、可能是字符串、可能是 "0/0" 这种除不了的帧率。
 */

import { execFile } from 'node:child_process'
import fs from 'node:fs'

export class FFmpegError extends Error {}
export class FFmpegMissing extends FFmpegError {}

export const toFloat = (value) => {
  const n = Number(value)
  return Number.isFinite(n) ? n : 0
}

/** 帧率是 "24000/1001" 这种分数写法。分母是 0 时不能直接除。 */
export function parseFps(rate) {
  const [num, den] = String(rate ?? '').split('/')
  const d = Number(den || 1)
  if (!Number.isFinite(d) || d === 0) return 0
  const n = Number(num)
  return Number.isFinite(n) ? n / d : 0
}

/** 取样位置。避开首尾各 10%，那里常有编码伪影。 */
export function samplePoints(samples) {
  if (samples <= 1) return [0.5]
  const lo = 0.1
  const hi = 0.9
  const step = (hi - lo) / (samples - 1)
  return Array.from({ length: samples }, (_, i) => lo + step * i)
}

/** 一帧的亮度统计。 */
export const pixelStats = ({ mean = 0, spread = 0, minimum = 0, maximum = 0, low = 0, high = 0 }) => ({
  mean, spread, minimum, maximum, low, high,
})

const STAT_KEYS = {
  'lavfi.signalstats.YAVG': 'mean',
  'lavfi.signalstats.YMIN': 'minimum',
  'lavfi.signalstats.YMAX': 'maximum',
  'lavfi.signalstats.YLOW': 'low',
  'lavfi.signalstats.YHIGH': 'high',
}

/**
 * 解析 signalstats 的输出。
 *
 * signalstats 不输出标准差。YDIF 是相邻帧差异，单帧模式下根本不产生，
 * 拿它当标准差会让所有画面都被判成纯色——整条流水线会把每一个镜头
 * 都退回重跑，永远出不了片。用 YLOW 和 YHIGH 这两个百分位数算展布才对。
 */
export function parseSignalstats(text) {
  const found = {}
  for (const line of String(text).split('\n')) {
    const at = line.indexOf('=')
    if (at < 0) continue
    const name = STAT_KEYS[line.slice(0, at).trim()]
    if (name) found[name] = toFloat(line.slice(at + 1))
  }
  if (!('mean' in found)) {
    throw new FFmpegError('signalstats 没有返回亮度统计，可能是文件损坏或没有视频轨')
  }
  const low = found.low ?? found.minimum ?? 0
  const high = found.high ?? found.maximum ?? 0
  return pixelStats({
    mean: found.mean ?? 0,
    spread: Math.max(0, high - low),
    minimum: found.minimum ?? 0,
    maximum: found.maximum ?? 0,
    low,
    high,
  })
}

/** 解析 ffprobe 的 JSON 输出。 */
export function parseProbe(raw, path = '') {
  let data
  try {
    data = typeof raw === 'string' ? JSON.parse(raw) : raw
  } catch {
    throw new FFmpegError(`ffprobe 输出无法解析：${path}`)
  }

  const streams = data.streams ?? []
  const video = streams.find((s) => s.codec_type === 'video') ?? null
  const audio = streams.find((s) => s.codec_type === 'audio') ?? null

  // 有些封装格式的时长只写在流上，不写在容器上
  let duration = toFloat(data.format?.duration)
  if (duration === 0 && video) duration = toFloat(video.duration)

  return {
    path,
    duration_s: duration,
    width: video ? Number(video.width) || 0 : 0,
    height: video ? Number(video.height) || 0 : 0,
    fps: video ? parseFps(video.r_frame_rate ?? '0/1') : 0,
    frames: video ? Number(video.nb_frames) || 0 : 0,
    has_video: video !== null,
    has_audio: audio !== null,
    pix_fmt: video?.pix_fmt ?? '',
    sar: video?.sample_aspect_ratio ?? '',
  }
}

/** 解析 loudnorm 第一遍的输出。两遍归一法要拿它当第二遍的输入。 */
export function parseLoudnorm(text) {
  // loudnorm 把 JSON 打在 stderr 末尾，前面还有一堆日志
  const at = String(text).lastIndexOf('{')
  if (at < 0) throw new FFmpegError('loudnorm 没有返回测量结果')
  try {
    const data = JSON.parse(String(text).slice(at))
    return {
      input_i: toFloat(data.input_i),
      input_tp: toFloat(data.input_tp),
      input_lra: toFloat(data.input_lra),
      input_thresh: toFloat(data.input_thresh),
      target_offset: toFloat(data.target_offset),
    }
  } catch {
    throw new FFmpegError('loudnorm 的测量结果不是合法 JSON')
  }
}

export class FFmpeg {
  constructor({ ffmpeg = 'ffmpeg', ffprobe = 'ffprobe', execImpl = null } = {}) {
    this.ffmpeg = ffmpeg
    this.ffprobe = ffprobe
    this.exec = execImpl ?? defaultExec
  }

  /**
   * 启动时体检。
   *
   * 缺了要在跑之前就说清楚，而不是跑到装配那一步才炸——那时候几十分钟的
   * 渲染已经跑完了。
   */
  async check() {
    const missing = []
    for (const [name, exe] of [['ffmpeg', this.ffmpeg], ['ffprobe', this.ffprobe]]) {
      try {
        await this.exec(exe, ['-version'], 15000)
      } catch {
        missing.push(name)
      }
    }
    if (missing.length) {
      throw new FFmpegMissing(
        `找不到 ${missing.join(' 和 ')}。装配成片要用它们，` +
          `装好之后确认在 PATH 里，或者去设置里填绝对路径。`,
      )
    }
  }

  async available() {
    try {
      await this.check()
      return true
    } catch {
      return false
    }
  }

  runFfmpeg(args, timeoutMs = 1800000) {
    return this.exec(this.ffmpeg, ['-hide_banner', '-nostdin', '-y', ...args], timeoutMs)
  }

  runFfprobe(args, timeoutMs = 120000) {
    return this.exec(this.ffprobe, ['-hide_banner', ...args], timeoutMs)
  }

  async probe(file) {
    if (!fs.existsSync(file)) throw new FFmpegError(`文件不存在：${file}`)
    const { stdout } = await this.runFfprobe([
      '-v', 'error', '-print_format', 'json', '-show_format', '-show_streams', String(file),
    ])
    return parseProbe(stdout, String(file))
  }

  /**
   * 取一帧的亮度统计。
   *
   * 用 ffmpeg 的 signalstats 滤镜算，不在 Node 里解码——那样要拉一个
   * 解码库进来，而这一步只需要几个数字。
   */
  async pixelStats(file, atSecond = null) {
    const args = []
    if (atSecond !== null) args.push('-ss', atSecond.toFixed(3))
    args.push(
      '-i', String(file), '-frames:v', '1',
      '-vf', 'signalstats,metadata=print:file=-',
      '-f', 'null', '-',
    )
    const { stdout, stderr } = await this.runFfmpeg(args, 120000)
    // signalstats 把结果写到 stdout，失败时退回读 stderr
    return parseSignalstats(stdout || stderr)
  }

  /** 在片段的几个位置取样。只看一帧会漏掉中途崩坏的镜头。 */
  async samplePixelStats(file, samples = 3) {
    const info = await this.probe(file)
    if (info.duration_s <= 0) return []
    const out = []
    for (const frac of samplePoints(samples)) {
      try {
        // eslint-disable-next-line no-await-in-loop
        out.push(await this.pixelStats(file, info.duration_s * frac))
      } catch {
        // 某一个取样点读不出来不该让整个镜头判不了，跳过就是
      }
    }
    return out
  }

  /** 测响度。两遍归一法的第一遍。 */
  async measureLoudness(file, target) {
    const filter =
      `loudnorm=I=${target.i}:TP=${target.tp}:LRA=${target.lra}:print_format=json`
    const { stdout, stderr } = await this.runFfmpeg(
      ['-i', String(file), '-af', filter, '-f', 'null', '-'],
      600000,
    )
    return parseLoudnorm(stderr || stdout)
  }
}

function defaultExec(exe, args, timeoutMs) {
  return new Promise((resolve, reject) => {
    execFile(
      exe,
      args,
      { timeout: timeoutMs, maxBuffer: 32 * 1024 * 1024, encoding: 'utf8' },
      (err, stdout, stderr) => {
        if (!err) return resolve({ stdout, stderr })
        if (err.code === 'ENOENT') return reject(new FFmpegMissing(`找不到 ${exe}`))
        if (err.killed) {
          return reject(new FFmpegError(`${exe} 执行超时（${(timeoutMs / 1000).toFixed(0)} 秒）`))
        }
        // 只留最后几行。ffmpeg 的日志很长，前面全是编码参数，
        // 真正的原因在最后
        const tail = String(stderr ?? '').trim().split('\n').slice(-8).join('\n')
        return reject(
          new FFmpegError(`${exe} 执行失败（退出码 ${err.code}）：\n${tail}`),
        )
      },
    )
  })
}
