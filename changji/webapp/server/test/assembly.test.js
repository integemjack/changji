// 成片装配：时间线与字幕。
//
// 时间线由分镜表加配音的**真实**时长推出来，不是估算——这是音画对齐的
// 最后一环。字幕时间戳来自同一份数据，所以字幕、配音、画面天然对齐，
// 不需要事后再凑。
//
// ASS 生成和字幕自检与 Python 版做过 11 个用例的逐一比对（含整份 ASS
// 文件内容逐字节相同）。

import path from 'node:path'
import { describe, expect, it } from 'vitest'

import {
  AssemblyError,
  assTime,
  buildAss,
  buildTimeline,
  escapeFilterPath,
  timelineCues,
  timelineDurationS,
  validateCues,
} from '../src/engine/assembly.js'
import { ShotSchema } from '../src/engine/models/shot.js'

const absPath = (rel) => path.posix.join('/proj', rel)

const shot = (over = {}) =>
  ShotSchema.parse({
    shot_id: 'ep01_sh001',
    scene_id: 's1',
    order: 0,
    duration_s: 3,
    video_path: 'shots/final/ep01_sh001.mp4',
    ...over,
  })

const spoken = (texts, durs, over = {}) =>
  shot({
    characters: [{ char_id: 'c_a' }],
    dialogue: texts.map((t, i) => ({
      char_id: 'c_a',
      text: t,
      actual_duration_s: durs[i],
      audio_path: `audio/${over.shot_id ?? 'ep01_sh001'}_${i}.wav`,
    })),
    ...over,
  })

describe('时间线', () => {
  it('镜头首尾相接', () => {
    const tl = buildTimeline(
      [shot({ duration_s: 3 }), shot({ shot_id: 'ep01_sh002', order: 1, duration_s: 5 })],
      { absPath },
    )
    expect(tl.entries.map((e) => e.start_s)).toEqual([0, 3])
    expect(timelineDurationS(tl)).toBe(8)
  })

  it('溶解让两镜重叠，起点往回挪', () => {
    // 不挪的话成片总时长会比计划长出所有转场时长的总和
    const tl = buildTimeline(
      [
        shot({ duration_s: 3 }),
        shot({
          shot_id: 'ep01_sh002', order: 1, duration_s: 5,
          transition_in: 'dissolve', transition_dur_s: 0.4,
        }),
      ],
      { absPath },
    )
    expect(tl.entries[1].start_s).toBeCloseTo(2.6, 6)
  })

  it('第一镜就算有转场也不会挪成负数', () => {
    const tl = buildTimeline(
      [shot({ transition_in: 'fade_in', transition_dur_s: 0.5 })],
      { absPath },
    )
    expect(tl.entries[0].start_s).toBe(0)
  })

  it('字幕时间戳来自配音的真实时长', () => {
    // 这是音画对齐的最后一环。用估算的话，说得快的那句字幕会提前消失
    const tl = buildTimeline([spoken(['第一句', '第二句'], [1.2, 0.8])], { absPath })
    const cues = timelineCues(tl)
    expect(cues.map((c) => [c.start_s, c.end_s])).toEqual([
      [0, 1.2],
      [1.2, 2],
    ])
  })

  it('旁白和对白用不同样式', () => {
    // 观众一眼能分出「谁在说」和「画外的声音」
    const s = shot({ dialogue: [{ char_id: null, text: '三年后', actual_duration_s: 1 }] })
    expect(timelineCues(buildTimeline([s], { absPath }))[0].style).toBe('narration')
  })

  it('没配上音的台词不出字幕', () => {
    // 时长是 0 的话字幕会瞬间闪过，还不如不出
    const s = shot({
      characters: [{ char_id: 'c_a' }],
      dialogue: [{ char_id: 'c_a', text: '没配音', actual_duration_s: null }],
    })
    expect(timelineCues(buildTimeline([s], { absPath }))).toEqual([])
  })

  it('音频路径收集起来给混音用', () => {
    const tl = buildTimeline([spoken(['a', 'b'], [1, 1])], { absPath })
    expect(tl.entries[0].audio_paths.length).toBe(2)
    expect(tl.entries[0].audio_paths[0]).toContain('/proj/audio/')
  })

  it('没有视频的镜头当场拦下', () => {
    // 装到一半才发现缺文件，前面几分钟的转码就白做了
    expect(() => buildTimeline([shot({ video_path: null })], { absPath })).toThrow(
      AssemblyError,
    )
    expect(() => buildTimeline([shot({ video_path: null })], { absPath })).toThrow(
      /ep01_sh001/,
    )
  })
})

describe('ASS 时间格式', () => {
  it('时:分:秒.厘秒', () => {
    expect(assTime(0)).toBe('0:00:00.00')
    expect(assTime(61.25)).toBe('0:01:01.25')
    expect(assTime(3661.005)).toBe('1:01:01.01')
  })

  it('负数夹到零', () => {
    expect(assTime(-1)).toBe('0:00:00.00')
  })

  it('秒数进位到分钟，不出现 60 秒', () => {
    // Python 版在这里会给 0:00:60.00——秒字段取到 60 而分钟没进位，
    // 不是合法的 ASS 时间戳。这里有意与它不一致
    expect(assTime(59.999)).toBe('0:01:00.00')
    expect(assTime(3599.999)).toBe('1:00:00.00')
    expect(assTime(59.994)).toBe('0:00:59.99')
  })
})

describe('ASS 生成', () => {
  const cues = [
    { start_s: 0, end_s: 1.5, text: '我走了', style: 'dialogue' },
    { start_s: 1.5, end_s: 3.2, text: '深夜的便利店里只剩下收银台那一盏灯还亮着', style: 'narration' },
  ]

  it('样式和分辨率写进头部', () => {
    const ass = buildAss(cues, { width: 1080, height: 1920 })
    expect(ass).toContain('PlayResX: 1080')
    expect(ass).toContain('Style: dialogue,')
    expect(ass).toContain('Style: narration,')
  })

  it('断行由我们算好，用 \\N 显式换行', () => {
    // WrapStyle 2 表示只在显式换行符处断行。交给 libass 的话它对中文
    // 只按字符断不按语义断，一句话会在词中间折断
    const ass = buildAss(cues)
    expect(ass).toContain('WrapStyle: 2')
    expect(ass).toMatch(/Dialogue: 0,0:00:01\.50,.*\\N/)
  })

  it('空字幕不写进去', () => {
    const ass = buildAss([{ start_s: 0, end_s: 1, text: '   ', style: 'dialogue' }])
    expect(ass).not.toContain('Dialogue: 0,')
  })

  it('不认识的样式退回 dialogue', () => {
    const ass = buildAss([{ start_s: 0, end_s: 1, text: 'x', style: '瞎写的' }])
    expect(ass).toContain(',dialogue,,')
  })
})

describe('字幕自检', () => {
  it('时间倒挂、空内容、太短都查出来', () => {
    const problems = validateCues([
      { start_s: 6, end_s: 5, text: '倒挂', style: 'dialogue' },
      { start_s: 7, end_s: 9, text: '  ', style: 'dialogue' },
      { start_s: 10, end_s: 10.2, text: '太短', style: 'dialogue' },
    ])
    expect(problems.some((p) => p.includes('倒挂'))).toBe(true)
    expect(problems.some((p) => p.includes('空的'))).toBe(true)
    expect(problems.some((p) => p.includes('看不清'))).toBe(true)
  })

  it('重叠查得出来', () => {
    const problems = validateCues([
      { start_s: 0, end_s: 2, text: '前一句', style: 'dialogue' },
      { start_s: 1.5, end_s: 3, text: '后一句', style: 'dialogue' },
    ])
    expect(problems.some((p) => p.includes('重叠'))).toBe(true)
  })

  it('浮点误差不误报重叠', () => {
    // 时长是浮点累加出来的，严格比较会报出一堆假问题
    const problems = validateCues([
      { start_s: 0, end_s: 1.2, text: '前', style: 'dialogue' },
      { start_s: 1.1999999, end_s: 2, text: '后', style: 'dialogue' },
    ])
    expect(problems).toEqual([])
  })

  it('正常字幕没有问题', () => {
    expect(
      validateCues([{ start_s: 0, end_s: 1.5, text: '我走了', style: 'dialogue' }]),
    ).toEqual([])
  })
})

describe('滤镜路径转义', () => {
  it('Windows 路径的冒号和反斜杠都要处理', () => {
    // 冒号是 ffmpeg 滤镜的参数分隔符，反斜杠是转义符。
    // 直接传进去会解析失败，而且报的错跟路径八竿子打不着
    expect(escapeFilterPath('C:\\AI短剧\\a.ass')).toBe('C\\:/AI短剧/a.ass')
  })

  it('POSIX 路径原样', () => {
    expect(escapeFilterPath('/data/x.ass')).toBe('/data/x.ass')
  })
})
