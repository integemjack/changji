// 提示词拼接。一致性真正生效的地方。
//
// 最要紧的一条：同一个角色在几十个镜头里拿到的身份层必须逐字节相同。
// 它不是靠在提示词里叮嘱模型「保持一致」，是靠这段代码每次从资产库
// 读出同一个字符串。

import { describe, expect, it } from 'vitest'

import { AssetLibrarySchema, StyleLine } from '../src/engine/models/character.js'
import { ShotSchema } from '../src/engine/models/shot.js'
import { PromptComposer, RenderError } from '../src/engine/prompt.js'

const assets = (over = {}) =>
  AssetLibrarySchema.parse({
    characters: {
      c_lin: {
        char_id: 'c_lin',
        name: '林晚',
        appearance: {
          identity: '一位三十岁上下的女性，冷静克制。',
          body: '身形偏瘦',
          face: '长发及肩，眼神锐利',
          attire: '深色西装',
        },
        wardrobe: [{ wardrobe_id: 'torn', description: '西装破损，沾着灰' }],
        ref_front: 'refs/lin_front.png',
      },
    },
    locations: {
      loc_office: {
        location_id: 'loc_office',
        name: '办公室',
        space: '开放式办公区，工位成排',
        lighting: '冷调顶光',
        ref_empty: 'refs/office.png',
      },
    },
    style: { global_style: '电影感，浅景深' },
    ...over,
  })

const shot = (over = {}) =>
  ShotSchema.parse({
    shot_id: 'ep01_sh001',
    scene_id: 's1',
    order: 0,
    characters: [{ char_id: 'c_lin' }],
    location_id: 'loc_office',
    shot_size: 'MCU',
    camera_angle: 'eye_level',
    ...over,
  })

describe('分层顺序', () => {
  it('身份在最前，风格在最后', () => {
    // 提示词里靠前的词权重更高。顺序一变画面重心就跟着变，
    // 同一个角色在不同镜头里会显得不是同一个人
    const { positive } = new PromptComposer(assets()).compose(shot())
    const idx = (s) => positive.indexOf(s)
    expect(idx('三十岁上下的女性')).toBeLessThan(idx('开放式办公区'))
    expect(idx('开放式办公区')).toBeLessThan(idx('近景'))
    expect(idx('近景')).toBeLessThan(idx('电影感'))
  })

  it('同一角色在不同镜头里身份层逐字节相同', () => {
    const composer = new PromptComposer(assets())
    const a = composer.compose(shot({ shot_size: 'CU', first_frame_prompt: '她低头' }))
    const b = composer.compose(shot({ shot_size: 'LS', first_frame_prompt: '她抬头' }))
    const identity = '一位三十岁上下的女性，冷静克制，身形偏瘦，长发及肩，眼神锐利，深色西装'
    expect(a.positive.startsWith(identity)).toBe(true)
    expect(b.positive.startsWith(identity)).toBe(true)
  })

  it('外观里的尾部句号被剥掉', () => {
    // 手写的设定常带句号，不剥的话拼出来是「冷静克制。，身形偏瘦」，
    // 而这个串会出现在每一个镜头的提示词里
    const { positive } = new PromptComposer(assets()).compose(shot())
    expect(positive).not.toContain('。，')
  })
})

describe('本镜可变项', () => {
  it('表情和动作接在身份层后面', () => {
    const { positive } = new PromptComposer(assets()).compose(
      shot({ characters: [{ char_id: 'c_lin', expression: '愕然', action: '后退半步' }] }),
    )
    expect(positive).toContain('愕然')
    expect(positive).toContain('后退半步')
  })

  it('换装只替换服装那一段', () => {
    const { positive } = new PromptComposer(assets()).compose(
      shot({ characters: [{ char_id: 'c_lin', wardrobe_state: 'torn' }] }),
    )
    expect(positive).toContain('西装破损，沾着灰')
    expect(positive).not.toContain('深色西装')
    // 其余几段一字不变
    expect(positive).toContain('长发及肩，眼神锐利')
  })
})

describe('参考图', () => {
  it('按面部朝向挑，场景空景图也带上', () => {
    const { reference_images } = new PromptComposer(assets()).compose(shot())
    expect(reference_images).toEqual(['refs/lin_front.png', 'refs/office.png'])
  })
})

describe('未注册的引用', () => {
  it('角色不在库里就报清楚是哪一镜哪个 id', () => {
    expect(() =>
      new PromptComposer(assets()).compose(
        shot({ characters: [{ char_id: 'c_nobody' }] }),
      ),
    ).toThrow(RenderError)
  })

  it('场景不在库里同样', () => {
    // 这一条对应一个真实的静默失败：location_id 空着时场景描述整段丢掉，
    // 不报错，只是每个镜头里的房间都不一样
    expect(() =>
      new PromptComposer(assets()).compose(shot({ location_id: 'loc_nope' })),
    ).toThrow(/未注册场景/)
  })
})

describe('动漫线', () => {
  it('用英文逗号加空格分隔', () => {
    const anime = assets({ style: { style_line: StyleLine.ANIME, global_style: 'anime style' } })
    const { positive } = new PromptComposer(anime).compose(shot())
    expect(positive).toContain(', ')
  })
})

describe('运动提示词', () => {
  it('只管动作不重复外观', () => {
    const motion = new PromptComposer(assets()).motionPrompt(
      shot({
        camera_move: 'push_in',
        motion_prompt: '她缓缓抬起头',
        characters: [{ char_id: 'c_lin', action: '握紧手中的笔' }],
      }),
    )
    expect(motion).toBe('镜头缓慢推近，她缓缓抬起头，握紧手中的笔')
    expect(motion).not.toContain('长发及肩')
  })
})
