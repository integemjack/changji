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
 * 流水线阶段。顶栏那块「AI 作业中」和镜头墙上的进度用（三处消费者见下）。
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
  // **叫「出片」不叫「成片档」。**「档」这个字只在和草稿档对照时才有意义，
  // 而草稿档默认不跑（skip_draft 默认为真，挂上 Turbo 之后两档拉不开差距）
  // ——于是界面上那句「配音 50 · 首帧 51 · 成片档 68」里，只有它是行话。
  // 真开了草稿档的时候两个词并排出现，各自读得懂，不用对称。
  final: '出片',
  assemble: '装配成片',
}

/**
 * 这个成片文件属不属于这一集。
 *
 * ⚠️ **不能用 `name.includes(episodeId)`。** 集号到 99 以内是 `ep%02d`，
 * **第 100 集起变成 `ep100`、`ep101`……**（story_plan.cpp 的 `ep_id` 和
 * http/episodes.cpp 的 `ep_fmt` 都是这么写的，那是刻意留的）。于是站在
 * `ep10` 上时，`"ep107.mp4".includes("ep10")` 是真——`ep100` 到 `ep109`
 * 十条片子全算成 ep10 的。短剧动辄七八十上百集，这不是假想的数。
 *
 * 后果不是少一个数，是**审片页会播错片而且不吭声**：`load()` 默认选
 * `forThisEpisode[0]`（片单按 mtime 倒序），完全可能选中 ep107 那条；
 * 而 `isMine` 用的是同一个判据，于是它也说"这是你这一集的"——那句
 * 「播着别的集的片，一个字都不说」的守卫正好被同一个 bug 绕过去。
 * 底下那条跳转条画的永远是 ep10 的镜头，点一下按 ep10 的时长跳。
 *
 * 判据改成**两边都要挨着非字母数字**（或者文件名的头尾）：
 *
 *   ep01.mp4      / ep01  → 真（后面是 `.`）
 *   ep01_2k.mp4   / ep01  → 真（后面是 `_`，手动超分出来的那份还算这一集）
 *   导演版_ep01.mp4 / ep01 → 真（前面是 `_`，改过名的也还认）
 *   ep107.mp4     / ep10  → 假（后面是 `0`，这才是要挡的那条）
 *
 * 引擎那边 flow.cpp 的 `has_film` 是同一个判据的 C++ 版，两边要一起改。
 */
export function isFilmOf(name, episodeId) {
  const id = String(episodeId ?? '')
  if (!id) return false
  const s = String(name ?? '')
  const alnum = (c) => c !== undefined && /[A-Za-z0-9]/.test(c)
  for (let at = s.indexOf(id); at >= 0; at = s.indexOf(id, at + 1)) {
    if (!alnum(s[at - 1]) && !alnum(s[at + id.length])) return true
  }
  return false
}

/**
 * 一份剧本「几个字」。
 *
 * **为什么要收在一处。** 这个数原来在两个地方各算各的，而且**同时印在
 * 同一屏上**：分集标签写「剧本 157 字」，正下方工具条写「129 字 · 目标
 * 60 秒」。同一份稿子，差 28。
 *
 * 两边各对了一半：
 *
 *   · EpisodeView 用 `[...s.trim()].length`——按**码位**数是对的，但**连
 *     换行和空行一起数**。剧本里空行是排版，多空两行字数就涨两个，而那
 *     一栏正是用来判断"这一集写了多少"的。
 *   · EpScript 用 `s.replace(/\s/g,'').length`——去空白是对的，但 `.length`
 *     数的是 UTF-16 码元：基本平面外的汉字（𠮷 这种，人名里真的有）一个
 *     算两个。
 *
 * 合起来才是想要的那条规则：**去掉所有空白，按码位数**。
 *
 * 顺带一提，屏幕上还有第三个数——ScriptReader 那句「对白 58 字」。那个是
 * **另一件事**（只数台词，用来和时长预算比），它自己写明了"对白"两个字，
 * 不在这条规则的管辖内。
 */
export function countScriptChars(text) {
  return [...String(text ?? '').replace(/\s/g, '')].length
}
