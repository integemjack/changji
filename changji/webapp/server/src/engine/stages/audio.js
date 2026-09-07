/**
 * 配音阶段的时长逻辑。
 *
 * 这一步在生成任何画面之前跑完，拿到每句台词的真实时长，反过来锁定镜头
 * 时长。音画对齐从源头解决，而不是等成片后再想办法把声音塞进去。
 *
 * 这个文件只放不依赖任何外部服务的那部分：时长估算、超长台词切分、
 * 时长反推锁定、镜头拆分。真正出声音的后端在 backends.js。
 */

import { ceilDuration, maxShotDurationS } from '../timing.js'

export class AudioError extends Error {}

/**
 * 中文普通话正常语速的字符数每秒。用于估算，真实值由 TTS 返回。
 * 这个数字来自播音语速的常见区间，短剧对白通常偏快。
 */
export const CHARS_PER_SECOND = 4.6
/** 每句话前后的呼吸留白。 */
export const LEAD_IN_S = 0.15
export const TAIL_S = 0.25

/** 标点不发音但产生停顿：逗号短一点，句号问号感叹号长一点。 */
const PAUSES = {
  '，': 0.18, '、': 0.12, '；': 0.22, '：': 0.18,
  '。': 0.32, '？': 0.35, '！': 0.35, '…': 0.4, '—': 0.25,
}

/** 估算一句中文台词的时长。 */
export function estimateSpeechDuration(text) {
  const stripped = String(text ?? '').replace(/\s/g, '')
  if (!stripped) return 0

  let pauseTotal = 0
  let spoken = 0
  for (const ch of stripped) {
    const pause = PAUSES[ch]
    if (pause !== undefined) pauseTotal += pause
    else spoken += 1
  }
  return spoken / CHARS_PER_SECOND + pauseTotal + LEAD_IN_S + TAIL_S
}

/**
 * 一句台词能占的最长时间。
 *
 * 超过单镜上限的台词，配出来的音频装不进任何一个镜头，混音时会盖到
 * 下一镜上去，成片里两个人同时说话。留出尾巴的余量。
 */
export const maxLineSeconds = (fps = 24) => maxShotDurationS(fps) - TAIL_S

const SENTENCE_ENDS = '。！？…'
const CLAUSE_ENDS = '；，、'

/**
 * 把过长的台词按标点切成几句，每句都装得进一个镜头。
 *
 * 先在句末标点处切，切不够再退到逗号分号。硬切字数是最后手段，
 * 那样会把词切断，听起来很别扭，但总好过整句被下一镜的声音盖住。
 */
export function splitLongText(text, maxSeconds) {
  if (estimateSpeechDuration(text) <= maxSeconds) return [text]

  const chunksBy = (marks, src) => {
    const out = []
    for (const part of src) {
      if (estimateSpeechDuration(part) <= maxSeconds) {
        out.push(part)
        continue
      }
      let buf = ''
      for (const ch of part) {
        buf += ch
        if (marks.includes(ch) && estimateSpeechDuration(buf) >= maxSeconds * 0.6) {
          out.push(buf)
          buf = ''
        }
      }
      if (buf) out.push(buf)
    }
    return out
  }

  let pieces = chunksBy(SENTENCE_ENDS, [text])
  pieces = chunksBy(CLAUSE_ENDS, pieces)

  // 还有过长的就只能按字数硬切。每一段都要自带头尾的呼吸留白，
  // 算字数时先把这部分扣掉，否则切出来的每一段都刚好超一点。
  const speakable = Math.max(0.5, maxSeconds - LEAD_IN_S - TAIL_S)
  const take = Math.max(1, Math.trunc(speakable * CHARS_PER_SECOND))
  const final = []
  for (let piece of pieces) {
    while (estimateSpeechDuration(piece) > maxSeconds) {
      final.push(piece.slice(0, take))
      piece = piece.slice(take)
    }
    if (piece) final.push(piece)
  }
  return final.map((p) => p.trim()).filter(Boolean)
}

/**
 * 由配音时长反推镜头时长。
 *
 * 没有台词的镜头保持分镜给的时长不动，它们是节奏调节的余量。
 * 有台词的镜头向上吸附到可生成档位并锁定，宁长勿短——短了会截断台词，
 * 长了尾巴上留一点表演余韵反而自然。
 *
 * 会就地改 shot。锁定之后再平衡总时长的那一步就不会再动它。
 */
export function lockDuration(shot, speechS) {
  if (!shot.dialogue.length) return shot.duration_s
  const locked = ceilDuration(speechS + TAIL_S)
  shot.duration_s = locked
  shot.duration_locked = true
  return locked
}

/** 一个镜头的配音结果与时长决策。 */
export function shotAudioPlan(shotId, speechDurationS, lockedDurationS, lines) {
  const slackS = lockedDurationS - speechDurationS
  return {
    shot_id: shotId,
    speech_duration_s: speechDurationS,
    locked_duration_s: lockedDurationS,
    slack_s: slackS,
    lines,
    // 留白小于半秒。这类镜头在装配时不能再压缩
    is_tight: slackS < 0.5,
  }
}

/** 给拆出来的新镜取一个全集没用过的编号。 */
export function freeShotId(base, used) {
  for (const suffix of 'bcdefghijklmnopqrstuvwxyz') {
    const candidate = `${base}_${suffix}`
    if (!used.has(candidate)) return candidate
  }
  let n = 2
  while (used.has(`${base}_${n}`)) n += 1
  return `${base}_${n}`
}

/**
 * 把一个镜头的台词按时长打包，每包都装得进一个镜头。
 *
 * 一个镜头塞了三句长台词是常事——模型不知道单段视频只能出 5 秒。
 * 不拆的话这三句的音频加起来超过镜头时长，混音时盖到下一镜上。
 */
export function groupLines(shot, maxSeconds) {
  if (shot.dialogue.length <= 1) return [[...shot.dialogue]]

  const dur = (line) =>
    line.actual_duration_s !== null && line.actual_duration_s !== undefined
      ? line.actual_duration_s
      : estimateSpeechDuration(line.text)

  const budget = Math.max(0.5, maxSeconds - TAIL_S)
  const groups = []
  let current = []
  let total = 0
  for (const line of shot.dialogue) {
    const d = dur(line)
    if (current.length && total + d > budget) {
      groups.push(current)
      current = []
      total = 0
    }
    current.push(line)
    total += d
  }
  if (current.length) groups.push(current)
  return groups
}
