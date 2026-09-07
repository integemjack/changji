/**
 * 角色与场景资产。
 *
 * 外观描述只存在于这里。分镜表里只有 id。渲染提示词时由程序把不可变外观块
 * 和本镜可变项机械拼接，保证同一角色在几十个镜头里拿到逐字节相同的描述。
 */

import { z } from 'zod'

/** 风格线。决定用哪套出图基座和提示词范式。 */
export const StyleLine = {
  REALISTIC: 'realistic',
  ANIME: 'anime',
}

/**
 * 不可变外观块。一旦定稿就不再改，改了等于换角色。
 *
 * 五段式结构。拼提示词时按固定顺序连接，顺序也不能变，
 * 因为提示词里靠前的词权重更高，顺序变了画面就会漂。
 */
export const AppearanceBlockSchema = z
  .object({
    identity: z.string(), // 身份：性别、年龄段、气质
    body: z.string().default(''), // 体型、身高感
    face: z.string(), // 五官、发型、发色、瞳色
    attire: z.string(), // 默认服装
    style: z.string().default(''), // 该角色特有的画风修饰
  })
  .strict()

/**
 * 把外观块拼成提示词片段。写实线用自然语言，动漫线用标签串。
 *
 * 各段的尾部标点要去掉。手写的设定里常带句号，拼接后会变成
 * 「冷静克制。，身姿笔挺。，」这样标点重复的串，
 * 而这个串会出现在每一个镜头的提示词里。
 */
export function renderAppearance(block, styleLine) {
  const raw = [block.identity, block.body, block.face, block.attire, block.style]
  const parts = raw
    .map((p) => String(p ?? '').trim().replace(/[。.；;，,、\s]+$/, ''))
    .filter(Boolean)
  const sep = styleLine === StyleLine.ANIME ? ', ' : '，'
  return parts.join(sep)
}

/** 服装变体。剧情里换装、衣服破损这些状态。 */
export const WardrobeVariantSchema = z
  .object({
    wardrobe_id: z.string(),
    description: z.string(), // 覆盖 AppearanceBlock.attire
  })
  .strict()

export const CharacterSchema = z
  .object({
    char_id: z.string().regex(/^c_[a-z0-9_]+$/), // 必须以 c_ 开头
    name: z.string(), // 剧本里的称呼
    appearance: AppearanceBlockSchema,
    wardrobe: z.array(WardrobeVariantSchema).default([]),

    // 参考图，相对项目根的路径
    ref_front: z.string().nullable().default(null),
    ref_three_quarter: z.string().nullable().default(null),
    ref_back: z.string().nullable().default(null),

    // 训练出来的角色 LoRA
    lora_path: z.string().nullable().default(null),
    lora_trigger: z.string().nullable().default(null),
    lora_strength: z.number().min(0).max(2).default(1),

    // 配音
    voice_id: z.string().nullable().default(null),
    voice_ref_audio: z.string().nullable().default(null),
    // 猜出来的性别和角色序号。配音时拿它们从服务端的音色列表里挑。
    voice_gender: z.string().default(''),
    voice_order: z.number().int().default(0),
  })
  .strict()

/** 取指定服装状态的描述，找不到就退回默认。 */
export function wardrobeDesc(character, wardrobeState) {
  if (wardrobeState && wardrobeState !== 'default') {
    const hit = character.wardrobe.find((v) => v.wardrobe_id === wardrobeState)
    if (hit) return hit.description
  }
  return character.appearance.attire
}

/**
 * 渲染该角色的完整外观提示词片段。
 *
 * 换装时只替换 attire 那一段，其余逐字节不变。
 */
export function renderCharacterPrompt(character, styleLine, wardrobeState = 'default') {
  let block = character.appearance
  if (wardrobeState !== 'default') {
    block = { ...block, attire: wardrobeDesc(character, wardrobeState) }
  }
  const rendered = renderAppearance(block, styleLine)
  if (character.lora_trigger) {
    const sep = styleLine === StyleLine.ANIME ? ', ' : '，'
    return `${character.lora_trigger}${sep}${rendered}`
  }
  return rendered
}

/** 按面部朝向挑参考图。 */
export function refForPose(character, facePose) {
  const table = {
    front: character.ref_front,
    three_quarter: character.ref_three_quarter || character.ref_front,
    profile: character.ref_three_quarter || character.ref_front,
    back: character.ref_back || character.ref_three_quarter,
  }
  return facePose in table ? table[facePose] : character.ref_front
}

/** 一个场景。空景图是场景一致性的锚点。 */
export const LocationSchema = z
  .object({
    location_id: z.string().regex(/^loc_[a-z0-9_]+$/),
    name: z.string(),
    space: z.string(), // 空间结构
    lighting: z.string(), // 光线基调，如 冷调顶光、暖调侧逆光
    palette: z.string().default(''), // 色彩方案
    ref_empty: z.string().nullable().default(null), // 空景图路径，无人物
  })
  .strict()

export function renderLocationPrompt(location, styleLine) {
  const parts = [location.space, location.lighting, location.palette]
    .map((p) => String(p ?? '').trim())
    .filter(Boolean)
  const sep = styleLine === StyleLine.ANIME ? ', ' : '，'
  return parts.join(sep)
}

/** 全剧统一的风格层。所有镜头共用，保证整体调性不漂。 */
export const StyleProfileSchema = z
  .object({
    style_line: z.enum(Object.values(StyleLine)).default(StyleLine.REALISTIC),
    global_style: z.string().default(''), // 全剧画风、色温、质感
    negative_prompt: z.string().default(''),
    aspect_ratio: z.string().default('9:16'), // 9:16 竖屏 或 16:9 横屏
  })
  .strict()

/** 角色和场景的资产库。分镜表里的每个 id 都必须能在这里查到。 */
export const AssetLibrarySchema = z
  .object({
    characters: z.record(z.string(), CharacterSchema).default({}),
    locations: z.record(z.string(), LocationSchema).default({}),
    style: StyleProfileSchema.prefault({}), // prefault 不是 default，理由见 config.js
  })
  .strict()

/** 给大模型做约束解码用的枚举。有了它模型就编不出新角色。 */
export const characterIds = (assets) => Object.keys(assets.characters).sort()
export const locationIds = (assets) => Object.keys(assets.locations).sort()

/** 检查分镜表引用的 id 是否都已注册。返回问题列表。 */
export function validateReferences(assets, charIds, locIds) {
  const problems = []
  for (const cid of [...charIds].sort()) {
    if (!(cid in assets.characters)) problems.push(`角色 ${cid} 未在资产库注册`)
  }
  for (const lid of [...locIds].sort()) {
    if (!(lid in assets.locations)) problems.push(`场景 ${lid} 未在资产库注册`)
  }
  return problems
}

// 参考音色的偏好顺序。真正能用哪些由服务端说了算，
// 这里只是没得选时的默认倾向：中文样本优先，其次按性别分。
//
// 踩过的坑：写死 vibevoice 的中文样本提交上去，节点校验直接拒绝，
// 整条流水线断在配音这一步。文件在磁盘上，但不在这个节点的列表里。
// 参考音色在 ComfyUI 那边是个下拉框，装了哪些插件就有哪些选项，
// 换一台机器列表就不一样，所以不能假设任何一条路径一定存在。
const FEMALE_MARKS = ['female', 'woman', '_f_', 'belinda', 'mabel', 'sophie']
const MALE_MARKS = [
  'male', 'man', '_m_', 'chadwick', 'eastwood', 'freeman', 'attenborough',
]

const FEMALE_WORDS = ['女', '妈', '母', '姐', '妹', '婆', '娘', '妻', '太太', '阿姨']
const MALE_WORDS = ['男', '爸', '父', '哥', '弟', '爷', '叔', '夫', '先生', '伯']

/**
 * 从身份描述里猜性别。猜不出来返回空串。
 *
 * 身份描述里通常会写「一位年轻的女性」这类话，够用了。
 */
export function guessGender(identity) {
  const text = identity || ''
  const female = FEMALE_WORDS.some((w) => text.includes(w))
  const male = MALE_WORDS.some((w) => text.includes(w))
  if (female && !male) return 'female'
  if (male && !female) return 'male'
  return ''
}

function voiceGender(name) {
  const low = name.toLowerCase()
  // female 里含 male，先判 female
  if (FEMALE_MARKS.some((m) => low.includes(m))) return 'female'
  if (MALE_MARKS.some((m) => low.includes(m))) return 'male'
  return ''
}

/**
 * 从服务端给的音色列表里挑一条参考音频。
 *
 * 先按性别分，再在同性别里优先中文样本。顺序不能反过来：
 * 唯一那条中文样本是男声，反过来的话女角色会被配上男声，
 * 这比带一点口音难听得多。
 */
export function pickVoice(available, gender, index = 0) {
  const pool = available.filter((v) => v && v !== 'none')
  if (!pool.length) return null

  const rank = (v) => {
    const vg = voiceGender(v)
    let genderRank
    if (!gender || !vg) genderRank = 1 // 分不出来的排中间
    else if (vg === gender) genderRank = 0
    else genderRank = 2 // 性别相反的排最后
    const low = v.toLowerCase()
    const zhRank = low.includes('zh_') || low.includes('zh-') ? 0 : 1
    return [genderRank, zhRank, v]
  }

  const ranked = [...pool].sort((a, b) => {
    const ra = rank(a)
    const rb = rank(b)
    return ra[0] - rb[0] || ra[1] - rb[1] || (ra[2] < rb[2] ? -1 : ra[2] > rb[2] ? 1 : 0)
  })
  const best = rank(ranked[0]).slice(0, 2)
  // 同一档里按角色序号轮着分，不同角色至少声音不同
  const tier = ranked.filter((v) => {
    const r = rank(v)
    return r[0] === best[0] && r[1] === best[1]
  })
  return tier[index % tier.length]
}
