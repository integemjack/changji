/**
 * 枚举值到中文的映射。
 *
 * 引擎那边的枚举是英文的，界面上不该出现 mcu、eye_level 这种词。
 * 收在一处，改一次全站生效。
 */

export const SHOT_SIZES = [
  { value: 'ECU', label: '大特写' },
  { value: 'CU', label: '特写' },
  { value: 'MCU', label: '近景' },
  { value: 'MS', label: '中景' },
  { value: 'MLS', label: '中远景' },
  { value: 'LS', label: '远景' },
  { value: 'ELS', label: '大远景' },
]

export const CAMERA_ANGLES = [
  { value: 'low', label: '仰拍' },
  { value: 'eye_level', label: '平视' },
  { value: 'high', label: '俯拍' },
  { value: 'overhead', label: '顶拍' },
  { value: 'dutch', label: '斜角' },
]

export const CAMERA_MOVES = [
  { value: 'static', label: '固定' },
  { value: 'pan_left', label: '左摇' },
  { value: 'pan_right', label: '右摇' },
  { value: 'tilt_up', label: '上摇' },
  { value: 'tilt_down', label: '下摇' },
  { value: 'push_in', label: '推进' },
  { value: 'pull_out', label: '拉远' },
  { value: 'handheld', label: '手持' },
  { value: 'orbit', label: '环绕' },
]

export const TRANSITIONS = [
  { value: 'cut', label: '硬切' },
  { value: 'dissolve', label: '溶解' },
  { value: 'fade_in', label: '淡入' },
  { value: 'fade_out', label: '淡出' },
  { value: 'whip', label: '甩镜' },
]

/** 镜头状态。tone 决定标签的颜色，界面上一列看下去就知道哪儿卡了。 */
export const SHOT_STATUS = {
  planned: { label: '未开工', tone: 'neutral' },
  audio_done: { label: '配音完成', tone: 'info' },
  frame_done: { label: '首帧完成', tone: 'info' },
  draft_done: { label: '草稿完成', tone: 'info' },
  draft_rejected: { label: '草稿未过闸', tone: 'warn' },
  final_done: { label: '成片完成', tone: 'ok' },
  final_rejected: { label: '成片未过闸', tone: 'warn' },
  fallback: { label: '已降级', tone: 'warn' },
  locked: { label: '已锁定', tone: 'ok' },
}

const pick = (list, value) =>
  list.find((x) => x.value === value)?.label ?? value ?? ''

export const sizeLabel = (v) => pick(SHOT_SIZES, v)
export const angleLabel = (v) => pick(CAMERA_ANGLES, v)
export const moveLabel = (v) => pick(CAMERA_MOVES, v)
export const transitionLabel = (v) => pick(TRANSITIONS, v)
export const statusOf = (v) => SHOT_STATUS[v] ?? { label: v, tone: 'neutral' }

/** 流水线阶段。制作页的进度条和事件流用。 */
export const STAGE_LABELS = {
  audio: '配音',
  frames: '首帧',
  draft: '草稿档',
  final: '成片档',
  lipsync: '口型',
  gate: '质量闸门',
  assemble: '装配成片',
  done: '完成',
}
