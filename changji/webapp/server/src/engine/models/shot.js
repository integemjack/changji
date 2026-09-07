/**
 * 分镜表：整套系统的中枢数据结构。
 *
 * 设计上最重要的一条：这里没有任何描述角色长相、发型、服装的字段。
 *
 * 保持角色跨镜头一致的常见做法是在提示词里反复强调，那不可靠。这里的做法
 * 是让大模型在结构上就没有写错的机会：它只能填 char_id 和本镜可变项，
 * 外观描述在渲染时由程序从角色资产库机械拼接，逐字节相同。
 *
 * 同一份定义既用于运行时校验，也用于导出 JSON Schema 约束大模型输出。
 */

import { z } from 'zod'

/** 景别。取值顺序由近到远。 */
export const ShotSize = {
  ECU: 'ECU', // 大特写
  CU: 'CU', // 特写
  MCU: 'MCU', // 近景
  MS: 'MS', // 中景
  MLS: 'MLS', // 中远景
  LS: 'LS', // 远景
  ELS: 'ELS', // 大远景
}

/** 能看清嘴部动作的景别。口型判定用，不要改成靠模型判断。 */
export const LIPSYNC_CAPABLE_SIZES = new Set([
  ShotSize.ECU,
  ShotSize.CU,
  ShotSize.MCU,
  ShotSize.MS,
])

export const CameraAngle = {
  LOW: 'low', // 仰拍
  EYE_LEVEL: 'eye_level', // 平视
  HIGH: 'high', // 俯拍
  OVERHEAD: 'overhead', // 顶拍
  DUTCH: 'dutch', // 斜角
}

/** 俯拍和顶拍看不清嘴，排除在口型之外。 */
export const LIPSYNC_CAPABLE_ANGLES = new Set([
  CameraAngle.LOW,
  CameraAngle.EYE_LEVEL,
  CameraAngle.HIGH,
  CameraAngle.DUTCH,
])

export const CameraMove = {
  STATIC: 'static',
  PAN_LEFT: 'pan_left',
  PAN_RIGHT: 'pan_right',
  TILT_UP: 'tilt_up',
  TILT_DOWN: 'tilt_down',
  PUSH_IN: 'push_in',
  PULL_OUT: 'pull_out',
  HANDHELD: 'handheld',
  ORBIT: 'orbit',
}

/** 角色面部朝向。口型判定用。 */
export const FacePose = {
  FRONT: 'front',
  THREE_QUARTER: 'three_quarter',
  PROFILE: 'profile',
  BACK: 'back',
  OFF_SCREEN: 'off_screen',
}

export const LIPSYNC_CAPABLE_POSES = new Set([
  FacePose.FRONT,
  FacePose.THREE_QUARTER,
  FacePose.PROFILE,
])

export const Transition = {
  CUT: 'cut',
  DISSOLVE: 'dissolve',
  FADE_IN: 'fade_in',
  FADE_OUT: 'fade_out',
  WHIP: 'whip',
}

/** 镜头在流水线上的位置。断点续跑靠它。 */
export const ShotStatus = {
  PLANNED: 'planned', // 分镜已出，未开工
  AUDIO_DONE: 'audio_done', // 配音已出，时长已锁
  FRAME_DONE: 'frame_done', // 首帧已出
  DRAFT_DONE: 'draft_done', // 草稿档视频已出
  DRAFT_REJECTED: 'draft_rejected', // 草稿未过闸门
  FINAL_DONE: 'final_done', // 成片档已出
  FINAL_REJECTED: 'final_rejected', // 成片未过闸门
  FALLBACK: 'fallback', // 重试超限，降级为静帧加运镜
  LOCKED: 'locked', // 人工确认，不再重跑
}

const enumOf = (obj) => z.enum(Object.values(obj))

/**
 * 角色在本镜头中的表现。
 *
 * 只有可变项。外观描述属于角色资产，不在这里，也不允许大模型在这里写。
 */
export const CharacterInShotSchema = z
  .object({
    char_id: z.string(), // 角色 id，必须是已注册角色之一
    expression: z.string().max(40).default(''), // 表情，如 愕然、隐忍
    action: z.string().max(80).default(''), // 本镜动作，如 后退半步
    wardrobe_state: z.string().max(40).default('default'), // 服装状态 id
    face_pose: enumOf(FacePose).default(FacePose.FRONT),
    screen_pos: z.string().default('center'), // left / center / right
  })
  .strict()

/** 一句台词。时长字段由配音阶段回填，分镜阶段不填。 */
export const DialogueLineSchema = z
  .object({
    char_id: z.string().nullable().default(null), // 为空表示旁白
    text: z.string().min(1).max(200),
    emotion: z.string().default('neutral'),
    emotion_intensity: z.number().min(0).max(1).default(0.5),
    voice_id: z.string().nullable().default(null),
    // 以下由配音阶段回填
    audio_path: z.string().nullable().default(null), // 相对项目根
    actual_duration_s: z.number().min(0).nullable().default(null),
  })
  .strict()

const SHOT_ID = /^[a-z0-9_]+$/

export const ShotSchema = z
  .object({
    shot_id: z.string().regex(SHOT_ID), // 全局唯一，如 ep01_s03_sh007
    scene_id: z.string().regex(SHOT_ID),
    order: z.number().int().min(0), // 集内顺序

    // ---- 画面 ----
    visual_desc: z.string().max(300).default(''), // 给人看的中文描述
    first_frame_prompt: z.string().max(1200).default(''),
    last_frame_prompt: z.string().max(1200).nullable().default(null), // 空则单帧图生视频
    motion_prompt: z.string().max(400).default(''), // 运动描述，给视频模型
    negative_prompt: z.string().default(''),

    shot_size: enumOf(ShotSize).default(ShotSize.MS),
    camera_angle: enumOf(CameraAngle).default(CameraAngle.EYE_LEVEL),
    camera_move: enumOf(CameraMove).default(CameraMove.STATIC),
    camera_id: z.string().nullable().default(null), // 同场景同机位保证不越轴

    // ---- 引用（只放 id，不放描述）----
    characters: z.array(CharacterInShotSchema).default([]),
    location_id: z.string().nullable().default(null),
    prop_ids: z.array(z.string()).default([]),

    // ---- 时间 ----
    duration_s: z.number().gt(0).max(30).default(5),
    duration_locked: z.boolean().default(false), // 真表示已由配音时长反推锁定

    // ---- 声音 ----
    dialogue: z.array(DialogueLineSchema).default([]),
    sfx: z.array(z.string()).default([]),
    bgm_cue: z.string().nullable().default(null),
    needs_lipsync: z.boolean().default(false), // 由 deriveNeedsLipsync 推导

    // ---- 剪辑 ----
    transition_in: enumOf(Transition).default(Transition.CUT),
    transition_dur_s: z.number().min(0).max(2).default(0),
    subtitle_text: z.string().default(''),

    // ---- 质控 ----
    beat: z.string().max(20).default(''), // 叙事功能，如 反转、铺垫
    continuity_notes: z.string().max(200).default(''),
    missing_info: z.array(z.string()).default([]), // 模型自报的信息缺口

    // ---- 运行时状态（大模型不填）----
    status: enumOf(ShotStatus).default(ShotStatus.PLANNED),
    attempts: z.number().int().min(0).default(0),
    frame_path: z.string().nullable().default(null),
    video_path: z.string().nullable().default(null),
    gate_notes: z.array(z.string()).default([]),
  })
  .strict()
  .superRefine((shot, ctx) => {
    if (shot.transition_in === Transition.CUT && shot.transition_dur_s !== 0) {
      ctx.addIssue({ code: 'custom', message: '硬切的转场时长必须为 0' })
    }
    if (shot.transition_in !== Transition.CUT && shot.transition_dur_s <= 0) {
      ctx.addIssue({
        code: 'custom',
        message: `${shot.transition_in} 需要一个大于 0 的转场时长`,
      })
    }
    // 台词里出现的角色必须也在 characters 里，否则是分镜自相矛盾
    const present = new Set(shot.characters.map((c) => c.char_id))
    for (const line of shot.dialogue) {
      if (line.char_id !== null && !present.has(line.char_id)) {
        ctx.addIssue({
          code: 'custom',
          message: `台词说话人 ${line.char_id} 不在本镜角色列表中。旁白请把 char_id 留空`,
        })
      }
    }
  })

export const parseShot = (data) => ShotSchema.parse(data)

export const hasOnscreenDialogue = (shot) =>
  shot.dialogue.some((line) => line.char_id !== null)

/** 全部台词的实际总时长。任一句未配音则返回 null。 */
export function totalDialogueDurationS(shot) {
  if (!shot.dialogue.length) return 0
  const durations = shot.dialogue.map((l) => l.actual_duration_s)
  if (durations.some((d) => d === null || d === undefined)) return null
  return durations.reduce((a, b) => a + b, 0)
}

/**
 * 判断一个镜头该不该做口型。
 *
 * 这件事必须用规则算，不能交给大模型判断，它在这上面很不稳。
 *
 * 四个条件同时成立才做：有出镜台词、景别够近能看清嘴、机位不是顶拍、
 * 并且说话的角色确实是正脸或侧脸对着镜头。
 */
export function deriveNeedsLipsync(shot) {
  if (!hasOnscreenDialogue(shot)) return false
  if (!LIPSYNC_CAPABLE_SIZES.has(shot.shot_size)) return false
  if (!LIPSYNC_CAPABLE_ANGLES.has(shot.camera_angle)) return false

  const speakers = new Set(
    shot.dialogue.filter((l) => l.char_id !== null).map((l) => l.char_id),
  )
  return shot.characters.some(
    (c) => speakers.has(c.char_id) && LIPSYNC_CAPABLE_POSES.has(c.face_pose),
  )
}

/** 批量回填 needs_lipsync。分镜生成后、配音之前调用。 */
export function applyLipsyncRules(shots) {
  for (const shot of shots) shot.needs_lipsync = deriveNeedsLipsync(shot)
  return shots
}
