/**
 * 写剧本。
 *
 * 流水线的第一步。产出不是一段自由文本，是一张结构化的场次表，再由我们
 * 自己渲染成「名字：台词」的写法。让模型直接写剧本格式的话，它一会儿用
 * 冒号一会儿用括号，一会儿把旁白也写成对白，后面识别角色就开始出错。
 * 形式我们定，模型只管内容。
 *
 * 时长靠字数控制。中文普通话大约每秒 4.6 个字，一集 60 秒的对白量就是
 * 两百多字。不给这个预算的话，模型写出来的东西按时长算能拍十分钟。
 */

import { completeJson } from '../llm.js'
import { StyleLine } from '../models/character.js'

export class ScriptError extends Error {}

/** 中文普通话的语速。配音时长估算和剧本字数预算共用同一个常数。 */
export const CHARS_PER_SECOND = 4.6

/**
 * 一集里对白占的比重。剩下的是动作和环境描写，不发声但占画面时长。
 * 全是对白的话没有留白，成片像念稿子。
 */
export const DIALOGUE_SHARE = 0.62

/** 这个时长大概能装多少字对白。 */
export const budgetChars = (durationS) =>
  Math.max(20, Math.trunc(durationS * CHARS_PER_SECOND * DIALOGUE_SHARE))

/**
 * 模型爱给动作行加一对括号，给台词加一对引号，无论提示词里怎么说。
 * 这些符号会一路流到分镜提示词和字幕里，所以在入口就削掉。
 * 形式是我们定的，不是模型定的。
 */
const WRAPPERS = [
  ['（', '）'], ['(', ')'], ['【', '】'], ['[', ']'],
  ['“', '”'], ['"', '"'], ['「', '」'], ['『', '』'], ["'", "'"],
]

/**
 * 首尾这一对是不是套住整段的那一对。
 *
 * 光数左右个数不够。「（甲说）乙答（丙笑）」左右各两个，数目相等，
 * 但首尾那两个并不是一对，削掉就把中间的括号弄错位了。
 * 要从头扫一遍看深度什么时候回到零。
 */
function wrapsWhole(text, left, right) {
  if (left === right) {
    // 引号这类左右一样的，中间不能再出现同一个符号
    return !text.slice(left.length, -right.length).includes(right)
  }
  let depth = 0
  for (let i = 0; i < text.length; i += 1) {
    if (text[i] === left) depth += 1
    else if (text[i] === right) {
      depth -= 1
      if (depth === 0) return i === text.length - 1
      if (depth < 0) return false
    }
  }
  return false
}

/**
 * 削掉整段外面套的那一对括号或引号。
 *
 * 只削套住整段的那一层，中间的括号是内容的一部分，不动。
 */
export function stripWrapper(text) {
  let out = String(text).trim()
  let changed = true
  while (changed && out.length >= 2) {
    changed = false
    for (const [left, right] of WRAPPERS) {
      if (!out.startsWith(left) || !out.endsWith(right)) continue
      if (!wrapsWhole(out, left, right)) continue
      const inner = out.slice(left.length, -right.length).trim()
      if (!inner) continue
      out = inner
      changed = true
      break
    }
  }
  return out
}

/**
 * 模型爱在动作行开头挂一个时间码，比如「[0-3秒] 画面特写：…」。
 * 提示词里没让它写，schema 里也没有这一项，它自己加的。
 * 这段文字会原样进到分镜提示词里，让画面模型去理解一个时间码。
 *
 * 只削开头那一对括号，而且要括号里确实像时间码才削：带数字，
 * 并且带秒、s 或者冒号。不做这个限制的话，「（他犹豫了）他开口」
 * 这种正常写法开头也会被削掉。
 */
const LEAD_BRACKETS = [['[', ']'], ['【', '】'], ['（', '）'], ['(', ')']]
const TIME_UNITS = ['秒', 's', 'S', ':', '：', '分', '帧']

export function stripLeadingTimecode(text) {
  const out = String(text).trim()
  for (const [left, right] of LEAD_BRACKETS) {
    if (!out.startsWith(left)) continue
    const end = out.indexOf(right)
    if (end <= 0) continue
    const inside = out.slice(left.length, end)
    if (!/\d/.test(inside)) continue
    if (!TIME_UNITS.some((u) => inside.includes(u))) continue
    return out.slice(end + right.length).trim()
  }
  return out
}

/**
 * 模型表示「没人说话」的各种写法。
 *
 * schema 里写的是填空字符串，但它经常填 none、null、旁白 这类词。
 * 原样当成名字的话，成片里会出现一个叫 none 的角色，
 * 字幕上写着「none：寂静」。
 */
const NO_SPEAKER = new Set([
  'none', 'null', 'nil', 'n/a', 'na', '-', '—', '无', '空',
  '旁白', '画外音', 'narrator', 'voiceover', 'vo', 'ost',
  'none.', '（无）', '(none)',
])

/** 把「没人说话」的各种写法统一成空字符串。 */
export function normalizeSpeaker(raw) {
  const name = stripWrapper(String(raw ?? ''))
  return NO_SPEAKER.has(name.trim().toLowerCase()) ? '' : name
}

/** 写一集用的 JSON Schema。约束解码靠它，模型编不出格式外的东西。 */
export const SCRIPT_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['title', 'logline', 'beats'],
  properties: {
    title: { type: 'string', description: '这一集的标题，六个字以内' },
    logline: {
      type: 'string',
      description: '一句话说清这一集发生了什么，给人看的，不进成片',
    },
    beats: {
      type: 'array',
      minItems: 4,
      description: '按时间顺序排的场次。动作和对白交替，不要连着五句对白',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['kind', 'speaker', 'text'],
        properties: {
          kind: {
            type: 'string',
            enum: ['action', 'dialogue'],
            description: 'action 是动作或环境描写，dialogue 是有人说话',
          },
          speaker: {
            type: 'string',
            description: '说话的人。kind 是 action 时填空字符串',
          },
          text: {
            type: 'string',
            description: '这一拍的内容。对白只写说出口的话，不要带引号也不要带名字前缀',
          },
        },
      },
    },
  },
}

/**
 * 想选题用的 schema。一次给几个而不是一个：定调子这件事，
 * 摆三个方案在面前挑，比看一个方案判断「行不行」容易得多。
 */
export const PREMISE_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['ideas'],
  properties: {
    ideas: {
      type: 'array',
      minItems: 3,
      maxItems: 5,
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['title', 'premise', 'hook'],
        properties: {
          title: { type: 'string', description: '剧名，八个字以内' },
          premise: {
            type: 'string',
            description: '一两句话说清这部剧讲什么。要具体到人物和处境，不要写题材标签',
          },
          hook: { type: 'string', description: '一句话说清观众为什么会看下去' },
        },
      },
    },
  },
}

/**
 * 渲染成后面几步认的写法。
 *
 * 对白一律「名字：台词」，动作单独成行。格式由我们定死，不看模型心情，
 * 否则下一步识别角色就开始出错。
 */
export function renderDraft(draft) {
  const lines = []
  for (const b of draft.beats) {
    const text = String(b.text ?? '').trim()
    if (!text) continue
    if (b.kind === 'dialogue') {
      const name = String(b.speaker ?? '').trim()
      lines.push(name ? `${name}：${text}` : text)
    } else {
      lines.push(text)
    }
  }
  return lines.join('\n')
}

export function draftSpeakers(draft) {
  const seen = []
  for (const b of draft.beats) {
    const name = String(b.speaker ?? '').trim()
    if (b.kind === 'dialogue' && name && !seen.includes(name)) seen.push(name)
  }
  return seen
}

export const draftDialogueChars = (draft) =>
  draft.beats.filter((b) => b.kind === 'dialogue').reduce((a, b) => a + b.text.length, 0)

// ---- 提示词 ----

export function buildPrompt(premise, durationS, styleLine, { previous = '', characters = null } = {}) {
  const chars = budgetChars(durationS)
  const styleHint =
    styleLine === StyleLine.REALISTIC
      ? '真人写实短剧，台词生活化，不要文绉绉的书面语'
      : '动漫短剧，台词可以更有戏剧张力，但仍要口语化'
  const parts = [
    `你在写一集竖屏短剧，总时长约 ${durationS.toFixed(0)} 秒。${styleHint}。`,
    '',
    '硬性要求：',
    `1. 所有对白加起来控制在 ${chars} 个字左右，超出很多就是拍不完。`,
    '2. 动作和对白交替推进，不要连着好几句对白，画面会没有呼吸。',
    '3. 出场角色不超过三个。人一多，短剧里根本立不住。',
    '4. 对白只写说出口的话，不要带引号，不要在 text 里重复人名。',
    '5. 同一个角色的名字前后必须一模一样，不要一会儿全名一会儿简称。',
    '6. 开头三秒就要有事发生，短剧没有铺垫的余地。',
    '7. 结尾留一个钩子或者一个明确的情绪落点。',
  ]
  if (characters?.length) {
    parts.push('', `必须沿用这些已有角色，名字一字不改：${characters.join('、')}。`)
  }
  if (previous) {
    parts.push(
      '',
      '前面几集的剧本如下，这一集要接着往下写，人物关系和已经发生的事不能推翻：',
      '',
      previous.trim().slice(0, 4000),
    )
  }
  parts.push('', '这一集要写的：', '', premise.trim(), '', '只输出 JSON，不要任何解释文字。')
  return parts.join('\n')
}

/**
 * 想选题的提示词。
 *
 * 最容易出的问题是模型给一堆题材标签——「都市 / 复仇 / 逆袭」。
 * 那种东西写不成剧本：下一步要拿它当写剧本的输入，标签里没有人物、
 * 没有处境，模型只能自己编，编出来的每一集互不相干。
 * 所以这里反复要求具体到人和事。
 */
export function buildPremisePrompt(keywords, styleLine, count = 3, existing = null) {
  const styleHint =
    styleLine === StyleLine.REALISTIC
      ? '真人写实短剧，题材要落在现实生活里'
      : '动漫短剧，可以有超现实设定，但情感冲突要真实'
  const parts = [
    `你在给一部竖屏${styleHint}想选题。给出 ${count} 个不同方向的方案。`,
    '',
    '硬性要求：',
    '1. premise 必须具体到人物和处境，一两句话。「都市复仇爽剧」这种是题材标签不是选题，不要给。',
    '2. 一句话里就要有冲突。没有冲突的设定拍不成短剧。',
    '3. 主要人物不超过三个，短剧里人一多就立不住。',
    '4. 几个方案要拉开差距，不要三个都是同一个故事换名字。',
    '5. 是能一直往下拍的设定，不是一集就讲完的段子。',
    '6. title 是剧名，八个字以内，不要副标题。',
  ]
  if (keywords.trim()) parts.push('', `往这个方向想：${keywords.trim()}`)
  if (existing?.length) {
    // 已经有的方向要避开，否则连点两次「再想几个」会拿到同一批
    parts.push(
      '',
      '下面这些方向已经有了，换别的：',
      existing.slice(0, 6).map((e) => e.trim().slice(0, 60)).join('、'),
    )
  }
  parts.push('', '只输出 JSON，不要任何解释文字。')
  return parts.join('\n')
}

/**
 * 预告片的提示词。
 *
 * 预告片和正片是两种东西，不能拿写正片的那套提示词缩短了用：
 * 正片要把一件事讲完，预告片要的正好相反——只给钩子，不给答案。
 * 正片是一条连续的时间线，预告片是几个不相干瞬间的蒙太奇。
 * 正片结尾留情绪落点，预告片结尾留悬念和「点进去看」的冲动。
 */
export function buildTrailerPrompt(premise, durationS, styleLine, { episodes = '', characters = null } = {}) {
  const chars = budgetChars(durationS)
  const styleHint = styleLine === StyleLine.REALISTIC ? '真人写实短剧' : '动漫短剧'
  const parts = [
    `你在写一部竖屏${styleHint}的预告片，总时长约 ${durationS.toFixed(0)} 秒。`,
    '',
    '预告片不是把正片缩短，是另一种东西。硬性要求：',
    `1. 所有对白加起来控制在 ${chars} 个字以内。预告片以画面为主，话越少越有劲。`,
    '2. 用蒙太奇：几个不相干的瞬间快速切换，不要讲一条完整的时间线。',
    '3. 前两秒必须是全片最抓人的那个画面或那句话，刷到就得停下来。',
    '4. 只给钩子，不给答案。关键情节点到为止，结局绝对不能剧透。',
    '5. 结尾停在悬念上，可以是一句反问、一个未完成的动作或一个眼神。',
    '6. 对白只写说出口的话，不要带引号，不要在 text 里重复人名。',
    '7. 出场角色不超过三个，名字前后一模一样。',
    '8. title 写这部剧的名字，logline 写一句能当封面文案的钩子。',
  ]
  if (characters?.length) {
    parts.push('', `必须沿用这些已有角色，名字一字不改：${characters.join('、')}。`)
  }
  if (episodes) {
    parts.push(
      '',
      '已经写好的剧集如下。从里面挑最有冲击力的瞬间来剪，不要编造剧里没有的情节，也不要把结局说出来：',
      '',
      episodes.trim().slice(0, 6000),
    )
  }
  parts.push('', '这部剧讲的是：', '', premise.trim(), '', '只输出 JSON，不要任何解释文字。')
  return parts.join('\n')
}

// ---- 解析 ----

export function parseDraft(data) {
  if (!data || typeof data !== 'object' || Array.isArray(data)) {
    throw new ScriptError('大模型没有返回对象')
  }
  const raw = data.beats
  if (!Array.isArray(raw) || !raw.length) throw new ScriptError('大模型没写出任何内容')

  const beats = []
  for (const item of raw) {
    if (!item || typeof item !== 'object') continue
    const text = stripWrapper(stripLeadingTimecode(String(item.text ?? '')))
    if (!text) continue
    let kind = item.kind === 'dialogue' ? 'dialogue' : 'action'
    const speaker = normalizeSpeaker(item.speaker)
    if (kind === 'dialogue' && !speaker) {
      // 说了话却没说是谁说的，当旁白处理。丢掉的话这句台词
      // 就从成片里消失了，那比配错声音还糟。
      kind = 'action'
    }
    beats.push({ kind, speaker, text })
  }

  if (!beats.length) throw new ScriptError('大模型写的内容全是空的')
  if (!beats.some((b) => b.kind === 'dialogue')) {
    throw new ScriptError('整集一句台词都没有，这样出来的是默片')
  }

  return {
    title: String(data.title ?? '').trim(),
    logline: String(data.logline ?? '').trim(),
    beats,
  }
}

export function parsePremises(data) {
  const items = Array.isArray(data) ? data : data?.ideas
  if (!Array.isArray(items) || !items.length) throw new ScriptError('大模型没给出任何选题')

  const ideas = []
  for (const item of items) {
    if (!item || typeof item !== 'object') continue
    const premise = stripWrapper(String(item.premise ?? ''))
    if (!premise) continue
    ideas.push({
      title: stripWrapper(String(item.title ?? '')),
      premise,
      hook: stripWrapper(String(item.hook ?? '')),
    })
  }
  if (!ideas.length) throw new ScriptError('大模型给的选题全是空的')
  return ideas
}

// ---- 生成 ----

/** 从一句梗概写出一集剧本。 */
export async function generateScript(llm, premise, {
  durationS = 60, styleLine = StyleLine.REALISTIC, previous = '', characters = null, ...rest
} = {}) {
  if (!String(premise).trim()) throw new ScriptError('得先说清楚这一集要写什么')
  const data = await completeJson(
    llm,
    buildPrompt(premise, durationS, styleLine, { previous, characters }),
    { schema: SCRIPT_SCHEMA, name: 'script', ...rest },
  )
  return parseDraft(data)
}

/**
 * 剪一条预告片。
 *
 * 产出和正片一样是一份场次表，所以后面的分镜、配音、装配一步都不用改
 * ——对流水线来说预告片就是特别短的一集。
 */
export async function generateTrailer(llm, premise, {
  durationS = 20, styleLine = StyleLine.REALISTIC, episodes = '', characters = null, ...rest
} = {}) {
  if (!String(premise).trim() && !String(episodes).trim()) {
    throw new ScriptError('既没有梗概也没有写好的剧集，剪不出预告片')
  }
  const data = await completeJson(
    llm,
    buildTrailerPrompt(premise || '见下面已写好的剧集', durationS, styleLine, {
      episodes,
      characters,
    }),
    { schema: SCRIPT_SCHEMA, name: 'trailer', ...rest },
  )
  return parseDraft(data)
}

/**
 * 想几个选题。
 *
 * 「这部剧讲什么」是整条流水线的源头，也是最难从零开始的一步。
 * 给三个方案挑，比对着空白框发呆容易。
 */
export async function generatePremises(llm, {
  keywords = '', styleLine = StyleLine.REALISTIC, count = 3, existing = null, ...rest
} = {}) {
  const n = Math.max(3, Math.min(5, count))
  const data = await completeJson(
    llm,
    buildPremisePrompt(keywords, styleLine, n, existing),
    { schema: PREMISE_SCHEMA, name: 'premises', ...rest },
  )
  return parsePremises(data)
}
