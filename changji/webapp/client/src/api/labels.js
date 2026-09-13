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

/**
 * 画质档位。**取值仍然是 "720p"/"hd"/"2k"**——那是存在每个项目
 * changji.toml 的 [video] 里的字符串，改了名老项目就读不出来。
 *
 * 短边长边分开存，是因为**同一档在横屏和竖屏下要反过来写**：竖屏的
 * 720p 是 544×928，横屏是 928×544。上一版把 "544×928" 直接写死在
 * <option> 里，横屏项目那三行全是错的。
 *
 * 数字跟着引擎 config/settings.cpp 的 VideoConfig::size()，
 * 改那边这里也要改（两边都是 32 对齐的硬约束，不能随手填）。
 */
export const VIDEO_QUALITIES = [
  { value: '720p', label: '标准', short: 544, long: 928, note: '快' },
  { value: 'hd', label: '高清', short: 704, long: 1280, note: '推荐' },
  { value: '2k', label: '2K', short: 1440, long: 2560, note: '很吃显存' },
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

/**
 * 画质档的中文名。
 *
 * 以前是写在墙顶那行读数里的一句三元式 `quality === '2k' ? '2K' : '标准'`
 * ——2026-09-11 加回 hd 那一档之后，704×1280 的项目在界面上写着"标准"。
 * 两个值的三元式配三个值的字段，加一档就错一档，所以收到这儿来。
 */
export const qualityLabel = (v) => pick(VIDEO_QUALITIES, v)

/** 某一档在某个画幅下的真实尺寸，形如 "544×928"。 */
export const qualitySize = (v, orientation) => {
  const q = VIDEO_QUALITIES.find((x) => x.value === v)
  if (!q) return ''
  return orientation === 'landscape'
    ? `${q.long}×${q.short}`
    : `${q.short}×${q.long}`
}
export const statusOf = (v) => SHOT_STATUS[v] ?? { label: v, tone: 'neutral' }

/** 流水线阶段。制作页的进度条和事件流用。 */
export const STAGE_LABELS = {
  audio: '配音',
  frames: '首帧',
  draft: '草稿档',
  final: '成片档',
  // **没有 lipsync 这个阶段。** 2026-09-13 查过：Stage 枚举里只有
  // Audio/Frames/Draft/Final/Assemble，引擎一次都没发过 stage="lipsync"。
  // 这一条留着是历史残留，删了免得下一个人以为有这么一步。
  gate: '质量闸门',
  assemble: '装配成片',
  done: '完成',
}
