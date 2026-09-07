// 分镜表的结构约束。从 Python 版 tests/test_shot.py 搬过来。
//
// 这些不是形式上的校验，每一条都对应一类实际会毁掉成片的问题：
// 模型在分镜里重写角色外观、台词落到没出场的人身上、硬切带了转场时长。

import { describe, expect, it } from 'vitest'

import {
  CameraAngle,
  FacePose,
  ShotSize,
  ShotStatus,
  Transition,
  applyLipsyncRules,
  deriveNeedsLipsync,
  parseShot,
  totalDialogueDurationS,
} from '../src/engine/models/shot.js'

const base = (over = {}) => ({
  shot_id: 'ep01_sh001',
  scene_id: 's1',
  order: 0,
  ...over,
})

describe('结构约束', () => {
  it('角色外观字段被结构禁止', () => {
    // 一致性靠 schema 里没有这些字段来保证，不靠提示词里求模型别写
    expect(() =>
      parseShot(base({ characters: [{ char_id: 'c_a', hair: '长发' }] })),
    ).toThrow()
    expect(() => parseShot(base({ appearance: '长发白裙' }))).toThrow()
  })

  it('台词说话人必须在场', () => {
    expect(() =>
      parseShot(
        base({
          characters: [{ char_id: 'c_a' }],
          dialogue: [{ char_id: 'c_b', text: '我走了' }],
        }),
      ),
    ).toThrow(/不在本镜角色列表/)
  })

  it('旁白不需要在场角色', () => {
    const shot = parseShot(base({ dialogue: [{ text: '三年后' }] }))
    expect(shot.dialogue[0].char_id).toBeNull()
  })

  it('硬切不能有转场时长', () => {
    expect(() =>
      parseShot(base({ transition_in: Transition.CUT, transition_dur_s: 0.4 })),
    ).toThrow(/硬切/)
  })

  it('溶解必须有转场时长', () => {
    expect(() =>
      parseShot(base({ transition_in: Transition.DISSOLVE, transition_dur_s: 0 })),
    ).toThrow(/转场时长/)
  })

  it('shot_id 必须规范', () => {
    expect(() => parseShot(base({ shot_id: 'EP01-SH1' }))).toThrow()
  })

  it('默认值和 Python 版一致', () => {
    const shot = parseShot(base())
    expect(shot.shot_size).toBe(ShotSize.MS)
    expect(shot.camera_angle).toBe(CameraAngle.EYE_LEVEL)
    expect(shot.duration_s).toBe(5)
    expect(shot.status).toBe(ShotStatus.PLANNED)
    expect(shot.needs_lipsync).toBe(false)
  })
})

describe('口型判定', () => {
  // 这件事必须用规则算。交给大模型判断它很不稳，
  // 而判错的代价是给一个背对镜头的人做口型，或者漏掉一段正脸台词。
  const talking = (over = {}) =>
    parseShot(
      base({
        shot_size: ShotSize.MCU,
        camera_angle: CameraAngle.EYE_LEVEL,
        characters: [{ char_id: 'c_a', face_pose: FacePose.FRONT }],
        dialogue: [{ char_id: 'c_a', text: '你听我说' }],
        ...over,
      }),
    )

  it('近景正脸有台词要做口型', () => {
    expect(deriveNeedsLipsync(talking())).toBe(true)
  })

  it('远景不做', () => {
    expect(deriveNeedsLipsync(talking({ shot_size: ShotSize.LS }))).toBe(false)
  })

  it('顶拍不做', () => {
    expect(
      deriveNeedsLipsync(talking({ camera_angle: CameraAngle.OVERHEAD })),
    ).toBe(false)
  })

  it('背对镜头不做', () => {
    expect(
      deriveNeedsLipsync(
        talking({ characters: [{ char_id: 'c_a', face_pose: FacePose.BACK }] }),
      ),
    ).toBe(false)
  })

  it('旁白不做', () => {
    expect(
      deriveNeedsLipsync(
        talking({ characters: [], dialogue: [{ text: '三年后' }] }),
      ),
    ).toBe(false)
  })

  it('说话人背对但另一人正脸也不做', () => {
    // 看的是「说话那个人」的朝向，不是「画面里有没有正脸」
    expect(
      deriveNeedsLipsync(
        talking({
          characters: [
            { char_id: 'c_a', face_pose: FacePose.BACK },
            { char_id: 'c_b', face_pose: FacePose.FRONT },
          ],
          dialogue: [{ char_id: 'c_a', text: '你听我说' }],
        }),
      ),
    ).toBe(false)
  })

  it('批量回填', () => {
    const shots = [talking(), parseShot(base({ shot_id: 'ep01_sh002', order: 1 }))]
    applyLipsyncRules(shots)
    expect(shots.map((s) => s.needs_lipsync)).toEqual([true, false])
  })
})

describe('台词时长', () => {
  it('未配音时总时长为空', () => {
    // 返回 0 的话，配音还没跑的镜头会被当成「没台词」，
    // 时长反推那一步就整个跳过去了
    const shot = parseShot(
      base({
        characters: [{ char_id: 'c_a' }],
        dialogue: [
          { char_id: 'c_a', text: '一', actual_duration_s: 1.2 },
          { char_id: 'c_a', text: '二' },
        ],
      }),
    )
    expect(totalDialogueDurationS(shot)).toBeNull()
  })

  it('配音后累加', () => {
    const shot = parseShot(
      base({
        characters: [{ char_id: 'c_a' }],
        dialogue: [
          { char_id: 'c_a', text: '一', actual_duration_s: 1.2 },
          { char_id: 'c_a', text: '二', actual_duration_s: 0.8 },
        ],
      }),
    )
    expect(totalDialogueDurationS(shot)).toBeCloseTo(2.0, 6)
  })

  it('无台词镜头时长为零', () => {
    expect(totalDialogueDurationS(parseShot(base()))).toBe(0)
  })
})
