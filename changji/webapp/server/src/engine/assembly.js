/**
 * 成片装配。
 *
 * 时间线由分镜表加配音的**真实**时长推出来，不是估算——这是音画对齐的
 * 最后一环。字幕的时间戳也来自同一份数据，所以字幕、配音、画面三者
 * 天然对齐，不需要事后再去凑。
 *
 * 这个文件只放时间线和字幕生成，都是纯函数。真正调 ffmpeg 拼片的部分
 * 在 pipeline 里，那样这一层不装 ffmpeg 也能测。
 */

import { Transition } from './models/shot.js'
import { displayWidth, wrapChinese } from './subtitles.js'

export class AssemblyError extends Error {}

/**
 * 由分镜表推出时间线。
 *
 * 溶解会让两镜重叠，后一镜的起点要往回挪——不挪的话成片总时长会比
 * 计划长出所有转场时长的总和。
 */
export function buildTimeline(shots, { absPath }) {
  const entries = []
  let cursor = 0

  for (const shot of shots) {
    if (!shot.video_path) {
      throw new AssemblyError(`镜头 ${shot.shot_id} 还没有视频，不能装配`)
    }
    const video = absPath(shot.video_path)

    const overlap = shot.transition_in !== Transition.CUT ? shot.transition_dur_s : 0
    const start = Math.max(0, cursor - overlap)

    const audioPaths = []
    const cues = []
    let speechCursor = start
    for (const line of shot.dialogue) {
      const dur = line.actual_duration_s || 0
      if (line.audio_path) audioPaths.push(absPath(line.audio_path))
      const text = line.text.trim()
      if (text && dur > 0) {
        cues.push({
          start_s: speechCursor,
          end_s: speechCursor + dur,
          text,
          // 旁白和对白用不同的字幕样式：旁白斜体略小，
          // 观众一眼能分出「谁在说」和「画外的声音」
          style: line.char_id === null ? 'narration' : 'dialogue',
        })
      }
      speechCursor += dur
    }

    entries.push({
      shot_id: shot.shot_id,
      video_path: video,
      start_s: start,
      duration_s: shot.duration_s,
      transition_in: shot.transition_in,
      transition_dur_s: shot.transition_dur_s,
      audio_paths: audioPaths,
      cues,
    })
    cursor = start + shot.duration_s
  }

  return { entries }
}

export const timelineDurationS = (timeline) =>
  timeline.entries.reduce((a, e) => a + e.duration_s, 0)

export const timelineCues = (timeline) => timeline.entries.flatMap((e) => e.cues)

export const cueDurationS = (cue) => cue.end_s - cue.start_s

/**
 * ASS 的时间格式：时:分:秒.厘秒。
 *
 * 先把秒数化成厘秒并取整，再拆成时分秒——不能先拆再对秒数四舍五入。
 *
 * Python 版是后者，于是 59.999 秒会输出 `0:00:60.00`：秒字段取到 60，
 * 而分钟没有跟着进位。这不是合法的 ASS 时间戳，libass 遇到它的行为
 * 没有保证。这里有意和 Python 版不一致，输出 `0:01:00.00`。
 */
export function assTime(seconds) {
  const total = Math.round(Math.max(0, seconds) * 100) // 厘秒
  const cs = total % 100
  const allSeconds = (total - cs) / 100
  const s = allSeconds % 60
  const m = Math.floor(allSeconds / 60) % 60
  const h = Math.floor(allSeconds / 3600)
  return (
    `${h}:${String(m).padStart(2, '0')}:` +
    `${String(s).padStart(2, '0')}.${String(cs).padStart(2, '0')}`
  )
}

/**
 * 生成 ASS 字幕文件内容。
 *
 * 用 ASS 不用 SRT，因为 SRT 没法控样式和位置。
 * 竖屏短剧字幕通常放在下方偏上一点，避开平台的界面元素。
 */
export function buildAss(cues, {
  width = 1080,
  height = 1920,
  font = 'Source Han Sans SC',
  fontSize = 54,
  maxCharsPerLine = 15,
  maxLines = 2,
  marginV = 180,
} = {}) {
  const header = `[Script Info]
ScriptType: v4.00+
PlayResX: ${width}
PlayResY: ${height}
WrapStyle: 2
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: dialogue,${font},${fontSize},&H00FFFFFF,&H000000FF,&H00000000,&H64000000,0,0,0,0,100,100,0,0,1,3,1,2,60,60,${marginV},1
Style: narration,${font},${Math.trunc(fontSize * 0.92)},&H00E8E8E8,&H000000FF,&H00000000,&H64000000,0,1,0,0,100,100,0,0,1,3,1,2,60,60,${marginV},1
Style: title,${font},${Math.trunc(fontSize * 1.4)},&H00FFFFFF,&H000000FF,&H00000000,&H96000000,1,0,0,0,100,100,2,0,1,4,2,5,60,60,0,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
`
  const lines = [header]
  // WrapStyle 2 表示只在显式换行符处断行，这正是我们要的：
  // 断点由 wrapChinese 算好，不让渲染库自作主张——libass 对中文
  // 只按字符断不按语义断，一句话会在词中间折断。
  for (const cue of cues) {
    if (!cue.text.trim()) continue
    const body = wrapChinese(cue.text, maxCharsPerLine, maxLines).join('\\N')
    const style = ['dialogue', 'narration', 'title'].includes(cue.style) ? cue.style : 'dialogue'
    lines.push(
      `Dialogue: 0,${assTime(cue.start_s)},${assTime(cue.end_s)},${style},,0,0,0,,${body}`,
    )
  }
  return `${lines.join('\n')}\n`
}

/** 字幕自检。装配后闸门要用。 */
export function validateCues(cues, maxCharsPerLine = 15) {
  const problems = []
  for (const [i, cue] of cues.entries()) {
    if (cue.end_s <= cue.start_s) problems.push(`第 ${i + 1} 条字幕时间倒挂`)
    if (!cue.text.trim()) problems.push(`第 ${i + 1} 条字幕是空的`)
    const dur = cueDurationS(cue)
    if (dur < 0.4) {
      problems.push(`第 ${i + 1} 条字幕只显示 ${dur.toFixed(2)} 秒，看不清`)
    }
    const wrapped = wrapChinese(cue.text, maxCharsPerLine)
    if (wrapped.some((ln) => displayWidth(ln) > maxCharsPerLine * 1.6)) {
      problems.push(`第 ${i + 1} 条字幕断行后仍然超长`)
    }
  }
  for (let i = 0; i + 1 < cues.length; i += 1) {
    const a = cues[i]
    const b = cues[i + 1]
    // 留 0.01 秒容差：时长是浮点累加出来的，严格比较会误报一堆
    if (b.start_s < a.end_s - 0.01) {
      problems.push(`字幕重叠：${a.text.slice(0, 10)} 与 ${b.text.slice(0, 10)}`)
    }
  }
  return problems
}

/**
 * 把路径转成 ffmpeg 滤镜能接受的形式。
 *
 * Windows 上 `C:\x` 里的冒号是滤镜的参数分隔符，反斜杠是转义符，
 * 直接传进去会解析失败——而且报的错跟路径八竿子打不着。
 */
export function escapeFilterPath(p) {
  return String(p).replace(/\\/g, '/').replace(/:/g, '\\:')
}
