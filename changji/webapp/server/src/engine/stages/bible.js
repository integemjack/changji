/**
 * 角色圣经生成。
 *
 * 两阶段生成的第一阶段。先只产角色和场景，登记进资产库拿到 id，
 * 第二阶段才产分镜，那时 schema 里已经没有外观字段可写了。
 *
 * 顺序不能反。先有分镜再补角色，模型已经在分镜里写过一遍外观，
 * 之后再想统一就晚了。
 */

import { createHash } from 'node:crypto'

import { completeJson } from '../llm.js'
import { AssetLibrarySchema, StyleLine, guessGender } from '../models/character.js'

export class BibleError extends Error {}

export const BIBLE_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['characters', 'locations', 'global_style'],
  properties: {
    characters: {
      type: 'array',
      description: '剧本里所有有名有姓或有台词的角色',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['key', 'name', 'identity', 'face', 'attire'],
        properties: {
          key: { type: 'string', description: '英文小写下划线短标识，如 lin_wan' },
          name: { type: 'string', description: '剧本里的中文称呼' },
          identity: { type: 'string', description: '身份：性别、年龄段、气质。一句话' },
          body: { type: 'string', description: '体型和身高感' },
          face: {
            type: 'string',
            description:
              '五官、发型、发色、瞳色。这段会在几十个镜头里逐字复用，写得具体且不要含糊',
          },
          attire: { type: 'string', description: '默认服装' },
        },
      },
    },
    locations: {
      type: 'array',
      description: '剧本里出现的场景',
      items: {
        type: 'object',
        additionalProperties: false,
        required: ['key', 'name', 'space', 'lighting'],
        properties: {
          key: { type: 'string', description: '英文小写下划线短标识' },
          name: { type: 'string', description: '中文场景名' },
          space: { type: 'string', description: '空间结构和布景' },
          lighting: { type: 'string', description: '光线基调' },
          palette: { type: 'string', description: '色彩方案' },
        },
      },
    },
    global_style: {
      type: 'string',
      description: '全剧统一的画风、色温、质感。一句话',
    },
  },
}

export function buildPrompt(script, styleLine) {
  const styleHint =
    styleLine === StyleLine.ANIME
      ? '画风是二次元动漫，描述用简洁的标签式短语。'
      : '画风是真人写实，描述用自然的中文短句。'
  return `你是一位短剧美术指导。读下面的剧本，产出角色设定和场景设定。

${styleHint}

要求：

1. 只写剧本里真实出现的角色和场景，不要自己加人加景。
2. face 这一段会在几十个镜头里被逐字复用，是角色能不能保持一致的关键。
   写具体的、可画出来的特征：发型、发色、脸型、眼型。
   不要写"好看""气质佳"这类无法转成画面的词。
3. identity 用一句话说清性别、年龄段、气质。
4. attire 写这个角色的默认服装。剧情中的换装不在这里写。
5. key 用英文小写和下划线，比如 lin_wan、office_night。
6. lighting 要说清时间和光质，比如"夜间冷调顶光，霓虹反光"。
7. global_style 是全剧统一的调子，所有镜头都会带上它。

剧本：

${script}

只输出 JSON，不要任何解释文字。`
}

/** 转成合法的 id 片段。中文名也要能用。 */
export function slug(text) {
  const s = String(text ?? '')
    .toLowerCase()
    .replace(/[^a-z0-9_]+/g, '_')
    .replace(/^_+|_+$/g, '')
  if (s) return s
  // 纯中文的 key，用哈希兜底保证稳定且合法
  return 'x' + createHash('sha1').update(String(text ?? ''), 'utf8').digest('hex').slice(0, 6)
}

/**
 * 清洗模型给的字段。
 *
 * 要去掉尾部的句号。这些字段拼提示词时用逗号连接，模型带来的句号会
 * 让结果变成「冷静克制。，身姿笔挺。，」这样标点重复的串，
 * 而且这个串会出现在每一个镜头里。
 */
export function clean(text) {
  return String(text ?? '')
    .replace(/\s+/g, ' ')
    .trim()
    .replace(/[。.；;，,、\s]+$/, '')
}

/**
 * 默认负向提示词。
 *
 * 动漫线要额外压写实倾向，因为 Wan 有很强的写实偏置，
 * 不压的话动漫输入会被往真人方向拽。
 */
export function defaultNegative(styleLine) {
  const base =
    '低质量，模糊，过曝，畸形，多余的手指，画得不好的手部，' +
    '画得不好的脸部，静止不动的画面，字幕，水印'
  return styleLine === StyleLine.ANIME ? base + '，写实，照片质感，真人' : base
}

export function parseBible(data, styleLine = StyleLine.REALISTIC, aspectRatio = '9:16') {
  if (!data || typeof data !== 'object' || Array.isArray(data)) {
    throw new BibleError('大模型没有返回对象')
  }

  const characters = {}
  const items = Array.isArray(data.characters) ? data.characters : []
  for (const [index, item] of items.entries()) {
    const key = slug(item?.key || item?.name || '')
    if (!key) continue
    const charId = `c_${key}`
    characters[charId] = {
      char_id: charId,
      name: item?.name || key,
      // 音色留空，配音时按服务端实际有哪些参考音频再定。
      // 这里写死路径的话，换一台 ComfyUI 就可能对不上，
      // 节点校验不过整条流水线直接断在配音这一步。
      voice_id: null,
      voice_gender: guessGender(item?.identity ?? ''),
      voice_order: index,
      appearance: {
        identity: clean(item?.identity),
        body: clean(item?.body),
        face: clean(item?.face),
        attire: clean(item?.attire),
      },
    }
  }

  const locations = {}
  for (const item of Array.isArray(data.locations) ? data.locations : []) {
    const key = slug(item?.key || item?.name || '')
    if (!key) continue
    const locId = `loc_${key}`
    locations[locId] = {
      location_id: locId,
      name: item?.name || key,
      space: clean(item?.space),
      lighting: clean(item?.lighting),
      palette: clean(item?.palette),
    }
  }

  if (!Object.keys(characters).length) {
    throw new BibleError(
      '大模型没有产出任何角色。检查剧本里是否真的有人物，或者换一个更强的模型',
    )
  }

  try {
    return AssetLibrarySchema.parse({
      characters,
      locations,
      style: {
        style_line: styleLine,
        global_style: clean(data.global_style),
        negative_prompt: defaultNegative(styleLine),
        aspect_ratio: aspectRatio,
      },
    })
  } catch (err) {
    throw new BibleError(`产出的设定不合法：${err.message}`)
  }
}

/** 从剧本产出角色圣经和场景设定。 */
export async function generateBible(llm, script, {
  styleLine = StyleLine.REALISTIC, aspectRatio = '9:16', ...rest
} = {}) {
  if (!String(script).trim()) throw new BibleError('剧本是空的，出不了角色')
  const data = await completeJson(llm, buildPrompt(script, styleLine), {
    schema: BIBLE_SCHEMA,
    name: 'bible',
    ...rest,
  })
  return parseBible(data, styleLine, aspectRatio)
}
