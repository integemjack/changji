// 角色圣经与分镜。两阶段生成的两个阶段。
//
// 顺序不能反：先出角色登记进资产库拿到 id，第二阶段的 schema 里才可能
// 没有外观字段。先有分镜再补角色的话，模型已经在分镜里写过一遍外观，
// 之后再想统一就晚了。
//
// 和 Python 版逐用例比对过（圣经 22 个、分镜解析与 schema 约束若干）。

import { describe, expect, it, vi } from 'vitest'

import { LLMConfigSchema } from '../src/engine/config.js'
import { AssetLibrarySchema, StyleLine } from '../src/engine/models/character.js'
import {
  BibleError,
  clean,
  defaultNegative,
  generateBible,
  parseBible,
  slug,
} from '../src/engine/stages/bible.js'
import {
  StoryboardError,
  addMissingSpeakers,
  buildPrompt,
  checkCoverage,
  linkLocation,
  llmShotSchema,
  parseShots,
} from '../src/engine/stages/storyboard.js'
import { DURATION_SLOTS } from '../src/engine/timing.js'

const assets = AssetLibrarySchema.parse({
  characters: {
    c_lin: { char_id: 'c_lin', name: '林晚', appearance: { identity: '女', face: '长发', attire: '西装' } },
    c_wang: { char_id: 'c_wang', name: '老王', appearance: { identity: '男', face: '寸头', attire: '制服' } },
  },
  locations: {
    loc_office: { location_id: 'loc_office', name: '办公室', space: '工位', lighting: '冷光' },
  },
})

// ---------------- 角色圣经 ----------------

describe('id 生成', () => {
  it('英文名转小写下划线', () => {
    expect(slug('Lin Wan')).toBe('lin_wan')
    expect(slug('office-night')).toBe('office_night')
  })

  it('纯中文名用哈希兜底，保证稳定且合法', () => {
    // 不兜底的话 slug 是空串，这个角色就整个丢了
    const a = slug('林晚')
    expect(a).toMatch(/^x[0-9a-f]{6}$/)
    expect(slug('林晚')).toBe(a) // 同一个名字每次一样
  })
})

describe('字段清洗', () => {
  it('去掉尾部句号', () => {
    // 这些字段拼提示词时用逗号连接，模型带来的句号会让结果变成
    // 「冷静克制。，身姿笔挺。，」，而这个串会出现在每一个镜头里
    expect(clean('冷静克制。')).toBe('冷静克制')
    expect(clean('带逗号，')).toBe('带逗号')
    expect(clean('连续。。。')).toBe('连续')
  })

  it('多余空白压成一个空格', () => {
    expect(clean('  多  空格  ')).toBe('多 空格')
  })

  it('空值给空串', () => {
    expect(clean(null)).toBe('')
    expect(clean(undefined)).toBe('')
  })
})

describe('负向提示词', () => {
  it('动漫线额外压写实倾向', () => {
    // Wan 有很强的写实偏置，不压的话动漫输入会被往真人方向拽
    expect(defaultNegative(StyleLine.ANIME)).toContain('写实，照片质感，真人')
    expect(defaultNegative(StyleLine.REALISTIC)).not.toContain('照片质感')
  })
})

describe('解析角色圣经', () => {
  const sample = {
    characters: [
      { key: 'lin_wan', name: '林晚', identity: '一位三十岁的女性', face: '长发及肩。', attire: '深色西装。' },
      { key: '', name: '老王', identity: '中年男人', face: '寸头', attire: '保安制服' },
    ],
    locations: [{ key: 'office_night', name: '夜间办公室', space: '开放式工位', lighting: '冷调顶光' }],
    global_style: '电影感，冷调。',
  }

  it('id 加前缀，外观清洗过', () => {
    const lib = parseBible(sample)
    expect(lib.characters.c_lin_wan.appearance.face).toBe('长发及肩')
    expect(lib.locations.loc_office_night.lighting).toBe('冷调顶光')
    expect(lib.style.global_style).toBe('电影感，冷调')
  })

  it('key 缺了就用名字兜底', () => {
    expect(Object.keys(parseBible(sample).characters).length).toBe(2)
  })

  it('音色留空，等配音时按服务端实际有什么再定', () => {
    // 这里写死路径的话，换一台 ComfyUI 就可能对不上，
    // 节点校验不过整条流水线直接断在配音这一步
    expect(parseBible(sample).characters.c_lin_wan.voice_id).toBeNull()
  })

  it('从身份描述里猜性别，配音挑音色时用', () => {
    expect(parseBible(sample).characters.c_lin_wan.voice_gender).toBe('female')
  })

  it('一个角色都没有要报错而不是给空库', () => {
    // 空库继续往下走的话，分镜那一步的 schema 生成会失败，
    // 而那时报的错跟真正的原因隔着好几步
    expect(() => parseBible({ characters: [], locations: [], global_style: '' })).toThrow(
      /没有产出任何角色/,
    )
  })

  it('剧本为空时不去打模型', async () => {
    const fetchImpl = vi.fn()
    const llm = LLMConfigSchema.parse({ base_url: 'http://x/v1', model: 'm' })
    await expect(generateBible(llm, '  ', { fetchImpl })).rejects.toThrow(BibleError)
    expect(fetchImpl).not.toHaveBeenCalled()
  })
})

// ---------------- 分镜 ----------------

describe('给大模型的 schema', () => {
  const schema = () => llmShotSchema(assets).properties.shots.items.properties

  it('角色 id 收紧成枚举', () => {
    // 这是防止模型凭空造角色最硬的手段——它在结构上就填不了别的
    expect(schema().characters.items.properties.char_id.enum).toEqual(['c_lin', 'c_wang'])
  })

  it('场景 id 也收紧', () => {
    expect(schema().location_id.anyOf[0].enum).toEqual(['loc_office'])
  })

  it('时长只能取可生成的档位', () => {
    // 让模型自由填的话会填出 8 秒 10 秒，而单段只能出到 5 秒，
    // 超出部分被静默截断
    expect(schema().duration_s.enum).toEqual([...DURATION_SLOTS])
  })

  it('外观字段一个都没有', () => {
    // 一致性是结构保证而不是提示词祈使：模型想写也写不进去
    const inShot = Object.keys(schema().characters.items.properties)
    expect(inShot).not.toContain('face')
    expect(inShot).not.toContain('hair')
    expect(inShot).not.toContain('attire')
  })

  it('运行时字段不给模型填', () => {
    const props = schema()
    for (const k of ['needs_lipsync', 'status', 'attempts', 'frame_path', 'video_path']) {
      expect(props).not.toHaveProperty(k)
    }
  })

  it('台词里的时长和音频路径由配音回填，不让模型猜', () => {
    const keys = Object.keys(schema().dialogue.items.properties)
    expect(keys).not.toContain('audio_path')
    expect(keys).not.toContain('actual_duration_s')
    expect(keys).not.toContain('voice_id')
  })

  it('characters 和 dialogue 带说明且必填', () => {
    // 只给一个引用而不说要填什么，模型会整个略过这两个字段，
    // 结果是分镜里一句台词都没有，配音和口型全部落空
    const props = schema()
    expect(props.dialogue.description).toContain('每一句话都必须落到某个镜头上')
    const required = llmShotSchema(assets).properties.shots.items.required
    expect(required).toContain('characters')
    expect(required).toContain('dialogue')
  })

  it('没有角色时拒绝出 schema', () => {
    const empty = AssetLibrarySchema.parse({})
    expect(() => llmShotSchema(empty)).toThrow(/一个角色都没有/)
  })
})

describe('提示词', () => {
  it('只给 id 和名字，不给外观', () => {
    // 给了模型就会忍不住在分镜里复述一遍，而复述必然有偏差，
    // 那正是漂移的来源
    const p = buildPrompt('剧本', assets, { describe: () => '2 个 5 秒镜头', shotCount: 2, totalS: 10 }, 'ep01')
    expect(p).toContain('c_lin：林晚')
    expect(p).not.toContain('长发')
  })

  it('没有场景时也说得清楚', () => {
    const noLoc = AssetLibrarySchema.parse({ characters: assets.characters })
    const p = buildPrompt('剧本', noLoc, { describe: () => 'x', shotCount: 1, totalS: 5 }, 'ep01')
    expect(p).toContain('未定义场景')
  })
})

describe('解析分镜', () => {
  const raw = (over = {}) => ({
    shot_id: 'ep01_sh001', scene_id: 'loc_office', order: 0,
    first_frame_prompt: '她站在窗前', shot_size: 'MCU', duration_s: 3,
    characters: [{ char_id: 'c_lin', face_pose: 'front' }],
    dialogue: [{ char_id: 'c_lin', text: '我走了' }],
    ...over,
  })

  it('时长吸附到档位', () => {
    expect(parseShots({ shots: [raw({ duration_s: 3.3 })] }, assets)[0].duration_s).toBe(3)
    expect(parseShots({ shots: [raw({ duration_s: 7 })] }, assets)[0].duration_s).toBe(5)
  })

  it('硬切的转场时长兜成 0 而不是报错退出', () => {
    // 模型常忘了这条。为此让整张分镜表作废不划算
    const shot = parseShots({ shots: [raw({ transition_in: 'cut', transition_dur_s: 0.9 })] }, assets)[0]
    expect(shot.transition_dur_s).toBe(0)
  })

  it('溶解没给时长就补 0.4', () => {
    const shot = parseShots({ shots: [raw({ transition_in: 'dissolve' })] }, assets)[0]
    expect(shot.transition_dur_s).toBe(0.4)
  })

  it('说话人不在场就补进 characters', () => {
    // 模型很常漏这一步。直接拒绝的话整张分镜表作废，
    // 而问题其实只是少了一行引用
    const shot = parseShots(
      { shots: [raw({ characters: [], dialogue: [{ char_id: 'c_wang', text: '等等' }] })] },
      assets,
    )[0]
    expect(shot.characters.map((c) => c.char_id)).toEqual(['c_wang'])
  })

  it('scene_id 正是场景 id 时接到 location_id 上', () => {
    // 不接的话渲染时场景描述整段丢掉——不报错，只是每个镜头里的
    // 房间都不一样
    expect(parseShots({ shots: [raw({ location_id: null })] }, assets)[0].location_id).toBe(
      'loc_office',
    )
  })

  it('scene_id 不是场景 id 就别乱接', () => {
    const shot = parseShots({ shots: [raw({ scene_id: 's9', location_id: null })] }, assets)[0]
    expect(shot.location_id).toBeNull()
  })

  it('口型规则在这一步回填', () => {
    expect(parseShots({ shots: [raw()] }, assets)[0].needs_lipsync).toBe(true)
    expect(parseShots({ shots: [raw({ shot_size: 'LS' })] }, assets)[0].needs_lipsync).toBe(false)
  })

  it('引用了未注册角色要拒绝', () => {
    expect(() =>
      parseShots({ shots: [raw({ characters: [{ char_id: 'c_nobody' }], dialogue: [] })] }, assets),
    ).toThrow(/未注册/)
  })

  it('空分镜表报错', () => {
    expect(() => parseShots({ shots: [] }, assets)).toThrow(StoryboardError)
    expect(() => parseShots({}, assets)).toThrow(/没有返回镜头列表/)
  })

  it('剧本有对白但分镜一句没有，当场拦下', () => {
    // 模型很容易只写画面不写台词，产出一部哑剧。
    // 这类问题该在生成阶段检出，不该等到配音阶段发现一句话都没有
    expect(() =>
      parseShots({ shots: [raw({ dialogue: [], characters: [] })] }, assets, {
        script: '林晚：我走了',
      }),
    ).toThrow(/一句台词都没有/)
  })
})

describe('单独的兜底函数', () => {
  it('补说话人只补已注册的', () => {
    const item = { characters: [], dialogue: [{ char_id: 'c_nobody', text: 'x' }] }
    addMissingSpeakers(item, new Set(['c_lin']))
    expect(item.characters).toEqual([])
  })

  it('接场景只接已注册的', () => {
    const item = { scene_id: 'loc_office' }
    expect(linkLocation(item, new Set(['loc_office']))).toBe(true)
    expect(linkLocation({ scene_id: 's1' }, new Set(['loc_office']))).toBe(false)
  })

  it('已经填了 location_id 就不动', () => {
    const item = { scene_id: 'loc_office', location_id: 'loc_bar' }
    expect(linkLocation(item, new Set(['loc_office', 'loc_bar']))).toBe(false)
    expect(item.location_id).toBe('loc_bar')
  })

  it('剧本没有对白时不报哑剧', () => {
    expect(checkCoverage('空镜，风吹过', [{ dialogue: [] }])).toEqual([])
  })
})
