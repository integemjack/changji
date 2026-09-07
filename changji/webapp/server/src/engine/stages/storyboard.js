/**
 * 剧本转分镜。
 *
 * 两阶段生成。第一遍只出角色圣经，第二遍才出分镜，而且第二遍的 schema 里
 * 根本没有描述外观的字段。大模型想写也写不进去，一致性因此是结构保证
 * 而不是提示词祈使。
 *
 * 角色 id 用枚举限定为已注册列表，模型编不出新角色。
 * 时长用配额表下发，不让模型自由填，因为它在几十个镜头规模上做算术不可靠。
 */

import { z } from 'zod'

import { completeJson } from '../llm.js'
import { characterIds, locationIds, validateReferences } from '../models/character.js'
import { ShotSchema, applyLipsyncRules } from '../models/shot.js'
import { DURATION_SLOTS, DurationQuota, snapDuration } from '../timing.js'

export class StoryboardError extends Error {}

/**
 * 分镜阶段允许大模型填的字段。
 *
 * 外观类字段不在其中，这是刻意的：模型在结构上就没有写外观的地方。
 */
const LLM_SHOT_FIELDS = new Set([
  'shot_id', 'scene_id', 'order', 'visual_desc', 'first_frame_prompt',
  'motion_prompt', 'shot_size', 'camera_angle', 'camera_move', 'camera_id',
  'characters', 'location_id', 'duration_s', 'dialogue', 'transition_in',
  'transition_dur_s', 'subtitle_text', 'beat', 'continuity_notes',
  'missing_info',
])

/**
 * 生成给大模型的 JSON Schema。
 *
 * 从 Shot 模型导出，然后做三件事：删掉运行时字段、把角色和场景 id
 * 收紧成枚举、去掉 needs_lipsync（那个由规则算）。
 *
 * 从同一份定义导出而不是手写，保证 schema 和校验永远一致——手写一份的话，
 * 模型模型字段改了、schema 没跟上，出来的分镜过不了校验，而报错指向的是
 * 模型层，跟真正的原因隔着好几步。
 */
export function llmShotSchema(assets) {
  const full = z.toJSONSchema(ShotSchema, { io: 'input', unrepresentable: 'any' })
  const props = full.properties ?? {}

  const kept = {}
  for (const [key, value] of Object.entries(props)) {
    if (LLM_SHOT_FIELDS.has(key)) kept[key] = structuredClone(value)
  }

  const charIds = characterIds(assets)
  const locIds = locationIds(assets)
  if (!charIds.length) {
    throw new StoryboardError('资产库里一个角色都没有。请先生成角色圣经')
  }

  // 角色 id 收紧成枚举。这是防止模型凭空造角色最硬的手段。
  kept.characters = {
    type: 'array',
    description: '本镜出现的角色。没有人物出镜就填空数组',
    items: {
      type: 'object',
      additionalProperties: false,
      required: ['char_id'],
      properties: {
        ...(kept.characters?.items?.properties ?? {}),
        char_id: { type: 'string', enum: charIds, description: '必须是已注册角色之一' },
      },
    },
  }

  const dialogueProps = { ...(kept.dialogue?.items?.properties ?? {}) }
  // 时长和音频路径由配音阶段回填，不让模型猜
  for (const gone of ['audio_path', 'actual_duration_s', 'voice_id']) {
    delete dialogueProps[gone]
  }
  dialogueProps.char_id = {
    anyOf: [{ type: 'string', enum: charIds }, { type: 'null' }],
    description: '说话角色。旁白留空',
  }
  // characters 和 dialogue 必须是必填并且带说明。只给一个引用而不说要填什么，
  // 模型会整个略过这两个字段，结果是分镜里一句台词都没有，配音和口型全部落空。
  kept.dialogue = {
    type: 'array',
    description:
      '本镜的台词和旁白，逐句填。剧本里的每一句话都必须落到某个镜头上，' +
      '不能丢。这一镜没有人说话就填空数组',
    items: {
      type: 'object',
      additionalProperties: false,
      required: ['text'],
      properties: dialogueProps,
    },
  }

  if (locIds.length) {
    kept.location_id = {
      anyOf: [{ type: 'string', enum: locIds }, { type: 'null' }],
    }
  }
  kept.duration_s = {
    type: 'number',
    enum: [...DURATION_SLOTS],
    description: '只能取这些值',
  }

  return {
    type: 'object',
    additionalProperties: false,
    required: ['shots'],
    properties: {
      shots: {
        type: 'array',
        items: {
          type: 'object',
          additionalProperties: false,
          required: [
            'shot_id', 'scene_id', 'order', 'first_frame_prompt',
            'shot_size', 'duration_s', 'characters', 'dialogue',
          ],
          properties: kept,
        },
      },
    },
  }
}

/**
 * 拼给大模型的提示词。
 *
 * 角色和场景只给 id 和名字，不给外观描述。给了模型就会忍不住在分镜里
 * 复述一遍，而复述必然有偏差，那正是漂移的来源。
 */
export function buildPrompt(script, assets, quota, episodeId) {
  const roster = characterIds(assets)
    .map((cid) => `  ${cid}：${assets.characters[cid].name}`)
    .join('\n')
  const places =
    locationIds(assets)
      .map((lid) => `  ${lid}：${assets.locations[lid].name}`)
      .join('\n') || '  （未定义场景，location_id 留空）'

  return `你是一位短剧分镜师。把下面的剧本拆成分镜表。

可用角色（只能用这些 id）：
${roster}

可用场景：
${places}

镜头配额，必须严格按这个数量和时长分配：
  ${quota.describe()}
  合计 ${quota.shotCount} 个镜头，总时长 ${quota.totalS} 秒

硬性要求：

1. shot_id 用 ${episodeId}_sh001 这样的格式，三位数字，按顺序递增。
2. order 从 0 开始递增。
3. duration_s 只能取配额里出现过的值，且各档位的数量必须与配额完全一致。
4. characters 必须填。凡是这一镜里出现的人，都要在这里列出 char_id，
   并填本镜的表情、动作、面部朝向。画面里没有人才填空数组。
   只填这些，绝对不要描述角色的长相、发型、发色、身材或服装样式，
   那些由系统统一管理，你写了会被丢弃并造成前后不一致。
   换装只能通过 wardrobe_state 填一个状态名。
5. first_frame_prompt 描述这一镜的画面：环境、光线、构图、角色的姿态和位置。
   同样不要描述角色长相，系统会自动拼接。
6. dialogue 必须填。剧本里的每一句台词都要落到某个镜头上，一句都不能丢。
   说话的人填 char_id，旁白留空。这一镜没人说话才填空数组。
   说话的角色也必须同时出现在 characters 里。
7. 同一场景内连续镜头尽量复用 camera_id，避免越轴。
8. transition_in 默认 cut 且 transition_dur_s 必须为 0；
   只有场景切换才用 dissolve，此时 transition_dur_s 填 0.4。
9. continuity_notes 记录需要与前后镜保持一致的细节，比如道具在哪只手。
10. 如果剧本里有信息不足以确定画面的地方，写进 missing_info，不要自己编。

剧本：

${script}

只输出 JSON，不要任何解释文字。`
}

/**
 * 台词说话人不在 characters 里时，把人补进去。
 *
 * 模型很常漏这一步。直接拒绝的话整张分镜表作废，而问题其实只是
 * 少了一行引用——补上比让用户重跑一遍划算得多。
 */
export function addMissingSpeakers(item, known) {
  const present = new Set((item.characters ?? []).map((c) => c?.char_id))
  for (const line of item.dialogue ?? []) {
    const cid = line?.char_id
    if (!cid || present.has(cid) || !known.has(cid)) continue
    item.characters ??= []
    item.characters.push({ char_id: cid })
    present.add(cid)
  }
}

/**
 * location_id 空着但 scene_id 正是一个已注册场景时，把它接上。
 *
 * schema 里 scene_id 和 location_id 是两个字段，模型十次有八次把场景
 * id 填进 scene_id 就完事了。后果不是报错——分镜表照样合法，是渲染时
 * 只在 location_id 有值时才把场景描述拼进提示词，于是空间和光线那一段
 * 整个丢掉，同一个房间在每个镜头里都长得不一样。
 *
 * 这类静默失败最难查，所以在这里接上，而不是指望模型下次填对。
 */
export function linkLocation(item, known) {
  if (item.location_id) return false
  const scene = String(item.scene_id ?? '')
  if (known.has(scene)) {
    item.location_id = scene
    return true
  }
  return false
}

/**
 * 检查分镜有没有漏掉剧本里的东西。
 *
 * 大模型很容易只写画面不写台词，产出一部哑剧。这类问题在生成阶段
 * 就能检出，不该等到配音阶段发现一句话都没有。
 */
export function checkCoverage(script, shots) {
  const problems = []
  const scriptHasDialogue = /[：:]\s*\S/.test(script)
  const shotLines = shots.reduce((a, s) => a + s.dialogue.length, 0)
  if (scriptHasDialogue && shotLines === 0) {
    problems.push('剧本里有对白，但分镜表里一句台词都没有。模型多半漏填了 dialogue 字段')
  }
  return problems
}

export function parseShots(data, assets, { script = '' } = {}) {
  const items = Array.isArray(data) ? data : data?.shots
  if (!Array.isArray(items)) {
    throw new StoryboardError(`大模型没有返回镜头列表，拿到的是 ${typeof items}`)
  }
  if (!items.length) throw new StoryboardError('大模型返回了空的分镜表')

  const knownChars = new Set(Object.keys(assets.characters))
  const knownLocs = new Set(Object.keys(assets.locations))
  const shots = []
  const problems = []

  for (const [i, raw] of items.entries()) {
    const item = { ...raw }
    item.order ??= i
    item.duration_s = snapDuration(Number(item.duration_s) || DURATION_SLOTS[DURATION_SLOTS.length - 1])
    // 模型常忘了硬切必须零时长，这里兜一下而不是报错退出
    if ((item.transition_in ?? 'cut') === 'cut') item.transition_dur_s = 0
    else if (!item.transition_dur_s) item.transition_dur_s = 0.4
    addMissingSpeakers(item, knownChars)
    linkLocation(item, knownLocs)
    try {
      shots.push(ShotSchema.parse(item))
    } catch (err) {
      problems.push(`第 ${i + 1} 个镜头不合法：${err.message}`)
    }
  }

  if (problems.length) {
    throw new StoryboardError(
      `分镜表有 ${problems.length} 个镜头不合法：\n${problems.slice(0, 5).join('\n')}`,
    )
  }

  const refProblems = validateReferences(
    assets,
    new Set(shots.flatMap((s) => s.characters.map((c) => c.char_id))),
    new Set(shots.map((s) => s.location_id).filter(Boolean)),
  )
  if (refProblems.length) {
    throw new StoryboardError(`分镜引用了未注册的资产：\n${refProblems.join('\n')}`)
  }

  if (script) {
    const gaps = checkCoverage(script, shots)
    if (gaps.length) throw new StoryboardError(gaps.join('\n'))
  }

  return applyLipsyncRules(shots)
}

/** 调大模型出分镜。 */
export async function generateStoryboard(llm, script, assets, episodeId, targetDurationS, opts = {}) {
  if (!String(script).trim()) throw new StoryboardError('剧本是空的，拆不出分镜')
  const quota = DurationQuota.forDuration(targetDurationS)
  const schema = llmShotSchema(assets)
  const data = await completeJson(llm, buildPrompt(script, assets, quota, episodeId), {
    schema,
    name: 'storyboard',
    ...opts,
  })
  return parseShots(data, assets, { script })
}
