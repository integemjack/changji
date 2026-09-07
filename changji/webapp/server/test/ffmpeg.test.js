// ffmpeg / ffprobe 的输出解析。
//
// 解析拆成纯函数，不装 ffmpeg 也能测——而这些解析恰恰是最容易出错的
// 地方：字段可能缺、可能是字符串、帧率可能是 "0/0" 这种除不了的。
//
// 和 Python 版做过 22 个用例的逐一比对。

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { afterAll, beforeAll, describe, expect, it, vi } from 'vitest'

import {
  FFmpeg,
  FFmpegError,
  FFmpegMissing,
  parseFps,
  parseLoudnorm,
  parseProbe,
  parseSignalstats,
  samplePoints,
  toFloat,
} from '../src/engine/ffmpeg.js'

describe('数值解析', () => {
  it('解不出来给 0 而不是 NaN', () => {
    // NaN 一路传下去，最后表现成一个诡异的时长或分辨率
    expect(toFloat('1.5')).toBe(1.5)
    expect(toFloat('')).toBe(0)
    expect(toFloat('x')).toBe(0)
    expect(toFloat(null)).toBe(0)
  })

  it('帧率是分数写法', () => {
    expect(parseFps('24/1')).toBe(24)
    expect(parseFps('24000/1001')).toBeCloseTo(23.976, 3)
  })

  it('分母是 0 时不能除', () => {
    // 有些流的 r_frame_rate 就是 "0/0"，直接除会得到 Infinity 或 NaN
    expect(parseFps('0/0')).toBe(0)
    expect(parseFps('')).toBe(0)
    expect(parseFps('bad/x')).toBe(0)
  })
})

describe('取样位置', () => {
  it('避开首尾各 10%，那里常有编码伪影', () => {
    expect(samplePoints(3)).toEqual([0.1, 0.5, 0.9])
    expect(samplePoints(1)).toEqual([0.5])
  })
})

describe('signalstats 解析', () => {
  const sig = [
    'frame:0    pts:0       pts_time:0',
    'lavfi.signalstats.YMIN=16',
    'lavfi.signalstats.YLOW=32',
    'lavfi.signalstats.YAVG=118.4',
    'lavfi.signalstats.YHIGH=201',
    'lavfi.signalstats.YMAX=235',
    'lavfi.signalstats.UAVG=127.1',
  ].join('\n')

  it('展布用百分位之差，不用极差', () => {
    // 极差会被单个亮点带偏：几乎全黑但有一个高光点的废图，
    // 极差能到 250，看着完全正常
    const s = parseSignalstats(sig)
    expect(s.spread).toBe(201 - 32)
    expect(s.mean).toBe(118.4)
  })

  it('没有百分位就退回极差', () => {
    const s = parseSignalstats('lavfi.signalstats.YMIN=10\nlavfi.signalstats.YAVG=100\nlavfi.signalstats.YMAX=200')
    expect(s.spread).toBe(190)
  })

  it('没有亮度统计要报错而不是给 0', () => {
    // 给 0 的话所有画面都会被判成纯色，整条流水线把每个镜头
    // 都退回重跑，永远出不了片
    expect(() => parseSignalstats('什么都没有')).toThrow(FFmpegError)
  })

  it('展布不会是负数', () => {
    const s = parseSignalstats('lavfi.signalstats.YAVG=100\nlavfi.signalstats.YLOW=200\nlavfi.signalstats.YHIGH=50')
    expect(s.spread).toBe(0)
  })
})

describe('ffprobe 解析', () => {
  const raw = JSON.stringify({
    format: { duration: '5.041667' },
    streams: [
      {
        codec_type: 'video', width: 704, height: 1280,
        r_frame_rate: '24/1', nb_frames: '121',
        pix_fmt: 'yuv420p', sample_aspect_ratio: '1:1',
      },
      { codec_type: 'audio', codec_name: 'aac' },
    ],
  })

  it('取出时长、分辨率、帧率、轨道', () => {
    const info = parseProbe(raw)
    expect(info.duration_s).toBeCloseTo(5.041667, 6)
    expect([info.width, info.height]).toEqual([704, 1280])
    expect(info.fps).toBe(24)
    expect(info.has_video).toBe(true)
    expect(info.has_audio).toBe(true)
  })

  it('时长只写在流上时也取得到', () => {
    // 有些封装格式的容器里没有时长
    const info = parseProbe(
      JSON.stringify({ format: {}, streams: [{ codec_type: 'video', duration: '3.5' }] }),
    )
    expect(info.duration_s).toBe(3.5)
  })

  it('纯音频文件认得出来', () => {
    const info = parseProbe(JSON.stringify({ format: { duration: '2' }, streams: [{ codec_type: 'audio' }] }))
    expect(info.has_video).toBe(false)
    expect(info.has_audio).toBe(true)
  })

  it('输出不是 JSON 时报清楚是哪个文件', () => {
    expect(() => parseProbe('<html>', 'a.mp4')).toThrow(/a\.mp4/)
  })
})

describe('loudnorm 解析', () => {
  it('从一堆日志末尾抠出 JSON', () => {
    // loudnorm 把测量结果打在 stderr 末尾，前面全是编码日志
    const text = `一堆日志
[Parsed_loudnorm_0 @ 000]
{
  "input_i" : "-23.45",
  "input_tp" : "-2.10",
  "input_lra" : "7.20",
  "input_thresh" : "-33.50",
  "target_offset" : "0.30"
}`
    const m = parseLoudnorm(text)
    expect(m.input_i).toBeCloseTo(-23.45, 6)
    expect(m.target_offset).toBeCloseTo(0.3, 6)
  })

  it('没有结果时报错', () => {
    expect(() => parseLoudnorm('只有日志')).toThrow(FFmpegError)
  })
})

describe('执行', () => {
  // probe 会先看文件在不在，所以这些测试需要一个真实存在的文件。
  // 内容无所谓——ffmpeg 是打桩的。
  let media

  beforeAll(() => {
    media = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'changji-ff-')), 'a.mp4')
    fs.writeFileSync(media, 'not really a video')
  })
  afterAll(() => {
    fs.rmSync(path.dirname(media), { recursive: true, force: true })
  })

  it('找不到程序时说清楚该怎么办', async () => {
    // 缺了要在跑之前就说清楚，而不是跑到装配才炸——
    // 那时候几十分钟的渲染已经跑完了
    const ff = new FFmpeg({
      execImpl: async () => {
        throw new FFmpegMissing('找不到')
      },
    })
    await expect(ff.check()).rejects.toThrow(/找不到 ffmpeg 和 ffprobe/)
    expect(await ff.available()).toBe(false)
  })

  it('两个都在就通过', async () => {
    const ff = new FFmpeg({ execImpl: async () => ({ stdout: 'ffmpeg version 7', stderr: '' }) })
    expect(await ff.available()).toBe(true)
  })

  it('探测会带上标准参数', async () => {
    const execImpl = vi.fn(async () => ({
      stdout: JSON.stringify({ format: { duration: '1' }, streams: [] }),
      stderr: '',
    }))
    const ff = new FFmpeg({ execImpl })
    await ff.probe(media)
    const args = execImpl.mock.calls[0][1]
    expect(args).toContain('-print_format')
    expect(args).toContain('-show_streams')
  })

  it('文件不存在时不去启动进程', async () => {
    const execImpl = vi.fn()
    const ff = new FFmpeg({ execImpl })
    await expect(ff.probe('不存在的文件.mp4')).rejects.toThrow(/文件不存在/)
    expect(execImpl).not.toHaveBeenCalled()
  })

  it('取样时某一点读不出来不影响其余的', async () => {
    // 一个取样点失败就让整个镜头判不了是过度反应
    let call = 0
    const execImpl = vi.fn(async (exe, args) => {
      if (args.includes('-show_streams')) {
        return { stdout: JSON.stringify({ format: { duration: '6' }, streams: [{ codec_type: 'video' }] }), stderr: '' }
      }
      call += 1
      if (call === 2) throw new FFmpegError('读不出来')
      return { stdout: 'lavfi.signalstats.YAVG=100\nlavfi.signalstats.YLOW=20\nlavfi.signalstats.YHIGH=180', stderr: '' }
    })
    const ff = new FFmpeg({ execImpl })
    const stats = await ff.samplePixelStats(media, 3)
    expect(stats.length).toBe(2)
  })
})
