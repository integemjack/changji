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

/**
 * 四段的段名。剧本阅读视图靠它认段头「【开场钩子 0–5 秒】」。
 *
 * **不是四个，是十六个。** 引擎有**五种戏的走法**（prompts.toml 的
 * `[script].act_labels`，二十个槽 = 五组 × 四段），而 `/api/script/write`
 * 在界面不送 `variation` 时会 `random_shape()` 现摇一个——也就是说五组里
 * **四组**用的段名都不是老那一套。
 *
 * 这儿原来只写死了第 0 组那四个名字，于是十份剧本里有八份，段头被当成
 * 普通描写行画进正文，整页退成一个没名字的大段：「几句、约几秒」那套
 * 四段读数全没了，而那正是这一页存在的理由。
 *
 * ⚠️ **源头在 cpp/prompts.toml 的 `[script].act_labels`。** 那边加一组
 * 这边就要跟——`act-labels.test.js` 直接读那个文件比对，加了不跟会红。
 */
export const ACT_LABELS = [
  // 0 步步紧逼（老的那一套）
  '开场钩子', '冲突推进', '情绪回报', '集尾留扣',
  // 1 开门见山
  '当头一击', '追因', '摊牌',
  // 2 中途翻盘
  '越理越乱', '假性收束', '集尾反转',
  // 3 一路下坠
  '开场失手', '越陷越深', '断念',
  // 4 两头并进
  '两头起手', '双线并进', '交汇',
]

/**
 * 流水线阶段。制作页的进度条和事件流用。
 *
 * **键必须是引擎的 `stage`，不是 `kind`。** 三处消费者
 * （JobBadge、useShots、run store）查的都是 `x.stage`，而引擎那边
 * `stage` 的取值只有 `pipeline::Stage` 那五个（episode.hpp 的枚举 →
 * to_string：audio / frames / draft / final / assemble），一个不多。
 *
 * 这儿原来还有 `gate: '质量闸门'` 和 `done: '完成'` 两条——**那是 kind
 * 不是 stage**（`e.kind = "gate"`、`emit(progress, "audio", "done", …)`），
 * 永远查不到。删掉它们，顺带把下面这句提醒接上：
 *
 * **没有 lipsync 这个阶段。** 2026-09-13 查过：引擎一次都没发过
 * stage="lipsync"。这句话留着，免得下一个人以为有这么一步——而它原来
 * 紧挨着的正是刚说的那两条永远命不中的键。
 */
export const STAGE_LABELS = {
  audio: '配音',
  frames: '首帧',
  draft: '草稿档',
  final: '成片档',
  assemble: '装配成片',
}
