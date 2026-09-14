<script setup>
/**
 * 镜头。**分镜和制作合成的这一页。**
 *
 * 原来是两页：分镜页排镜头（AI 出分镜、改台词、调顺序），制作页跑镜头
 * （出片、看进度、重出）。两页读的是同一份 `api.shots()`，各自画了一遍
 * 卡片、缩略图、状态、空状态——两千行里两三百行是近似重复的。
 *
 * 但真正的问题不是重复，是**人的动作被切断了**：看片子不满意 → 改运镜或
 * 台词 → 重出，这三步在同一镜上，却要换页，还得记住自己刚才看的是第几镜。
 *
 * 所以合成一页，形态是**一面墙加一个抽屉**：
 *   墙   —— 每格是这一镜的播放器，底栏是进度和「首帧 / 成片」两个重出按钮
 *   抽屉 —— 点一格滑出来，台词、景别、运镜、时长、提示词都在里面改
 *
 * 一次点击就能从"看"进到"改"再到"重出"，不用离开这一页。
 *
 * **侧边栏还是两步**（分镜、制作），都指向这儿。那两步的完成判据本来就
 * 不同——有分镜 vs 每镜都出到成片，是两个真实的里程碑，只是不该对应
 * 两个页面。`/bff/flow` 一行没改。
 */
import {
  computed,
  nextTick,
  onActivated,
  onDeactivated,
  onMounted,
  onUnmounted,
  ref,
  watch,
} from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { runAsyncJob } from '@/composables/useAsyncJob'
import {
  CAMERA_ANGLES,
  CAMERA_MOVES,
  SHOT_SIZES,
  TRANSITIONS,
  sizeLabel,
} from '@/api/labels'
import { humanTime, useAction } from '@/composables/useAction'
import { STEPS, useShots } from '@/composables/useShots'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

/**
 * **「这一集」那一层给的两样东西，原来一样都没接。**
 *
 * `@go` 和 `:can-publish` 是 EpisodeView 给每个子视图都绑上的（成片页那边
 * 声明了、用着）。这一页两个都没声明，于是 `@go` 变成一个没人听的属性——
 * 底下那个空状态想把人送去剧本页也送不了。
 *
 * `scriptChars` 是顺手要来的：那一层为了 tab 上那个「N 字」本来就读过剧本，
 * 这一页拿它分辨"还没有剧本"和"有剧本还没拆镜头"，不用再问一趟。
 */
const emit = defineEmits(['go'])
const props = defineProps({ scriptChars: { type: Number, default: null } })

const session = useSession()
const ui = useUi()
const { run, isBusy, error } = useAction()

// 镜头表、每镜进度、重出队列，全在这儿。见 useShots。
const {
  shots,
  episodeDuration, loading, load, bustOf, previewOf,
  pct, shotState, busy,
  start, starting, stop, shotAction, stepBtn, shotTone, running,
} = useShots()

const openId = ref('')
/**
 * 抽屉开关时的焦点。
 *
 * 这个抽屉是 `position: fixed; inset: 0` 的全屏遮罩，和那三个弹窗一个性质：
 * 用键盘按开它之后，焦点还留在**遮罩后面**那颗按钮上，按 Tab 是在看不见的
 * 页面里走；关掉之后焦点落到 `<body>`，下一次 Tab 得从整页开头重走。
 *
 * 开：焦点放到面板本身（tabindex="-1"），不猜第一个控件。盯的是面板出现
 * 那一刻——模板 ref 是响应式的，元素挂上来就聚焦。
 * 关：还给把它叫起来的那颗按钮。**只在从"没开"到"开"时记一次**，换一条
 * （不关抽屉直接点另一个）时不重记，否则记下的会是抽屉里面的元素。
 */
const panel = ref(null)
watch(panel, (el) => el?.focus())
let opener = null
watch(openId, (now, before) => {
  if (now && !before) {
    opener = document.activeElement
    return
  }
  if (!now) {
    const back = opener
    opener = null
    back?.focus?.()
  }
})

const draft = ref(null)
const selected = ref(new Set())
const filter = ref('all')
const doctor = ref(null)
const video = ref(null)

const blocked = computed(() => doctor.value && doctor.value.can_run === false)
const failedChecks = computed(() =>
  (doctor.value?.checks ?? []).filter((x) => x.level === 'fail' || x.level === 'error'),
)
const openShot = computed(
  () => shots.value.find((s) => s.shot_id === openId.value) ?? null,
)

// ---- 概览 ----

/**
 * 这一集真正会出多长。
 *
 * **别自己加 `duration_s`。** 那是名义值——从档位表里挑的整数，编辑器里改的
 * 也是它——而模型只能按格子出帧（Wan 4n+1、MiniMax-H3 17k+5），名义 4 秒
 * 出来是 107 帧 = 4.458 秒。这里原来是 `reduce((a, s) => a + s.duration_s)`，
 * 于是标题写「18 镜 · 58 秒」，而 ffprobe 量磁盘上的成片是 61.8 秒。
 *
 * 格子规则只有服务端知道（`stages::VideoLimits`，跟着模型和显存变），
 * 在前端复刻一份就是第三份副本，换模型就全错。用 `/api/shots` 回的那个数。
 *
 * 兜底才回退到累加：老版本的服务端不回这个字段，宁可显示个偏小的数，
 * 也别显示 0。
 */
const totalDuration = computed(() =>
  episodeDuration.value ??
  shots.value.reduce((a, s) => a + (s.real_duration_s ?? s.duration_s ?? 0), 0),
)
const problemCount = computed(
  () => shots.value.filter((s) => s.gate_notes?.length).length,
)
/** 还没出片的镜头数。0 就是这一集做完了。 */
const pending = computed(() => shots.value.filter((s) => !s.video_path).length)
/** 还没有首帧的镜头数。「只出首帧」那个按钮按它显示。 */
const pendingFrames = computed(() => shots.value.filter((s) => !s.frame_path).length)
/**
 * 锁着的有几镜。**「全部重出」要把这个数说出来。**
 *
 * 锁是人工确认过的意思，而批量那几颗按钮教给人的正是"锁着的动不了"——
 * 「退回重跑」会跳过它们并报一句「N 个锁定的没动」。可 force 那条路不认锁
 * （引擎 pick_for_frames 里 `if (force) { todo.push_back(s); continue; }`），
 * 「全部重出」会连锁着的一起重渲染。那一下也许正是人想要的（改了画风），
 * 但得在按之前说出来，不能让"锁"在两个按钮上是两个意思。
 */
const lockedCount = computed(
  () => shots.value.filter((s) => s.status === 'locked').length,
)

/**
 * id 到名字。
 *
 * 卡片上显示 c_lao_wang、loc_security_room 是没法扫的，要显示「老王」
 * 「安保室」。名字在项目那份摘要里，分镜表里只有 id——这正是这套系统的
 * 设计，界面负责把 id 翻回人话。
 */
const charName = computed(() => {
  const map = new Map(session.characters.map((c) => [c.char_id, c.name]))
  return (id) => map.get(id) ?? id
})
const locName = computed(() => {
  const map = new Map(session.locations.map((l) => [l.location_id, l.name]))
  return (id) => map.get(id) ?? id
})

/** 这一镜挂在哪个场景上。老分镜只填了 scene_id，两个字段都认。 */
function locationOf(shot) {
  const known = new Set(session.locations.map((l) => l.location_id))
  if (shot.location_id) return shot.location_id
  return known.has(shot.scene_id) ? shot.scene_id : ''
}

// 出分镜要先有角色和场景：分镜表里只能填已注册的 id，
// 库是空的话大模型编不出来，直接报「引用了未注册的资产」。
const missingAssets = computed(() => {
  const gaps = []
  if (!session.characters.length) gaps.push('角色')
  if (!session.locations.length) gaps.push('场景')
  return gaps
})

/**
 * 工具行左边那行读数。**只剩这一集多长**：镜数在 tab 上，画幅在项目页
 * 「这部片子」那一行，三处说同一件事的时候留最不重复的那一份。
 *
 * （这儿原来还算一个 `size`（「竖屏 高清 · 704×1280」）然后 `void size`
 * 扔掉，注释说"留着算，格子比例还要它"——不对：`cellRatio` 自己从
 * `video` 算，一个字都没用到它。算完就丢的东西删掉，别让下一个人以为
 * 那行字还有别的用处。）
 */
const tagline = computed(() => {
  if (!shots.value.length) return ''
  return humanTime(totalDuration.value)
})

/**
 * 每格的画幅跟项目的 [video] 走，不是写死竖屏。
 *
 * 格子里是 `object-fit: contain` 的播放器——槽的比例和片子对不上就会留
 * 黑边。写死 9:16 的话，横屏项目的每张牌都是上下两条黑、中间一小条画面。
 */
const cellRatio = computed(() => {
  const v = video.value
  if (!v?.width || !v?.height) return '9 / 16'
  return `${v.width} / ${v.height}`
})

// ---- 筛选 ----

const FILTERS = [
  { key: 'all', label: '全部' },
  { key: 'todo', label: '未完成' },
  { key: 'problem', label: '有问题' },
]

const shown = computed(() => {
  if (filter.value === 'todo') {
    return shots.value.filter(
      (s) => !['final_done', 'locked', 'fallback'].includes(s.status),
    )
  }
  if (filter.value === 'problem') {
    return shots.value.filter(
      (s) => s.gate_notes?.length || s.status.endsWith('rejected'),
    )
  }
  return shots.value
})

// ---- 抽屉 ----

/**
 * 抽屉里能改的那几项。**一份，两处用**：判"改了没保存"和拼 patch。
 *
 * 原来这张表在两个地方各写了一遍（`draftDirty` 和 `saveShot`），而它们
 * 一旦漂开，坏法都是不报错的那种：只加进保存那份，角标和关抽屉时那句
 * 「改了还没保存」看不见这一项，改完点外面就没了；只加进判脏那份，角标
 * 亮着而保存不发它，那一项永远回不到"已保存"。
 *
 * 台词（`dialogue_texts`）不在这张表里：它是个数组，两处都另外按值比。
 * 引擎那边的白名单见 editing.cpp 的 `allowed_keys()`——那儿还多一个
 * `status`，改状态走的是批量那条路（锁定/解锁），不从抽屉发。
 */
const DRAFT_KEYS = [
  'visual_desc', 'first_frame_prompt', 'motion_prompt', 'negative_prompt',
  'subtitle_text', 'beat', 'shot_size', 'camera_angle', 'camera_move',
  'transition_in', 'transition_dur_s', 'duration_s', 'needs_lipsync',
]

const draftDirty = computed(() => {
  if (!draft.value) return false
  const was = shots.value.find((s) => s.shot_id === draft.value.shot_id)
  if (!was) return false
  if (DRAFT_KEYS.some((k) => draft.value[k] !== was[k])) return true
  const nowLines = draft.value.dialogue_texts ?? []
  const wasLines = (was.dialogue ?? []).map((d) => d.text)
  return JSON.stringify(nowLines) !== JSON.stringify(wasLines)
})

/**
 * 换转场，**顺手把时长带对**。
 *
 * 引擎那边这两项是一对（`Shot::validate`）：硬切的转场时长必须是 0，
 * 非硬切的必须大于 0。而界面上它们是两个独立控件，硬切时只把时长输入框
 * `disabled` 掉——**禁用不等于把值改成 0**。于是：
 *
 *   一镜本来是「溶解 0.5 秒」，改成硬切 → 输入框灰掉，值还是 0.5 →
 *   保存 400「硬切的转场时长必须为 0」。而那个必须改成 0 的框正灰着，
 *   人改不了；退回溶解再填 0 又撞另一条「必须大于 0」。**这一镜从此存不
 *   进去了**，除非把转场原样改回去。
 *
 * 反方向（硬切改成溶解）人还能自己填个数救回来，但也不该让他撞一次 400
 * 才知道。两边都在这儿一次带对。
 */
function pickTransition(v) {
  if (!draft.value) return
  draft.value.transition_in = v
  if (v === 'cut') draft.value.transition_dur_s = 0
  else if (!(draft.value.transition_dur_s > 0)) draft.value.transition_dur_s = 0.5
}

function openDraft(shot) {
  openId.value = shot.shot_id
  draft.value = {
    ...shot,
    dialogue_texts: (shot.dialogue ?? []).map((d) => d.text),
  }
}

/** 抽屉真收起来的那两行。**已经问过的地方别再走 close()**，见下面。 */
function shut() {
  openId.value = ''
  draft.value = null
}

/** 点一格。开着同一镜就收起，否则换过去。 */
function toggle(shot) {
  // 换镜头之前先问一句。改了提示词直接切走，改动就无声无息没了，
  // 而用户以为「点回来还在那儿」。
  if (draftDirty.value && !confirm('这一镜有改动还没保存，切走就没了。确定？')) {
    return
  }
  // **这儿原来调的是 close()，而它自己也要问一次**——点开着那一镜的牌子
  // 收抽屉，同一件事连弹两个一模一样的框：答应了第一个才看得到第二个，
  // 第二个上按「取消」抽屉还留着。问过了就直接收。
  if (openId.value === shot.shot_id) {
    shut()
    return
  }
  openDraft(shot)
}

function close() {
  if (draftDirty.value && !confirm('这一镜有改动还没保存，关掉就没了。确定？')) {
    return
  }
  shut()
}

/** 抽屉开着的时候，↑↓ 换镜头、Esc 收起。 */
function onKey(event) {
  if (!openId.value) return
  if (event.key === 'Escape') {
    close()
    return
  }
  if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return
  // 在输入框里按方向键是移动光标，不该跳镜头
  const tag = document.activeElement?.tagName
  if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return
  event.preventDefault()
  const list = shown.value
  const at = list.findIndex((s) => s.shot_id === openId.value)
  const to = at + (event.key === 'ArrowDown' ? 1 : -1)
  if (at < 0 || to < 0 || to >= list.length) return
  // **问同一句。**
  //
  // 这儿原来是「有改动就 return」：数据是保住了，但按下去什么都不发生、
  // 也没有任何解释。鼠标点另一格会问「这一镜有改动还没保存，切走就没了」，
  // 键盘却只是不动——同一件事两种脾气，而这一页上「有没有改动」根本没有
  // 角标（只有保存按钮亮不亮），人按两下 ↓ 只会以为方向键坏了。
  //
  // 答应了之后 draft 整个换成新那一镜，不脏了，所以连按不会一路弹窗。
  if (draftDirty.value && !confirm('这一镜有改动还没保存，切走就没了。确定？')) {
    return
  }
  openDraft(list[to])
  nextTick(() => {
    document
      .querySelector(`[data-shot="${list[to].shot_id}"]`)
      ?.scrollIntoView({ behavior: 'smooth', block: 'nearest' })
  })
}

// ---- 出分镜 ----

async function generate() {
  if (!session.episodeId) {
    ui.warn('先选一集')
    return
  }
  // **开工那一刻把项目和集号钉死。**
  //
  // 这一趟分三段：先取剧本，再等那条 socket 开（runAsyncJob 最多等两秒），
  // 然后才把请求发出去。而下面原来每一段都现读一次 session——中间在顶栏
  // 换一集的话，发出去的是**上一集的剧本配这一集的集号**，整张分镜表就是
  // 照着别人的剧本拆的，而且一个字都不会报错：长度、台词、场景看着都对，
  // 只是不是这一集的故事。设定页那条「一键出图」早就这么钉了，理由一样。
  const project = session.projectPath
  const episodeId = session.episodeId
  if (shots.value.length && !confirm('重出分镜会覆盖整张表，手改过的镜头会丢。继续？')) {
    return
  }
  const scriptData = await run(
    () => api.getScript(project, episodeId),
    { key: 'plan', quiet: true },
  )
  if (!scriptData?.script?.trim()) {
    // **"读不到"和"还没写"是两回事。** quiet 把提示条压住了，这儿要是一律
    // 说「还没有剧本」，人就会去写一篇已经写过的——而真正的原因（引擎连
    // 不上、这一集不在了）一个字都没有。run() 每次进来先把 error 清空，
    // 所以这会儿它装的就是这一趟的。
    ui.warn(
      error.value
        ? `读不到这一集的剧本：${error.value}`
        // **不要写「回第二步」。** 那是八步那会儿的编号，剧本当时自己占
        // 一页；现在剧本就在这一页上面那一排的第一格（同一个 EpisodeView
        // 的「剧本」那格），而「第二步」现在是故事。照旧那句话找，人会走到
        // 一个跟这一集无关的地方去。
        : '这一集还没有剧本。上面切到「剧本」那一格，自己写或者让 AI 改编一版',
    )
    return
  }
  const result = await run(
    () =>
      runAsyncJob(
        (extra) =>
          api.plan({
            project,
            script: scriptData.script,
            episode_id: episodeId,
            duration_s: scriptData.target_duration_s || 60,
            ...extra,
          }),
        { prefix: 'plan', label: '拆分镜' },
      ),
    { key: 'plan', refresh: true },
  )
  if (result) {
    const target = scriptData.target_duration_s || 60
    const missing = result.missing_lines ?? []
    // 出短了要说出来。60 秒的集出过两镜六秒——提示词里"合计 16 个镜头"
    // 一个字没少，模型照样只出两镜，然后静静地存下去，到成片才发现。
    if (result.duration_s < target * 0.8) {
      ui.warn(
        `只排到 ${humanTime(result.duration_s)}，目标 ${humanTime(target)}。分镜太少，重出一次，或者回剧本把内容写足`,
      )
    } else if (missing.length) {
      // 补完还漏，说明落位那一步也没兜住。不拦，但要让人看见。
      const head = missing.slice(0, 2).map((s) => `「${s}」`).join('、')
      ui.warn(
        `出了 ${result.shots} 个镜头，但剧本里有 ${missing.length} 句台词没排进去：${head}${missing.length > 2 ? ' 等' : ''}。在镜头里补上，或者重出一次`,
      )
    } else if (result.placed_lines) {
      // 分镜模型不搬台词（实跑九句只写两句），所以引擎照剧本把漏的补了。
      // 台词和说话人都是准的，位置是估的——说一声，人可以拖一下改。
      ui.ok(
        `出了 ${result.shots} 个镜头，共 ${humanTime(result.duration_s)}。其中 ${result.placed_lines} 句台词是照剧本自动排进去的，位置可以在镜头里调`,
      )
    } else {
      ui.ok(`出了 ${result.shots} 个镜头，共 ${humanTime(result.duration_s)}`)
    }
    // **没落到场景上的那几镜单独说一句。**
    //
    // 和上面几条不是一回事，所以不并进那个 if 链：上面说的是"长度够不够、
    // 台词漏没漏"，这条说的是画面。没有场景的镜头出首帧时**整段场景描述
    // 丢掉**，同一个咖啡馆的几镜会各画各的，而全程不报错。
    // 2026-09-13 实测 walk_c ep01：18 镜里 13 镜是空的。
    const noLoc = result.shots_without_location ?? 0
    if (noLoc > 0 && session.locations.length > 0) {
      ui.warn(
        `其中 ${noLoc} 个镜头没挑场景。这几镜出首帧时不会带上场景的外观描述，` +
          `同一个地方的几镜可能各画各的——在镜头里挑一下，或者重出一次`,
      )
    }
    await load()
  }
}


/**
 * 抽屉里按保存。
 *
 * ⚠️ **只送真改过的那几项。** 这儿原来是把十三个字段连同台词整份发过去，
 * 而引擎判"要不要退回重跑"看的是**patch 里有没有这个键**，不是值变没变
 * （editing.cpp：`if (!it.value().is_null() && visual_keys().count(...))`
 * ——那套语义是给 Python 客户端定的，那边 patch 是 `model_dump
 * (exclude_none=True)`，只有调用方真设过的字段才会出现）。
 *
 * visual_keys 是 first_frame_prompt / motion_prompt / negative_prompt /
 * shot_size / camera_angle / camera_move / duration_s——七项里有四项是
 * 每个镜头都有值的，所以整份发过去等于**每一次保存都必然 touched_visual**：
 *
 *   · 只改了一个字幕错别字、只换了个转场（这两项都不在 visual_keys 里），
 *     按下保存，这一镜照样从「成片完成」退回「未开工」、attempts 清零、
 *     闸门备注清空——下一轮出片会把它整个重渲染一遍，几十秒到几分钟的卡；
 *   · **锁定的镜头也会被解掉**：reset 那一支直接写 status = PLANNED，而
 *     锁正是为了护住人工审过的那几镜（批量重置就特意跳过它们）。
 *
 * 按键比对现成的：`draftDirty` 比的就是这十三项加台词，照它来。
 */
async function saveShot() {
  if (!draft.value) return
  const was = shots.value.find((s) => s.shot_id === draft.value.shot_id)
  const patch = {}
  for (const k of DRAFT_KEYS) {
    if (!was || draft.value[k] !== was[k]) patch[k] = draft.value[k]
  }
  const nowLines = draft.value.dialogue_texts ?? []
  const wasLines = (was?.dialogue ?? []).map((d) => d.text)
  // 台词那一项引擎自己是逐条比值的（改了才清配音、才算动过画面），
  // 一并按"变没变"送，免得空跑一趟比较。
  if (!was || JSON.stringify(nowLines) !== JSON.stringify(wasLines)) {
    patch.dialogue_texts = nowLines
  }
  // 按钮判着 draftDirty，正常走不到这儿；真空了就别发——空 patch 在引擎
  // 那边是一次白存（照样重写 project.json）。
  if (!Object.keys(patch).length) return
  const result = await run(
    () =>
      api.saveShot({
        project: session.projectPath,
        episode_id: session.episodeId,
        shot_id: draft.value.shot_id,
        patch,
      }),
    { key: 'save', refresh: true },
  )
  if (!result) return
  // **字段名是 `reset_to_planned`，不是 `reset`。**
  //
  // 引擎回的是 `{saved, reset_to_planned, status}`（editing.cpp 那个
  // post_shot 的结尾），而这儿读的 `result.reset` 永远是 undefined——于是
  // 这句话**从来没出现过**。
  //
  // 而它要说的事天天发生：动了画面那几项——引擎的 visual_keys 是首帧提示
  // 词、运镜提示词、负向、镜别、机位、运镜、时长，外加台词改没改——它就把
  // 这一镜退回 PLANNED、重试次数归零（`const bool reset = touched_visual
  // && !patch_has_status;`）。抽屉里改一句提示词存一下，格子就从「成片完
  // 成」变回「未开工」，而屏幕上只说了「已保存」——人第一反应是刚才那一镜
  // 的成片丢了。
  //
  // （visual_desc 不在那张表里：它不进出图提示词，只在接着往下拆分镜时
  //  当上一镜的尾巴用——storyboard_run.cpp 里那句 prev_tail。上面那段
  //  只送改过的字段，就是为了让这句话只在真该出现的时候出现。）
  ui.ok(result.reset_to_planned ? '已保存，这一镜退回重跑' : '已保存')
  await load()
  // 重新读一遍之后抽屉要跟着新数据走，否则「未保存」的提示会一直挂着
  const fresh = shots.value.find((s) => s.shot_id === openId.value)
  draft.value = fresh
    ? { ...fresh, dialogue_texts: (fresh.dialogue ?? []).map((d) => d.text) }
    : null
}

// ---- 顺序 ----

/**
 * 调镜头顺序。
 *
 * 只在「全部」筛选下开放：筛过之后墙上少了几格，往前挪一格到底是挪到
 * 相邻那一镜前面、还是挪到被筛掉的那一镜前面，说不清楚。
 * 说不清楚的操作不如不给。
 */
const canReorder = computed(() => filter.value === 'all' && shots.value.length > 1)

async function applyOrder(order) {
  // 先在本地摆好。等一个来回再动的话，连拖两下会按旧顺序算第二下。
  const byId = new Map(shots.value.map((s) => [s.shot_id, s]))
  shots.value = order.map((id, i) => ({ ...byId.get(id), order: i }))

  const result = await run(
    () =>
      api.reorderShots({
        project: session.projectPath,
        episode_id: session.episodeId,
        shot_ids: order,
      }),
    { key: 'reorder', quiet: true },
  )
  if (!result) {
    // 没存上就把界面退回去，免得看着是一回事、跑出来是另一回事
    ui.error('顺序没存上：' + (error.value || '未知原因'))
    await load()
  }
}

/**
 * 墙上拖拽换位。
 *
 * 原来列表上是两个 ▲▼ 按钮。墙上直接拖更直观——格子本来就是画面，
 * 拖到哪儿就是排到哪儿。触屏没有原生拖放，所以抽屉里留了一对上下按钮。
 */
const dragId = ref('')
const dragOverId = ref('')

function onDragStart(shot, event) {
  if (!canReorder.value) return
  dragId.value = shot.shot_id
  event.dataTransfer.effectAllowed = 'move'
  // Firefox 不设 data 就不触发 drop
  event.dataTransfer.setData('text/plain', shot.shot_id)
}

function onDragOver(shot, event) {
  if (!dragId.value || shot.shot_id === dragId.value) return
  event.preventDefault()
  dragOverId.value = shot.shot_id
}

async function onDrop(shot) {
  const from = dragId.value
  dragId.value = ''
  dragOverId.value = ''
  if (!from || from === shot.shot_id) return
  const order = shots.value.map((s) => s.shot_id)
  const at = order.indexOf(from)
  const to = order.indexOf(shot.shot_id)
  if (at < 0 || to < 0) return
  order.splice(to, 0, ...order.splice(at, 1))
  await applyOrder(order)
}

/** 抽屉里的上下挪。触屏上拖不动，这一对是给它们留的。 */
async function move(shot, delta) {
  const order = shots.value.map((s) => s.shot_id)
  const at = order.indexOf(shot.shot_id)
  const to = at + delta
  if (at < 0 || to < 0 || to >= order.length) return
  order.splice(to, 0, ...order.splice(at, 1))
  await applyOrder(order)
}

// ---- 批量 ----

function toggleSelect(shotId) {
  const next = new Set(selected.value)
  if (next.has(shotId)) next.delete(shotId)
  else next.add(shotId)
  selected.value = next
}

/**
 * 选中的那几镜，什么时候该作废。
 *
 * `selected` 装的是 shot_id，而批量那几颗按钮（退回重跑、锁定、解锁、清备注，
 * 还有「选中的一起重出」）直接把它整份交出去。它原来从不清空：
 *
 *   · **换了筛选**：在「全部」里挑了三镜，切到「有问题」——那三镜多半已经
 *     不在屏幕上了，而顶上仍写着「选中 3」，一按就是对看不见的东西动手。
 *     「退回重跑」会把已经渲染好的成片丢掉，这种事不能发生在看不见的行上。
 *   · **换了集**：shot_id 是 `ep01_s03_sh007` 这种、带着集号前缀（见引擎
 *     storyboard.cpp 里 `shot_id = episode_id + buf`），所以交到 ep02 上是
 *     404 而不是改错东西——但人看到的是「选中 3」配一句莫名其妙的报错。
 *   · **整张表换了**（重出分镜）：老 id 全成了幽灵。
 *
 * 前两条各清一次；第三条按"还在不在表里"剪一遍，这样重出之后留下的是交集
 * 而不是一堆死 id。
 */
watch(filter, () => {
  selected.value = new Set()
})
watch(
  () => [session.projectPath, session.episodeId],
  () => {
    selected.value = new Set()
    // **抽屉也要跟着关。**
    //
    // 换集之后 `openShot` 找不到那个 shot_id 了（id 带集号前缀），模板上
    // 那道 `v-if="draft && openShot"` 会把抽屉藏起来——看着像关了。可
    // `draft` 和 `openId` 还在：`draftDirty` 照样算成"有改动"，于是在**新
    // 这一集**点第一个镜头，弹出来的是「这一镜有改动还没保存，切走就没
    // 了」——说的是一个已经看不见、还属于上一集的镜头。
    openId.value = ''
    draft.value = null
  },
)
watch(shots, (list) => {
  if (!selected.value.size) return
  const alive = new Set(list.map((s) => s.shot_id))
  const next = new Set([...selected.value].filter((id) => alive.has(id)))
  if (next.size !== selected.value.size) selected.value = next
})

function selectAllShown() {
  if (selected.value.size === shown.value.length && shown.value.length) {
    selected.value = new Set()
    return
  }
  selected.value = new Set(shown.value.map((s) => s.shot_id))
}

async function batch(action) {
  if (!selected.value.size) return
  const labels = { reset: '退回重跑', lock: '锁定', unlock: '解锁', clear_notes: '清掉闸门备注' }
  const result = await run(
    () =>
      api.batchShots({
        project: session.projectPath,
        episode_id: session.episodeId,
        shot_ids: [...selected.value],
        action,
      }),
    { key: 'batch', refresh: true },
  )
  if (!result) return
  // **报真改了几个，不是报点了几个。**
  //
  // 引擎回的是 `{changed, total, skipped_locked}`，而这儿原来数的是选中数。
  // 差别是实打实的：`reset` 会跳过锁定的镜头（引擎那句注释——「锁定的镜头
  // 是人工确认过的，批量重置不该动它们，否则一次误操作就把已经审过的片全
  // 废了」），`lock` 跳过本来就锁着的，`clear_notes` 跳过没有备注的。
  //
  // 挑十镜、其中三镜锁着，按「退回重跑」——屏幕说「10 个镜头已退回重跑」，
  // 而那三镜原样不动。人接着会去纳闷为什么它们还挂着「成片完成」，
  // 而真正的答案（锁着，所以护住了）引擎明明算好了送过来。
  const done = result.changed ?? 0
  const kept = result.skipped_locked ?? 0
  if (done) {
    ui.ok(`${done} 个镜头已${labels[action]}` + (kept ? `，${kept} 个锁定的没动` : ''))
  } else if (kept) {
    ui.info(`选中的 ${kept} 镜都锁着，没有动。要重跑先解锁`)
  } else {
    ui.info('这几镜本来就是这样，没有要改的')
  }
  selected.value = new Set()
  await load()
}

/**
 * 顶上那个主按钮。
 *
 * 还有没出的就接着往下跑（不 force）。都出完了的时候它是「全部重出」，
 * 那就**必须带 force**——不带的话每一镜都已经是终态，引擎一个都挑不到，
 * 跑完什么都没变而且不报错，按钮点了像是没反应。
 */
async function startAll() {
  if (!pending.value) {
    const locked = lockedCount.value
    // 锁着的也会跟着重跑——见 lockedCount 上面那段。
    const note = locked ? `（含锁定的 ${locked} 镜）` : ''
    if (!confirm(`这一集已经全部出完了。重出会把每一镜${note}从头再跑一遍，确定？`)) {
      return
    }
  }
  const r = await start([], null, !pending.value)
  if (r.ok) return
  // 409 = 已经在跑了（多半是另一个浏览器、或者另一个标签页点的）。
  // **那不是错误**，跟着看进度就行——轮询和 WebSocket 进页面就开着了。
  if (r.error?.status === 409) ui.info('已经在跑了，下面跟着看进度就行')
  // r.error 是空的那种：上一发还在路上，start() 直接挡住了——什么都没发生，
  // 就别报错。见 useShots 里的 starting。
  else if (r.error) ui.error('起不来：' + r.error.message)
}

/**
 * 只把首帧出出来，不出视频。
 *
 * **先看一眼构图再决定要不要花那两分钟。** 首帧一张约一分钟，视频一镜约
 * 两分钟；构图不对的话视频跑得再好也是白跑。一集二十几镜先把首帧铺开，
 * 扫一眼哪几镜不对、改完提示词再出片，比整集跑完再返工省得多。
 *
 * 缺首帧时**连配音一起跑**：出首帧要求这一镜的配音已经跑完（时长锁了，
 * 入口状态是 AUDIO_DONE）。只发 `frames` 的话，还停在「未开工」的那些
 * 一个都挑不到——点了什么都不会发生，而且不报错。
 * 全部重出时反过来：force 之下 pick 不看状态，就别再把配音重跑一遍了。
 */
async function startFrames() {
  const missing = pendingFrames.value
  if (!missing) {
    const locked = lockedCount.value
    const note = locked ? `（含锁定的 ${locked} 镜）` : ''
    if (
      !confirm(`每一镜都已经有首帧了。重出会把它们${note}全部换掉（视频不动），确定？`)
    ) {
      return
    }
  }
  const r = missing
    ? await start([], ['audio', 'frames'], false)
    : await start([], ['frames'], true)
  if (r.ok) return
  if (r.error?.status === 409) ui.info('已经在跑了，下面跟着看进度就行')
  // r.error 是空的那种：上一发还在路上，start() 直接挡住了——什么都没发生，
  // 就别报错。见 useShots 里的 starting。
  else if (r.error) ui.error('起不来：' + r.error.message)
}

/** 选中的那几镜一起重出某一段。挑十几个要重做的，一次交出去。 */
async function batchRerun(step) {
  const ids = [...selected.value]
  if (!ids.length) return
  selected.value = new Set()
  const r = await start(ids, [step.id])
  if (!r.ok && r.error) ui.error('起不来：' + r.error.message)
}

// ---- 体检和画幅 ----

async function loadDoctor() {
  // **和下面 loadVideo 同一道闸。** 两条是同一个 watch 里一前一后发的，
  // 而这一条**更慢**：体检里有三项要发网络请求、各自 8 秒超时，最坏
  // 二十多秒；画幅那条只是读两个文件。于是它反而是更容易后落地的那个。
  //
  // 落错了不是显示偏一点：`doctor.can_run` 决定 `blocked`，而 blocked
  // 关着「开始出片」和「只出首帧」两颗按钮、还会在墙上摆一段「还不能跑」
  // 的说明。上一部剧的结论落到这一部头上，表现是这一部明明能跑却被拦住
  // （或者反过来），而理由写的是另一台机器/另一部剧的事。
  const want = session.projectPath
  try {
    const got = await api.doctor(want)
    if (want !== session.projectPath) return
    doctor.value = got
  } catch (err) {
    if (want !== session.projectPath) return
    doctor.value = {
      can_run: false,
      checks: [{ name: '体检', level: 'error', detail: err.message }],
    }
  }
}

async function loadVideo() {
  if (!session.projectPath) return
  // 换剧时慢的那趟后落地，画幅就是上一部的——而它决定这一页每个格子的
  // 长宽比和"竖屏/横屏"那句话
  const want = session.projectPath
  try {
    const got = await api.projectVideo(session.projectPath)
    if (want !== session.projectPath) return
    video.value = got
  } catch {
    if (want === session.projectPath) video.value = null
  }
}

// ---- 按下去之前先说清楚 ----
//
// `/api/run/preview` 的头注释写着它存在的理由：「以前只能按下开始再看，
// 一按就是几十分钟。哪些镜头会重做、总共要等多久，这两件事应该在按下去
// 之前就知道。」——接口写好了，界面上却一直没有一处调它。
//
// **不做成一个「预演」按钮。** 要人先点一下才知道要等多久，等于多一道
// 关卡，而多数人不会点；这是一句话的信息量，直接摆在按钮旁边。GET 很便宜
// （不碰模型，只数镜头状态），跑起来之后就不显示了——那时候进度条说的
// 是同一件事，而且更准。
//
// **参数要和真按下去的那一下一致**：`force` 跟着主按钮走（都出完了时它
// 是「全部重出」），`skip_draft` 两边都默认真。不一致的预览比没有更糟：
// 它报的是另一件事，而人按它安排时间。

const preview = ref(null)

async function loadPreview() {
  if (!session.projectPath || !session.episodeId || !shots.value.length ||
      running.value) {
    preview.value = null
    return
  }
  const want = `${session.projectPath}::${session.episodeId}`
  const mine = () => want === `${session.projectPath}::${session.episodeId}`
  try {
    const got = await api.runPreview({
      path: session.projectPath,
      episode_id: session.episodeId,
      force: !pending.value,
    })
    // 人按这一行安排时间（「要等 24 分钟」），报的是别的集就更糟
    if (!mine()) return
    preview.value = got
  } catch {
    // 读不到就不显示。这一行是锦上添花，不该因为它整页红。
    if (mine()) preview.value = null
  }
}

/**
 * 抽屉开着的时候，引擎那头把这一镜改了——**人没动过的话就跟上**。
 *
 * 出片跑起来之后这一页每 6 秒重拉一次镜头表，而 `draft` 是开抽屉那一刻
 * 拷的一份，从来不跟。于是在抽屉里点「重出配音」等它跑完：引擎按配音时长
 * 反推 `duration_s` 并把 `duration_locked` 置真，而抽屉里还是旧的那个数、
 * 那个输入框也还开着。后果有两层：
 *
 *   · `draftDirty` 拿 draft 和刚拉回来的比，**凭空变成"有改动"**——关抽屉
 *     时弹一句「这一镜有改动还没保存」，而人一个字都没改；
 *   · 这时候真按了保存，`patch.duration_s` 送的是**锁定之前**那个数，
 *     引擎那边没有针对 `duration_locked` 的拦截（只有改台词那条会把它
 *     置假），于是配音反推出来的时长被一个旧值顶掉，而锁还挂着。
 *
 * 判据用现成的 `draftDirty`：**只在人没改过的时候跟**，改过就一个字不动。
 * 和资产页 load 里那条是同一个规矩。相等就不换对象，免得每 6 秒白重绘
 * 一次抽屉。
 */
watch(shots, () => {
  if (!draft.value || draftDirty.value) return
  const fresh = shots.value.find((s) => s.shot_id === draft.value.shot_id)
  if (!fresh) return
  const next = {
    ...fresh,
    dialogue_texts: (fresh.dialogue ?? []).map((d) => d.text),
  }
  if (JSON.stringify(next) !== JSON.stringify(draft.value)) draft.value = next
})

watch(
  () => session.projectPath,
  () => {
    loadVideo()
    // 体检里有一项跟着这部剧的画幅走，换剧要重查。这一格被 KeepAlive
    // 冻着，onMounted 只跑一次——不重查的话那一项一直是上一部的答案。
    loadDoctor()
  },
  { immediate: true },
)
// 镜头数、还差几镜、跑没跑完——任何一个变了，这句话就该重算。
// 跑的过程中不算（上面那个卫语句挡着），停下来那一刻会算一次。
watch(
  () => [session.episodeId, shots.value.length, pending.value, running.value],
  loadPreview,
  { immediate: true },
)

/**
 * 刷新之前拦一下：抽屉里改了没存的那一镜。
 *
 * 切走和关抽屉都会问一句（见 toggle / close），唯独刷新和关标签页不会——
 * 而那两下丢的是同样的东西：提示词、台词、时长，改了半天一下没了。
 */
function beforeUnload(e) {
  if (!draftDirty.value) return
  e.preventDefault()
  e.returnValue = ''
}

onMounted(() => {
  // 体检那一趟上面那个 watch 已经带着 immediate 跑过了，这儿不用再来一遍
  // ——一趟体检里有三项要发网络请求，最坏二十多秒。
  window.addEventListener('keydown', onKey)
  // **这一条不跟着 onActivated 走。** 人切到剧本格去了，抽屉里那份改动
  // 还在（组件只是停用，draft 没清），刷新照样丢。
  window.addEventListener('beforeunload', beforeUnload)
})
onUnmounted(() => {
  window.removeEventListener('keydown', onKey)
  window.removeEventListener('beforeunload', beforeUnload)
})

/**
 * **切到别的格子就把键盘让出去。**
 *
 * 这一页被「这一集」的 `<KeepAlive>` 冻着——切到剧本 / 成片，组件是
 * **停用**不是卸载，`onUnmounted` 不会跑，这个 keydown 监听照样挂在
 * window 上。而 `onKey` 只看 `openId` 非空，切 tab 并不会关抽屉：
 *
 *   在镜头格点开一镜 → 切到成片格 → 按 ↑ / ↓
 *   → `preventDefault()` 照常执行，**页面和播放器都没法用方向键了**，
 *     而它真正做的事（换到上一镜 / 下一镜）发生在一个看不见的抽屉里。
 *
 * Esc 同理：在别的格子上按 Esc 会把那个看不见的抽屉关掉。
 *
 * 重复 add 同一个函数引用是安全的（DOM 会去重），所以首次挂载时
 * onMounted 和 onActivated 都跑一遍也没关系。
 */
onActivated(() => window.addEventListener('keydown', onKey))
onDeactivated(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="stack stack--lg">
    <div class="toolbar">
      <span v-if="tagline" class="tiny dim nowrap">{{ tagline }}</span>
      <span class="spacer" />
      <button
        v-if="running"
        class="btn btn--ghost btn--sm"
        type="button"
        @click="stop"
      >
        <AppIcon name="pause" :size="14" />
        停下
      </button>
      <template v-else>
        <button
          class="btn btn--ai"
          type="button"
          :disabled="!session.episodeId || isBusy('plan')"
          @click="generate"
        >
          <AppIcon name="sparkle" :size="15" />
          {{ isBusy('plan') ? '拆镜头中…' : shots.length ? 'AI 重出分镜' : 'AI 出分镜' }}
        </button>
        <!-- 先出首帧，看一眼构图再决定要不要花那两分钟出视频。 -->
        <button
          v-if="shots.length"
          class="btn btn--ghost btn--sm"
          type="button"
          :title="pendingFrames
            ? '先把缺的首帧铺开，不出视频'
            : '每一镜都有首帧了；点了会全部重出（视频不动）'"
          :disabled="blocked || starting"
          @click="startFrames"
        >
          <AppIcon name="image" :size="14" />
          {{ pendingFrames ? `只出首帧（差 ${pendingFrames}）` : '重出首帧' }}
        </button>
        <!-- 都出完了的时候这个按钮是「全部重出」，那就**必须带 force**：
             不带的话每一镜都已经是终态，引擎一个都挑不到，跑完什么都没变
             而且不报错——按钮点了像是没反应。 -->
        <button
          v-if="shots.length"
          class="btn btn--primary"
          type="button"
          :disabled="blocked || starting"
          @click="startAll"
        >
          <AppIcon name="film" :size="15" />
          {{ pending ? `出片（差 ${pending}）` : '全部重出' }}
        </button>
      </template>
    </div>

    <!-- 按下去之前的那一句。**跑起来就不显示**——那时候进度条说的是同一
         件事，而且更准。
         ⚠️ **时长走前端的 humanTime，不用引擎回的 `estimate_text`。**
         两边的格式不一样（引擎是「1.5 小时」「2 分钟」，前端是
         「1 小时 30 分」「1 分 30 秒」，见 cpp 那边的 human_time 语料），
         而这一页别处的时长——上面那行「18 镜 · 1 分 2 秒」、出完分镜那句
         「共 X」——全是前端这一套。混着用就是同一屏里两种写法。
         引擎两样都回，`estimate_s` 是秒数，直接拿它格式化。 -->
    <p v-if="preview && !running && !blocked" class="tiny dim prev">
      <template v-if="preview.idle">
        这一集每一镜都出到头了，点「全部重出」才会动。
      </template>
      <template v-else>
        这一次要跑：<template v-for="(st, i) in preview.stages" :key="st.stage"
          ><template v-if="i"> · </template><b>{{ st.label }} {{ st.shots }}</b></template
        ><template v-if="preview.estimate_s > 0">，约 {{ humanTime(preview.estimate_s) }}</template>
      </template>
    </p>

    <p v-if="session.hasProject && missingAssets.length" class="alert alert--warn">
      <AppIcon name="warn" :size="15" />
      <span>还没有{{ missingAssets.join('和') }}</span>
      <span class="spacer" />
      <RouterLink
        :to="missingAssets[0] === '角色' ? '/assets?tab=characters' : '/assets?tab=locations'"
        class="btn btn--ghost btn--sm"
      >
        先去出{{ missingAssets[0] }}
      </RouterLink>
    </p>

    <!-- **只在拦路时出现。** 全绿的时候一行都不占——
         体检的细节在设置页，这里只管"能不能开工"。 -->
    <section v-if="blocked && shots.length" class="sec">
      <div class="sec__head">
        <h2 class="sec__t">还不能开工</h2>
        <span class="spacer" />
        <div class="sec__acts">
          <button class="btn btn--ghost btn--sm" type="button" @click="loadDoctor">
            <AppIcon name="refresh" :size="14" />
            重新体检
          </button>
        </div>
      </div>
      <div class="stack stack--sm">
        <p v-for="c in failedChecks" :key="c.name" class="alert alert--bad">
          <AppIcon name="warn" :size="14" />
          <strong>{{ c.name }}</strong>
          <span class="alert__detail">{{ c.detail }}</span>
          <span v-if="c.fix" class="alert__fix tiny dim">{{ c.fix }}</span>
        </p>
      </div>
    </section>

    <!-- 「还没选到某一集」那个空状态挪到父页面了：进不到这一页就没有这一集，
         每个子视图各判一遍是三份同样的话。 -->
    <!-- **没有剧本时不要请人按「AI 出分镜」。** 那颗按钮第一件事就是去取
         剧本，取不到就弹一句「这一集还没有剧本，先回第二步写」——把人请进
         一条死路，还不给去路。分镜是照着剧本拆的，这时候该说的是这件事，
         并且直接送过去（成片页那个空状态早就是这么做的）。

         **判的是 `=== 0` 不是假值**：那一层还没读完时传过来的是 null，
         那时候"有没有剧本"还不知道。镜头表和剧本字数是两趟请求，镜头表
         先落地是常有的事——拿假值判的话，有剧本的集也会闪一下「还没有
         剧本」。不知道就走下面那个通用的空状态。 -->
    <EmptyState
      v-if="!loading && !shots.length && props.scriptChars === 0"
      icon="script"
      title="这一集还没有剧本"
      hint="分镜是照着剧本一场一场拆的，先把剧本写出来"
    >
      <button class="btn btn--primary btn--sm" type="button" @click="emit('go', 'script')">
        去写剧本
      </button>
    </EmptyState>

    <EmptyState v-else-if="!loading && !shots.length" icon="board" title="还没有分镜">
      <button
        class="btn btn--ghost btn--sm"
        type="button"
        :disabled="isBusy('plan')"
        @click="generate"
      >
        <AppIcon name="sparkle" :size="14" />
        AI 出分镜
      </button>
    </EmptyState>

    <template v-else>
      <!-- 时间轴：一集里哪几镜特别长、哪一段全是特写，扫一眼就知道，
           而在一面等大的墙上是看不出来的。 -->
      <div class="timeline">
        <div class="timeline__bars">
          <button
            v-for="s in shots"
            :key="s.shot_id"
            class="tl"
            :class="[`tl--${shotTone(s)}`, { 'tl--on': openId === s.shot_id }]"
            type="button"
            :style="{ width: Math.max(2, ((s.real_duration_s ?? s.duration_s ?? 0) / (totalDuration || 1)) * 100) + '%' }"
            :title="`${s.order + 1}. ${sizeLabel(s.shot_size)} ${(s.real_duration_s ?? s.duration_s)}s`"
            @click="toggle(s)"
          >
            <span class="tl__n">{{ s.order + 1 }}</span>
          </button>
        </div>
        <!-- 图例删了：四个色块一眼看得懂，格子上的状态字还写着同一件事。
             「N 镜适合做口型」也删了——流水线里没有口型这一步（Stage 枚举
             只有配音/首帧/草稿/成片/装配），给不存在的步骤计数是误导。
             有备注的才值得提一句，那是要人去看的。 -->
        <div v-if="problemCount" class="timeline__legend tiny">
          <span class="warn-text">{{ problemCount }} 镜有备注</span>
        </div>
      </div>

      <!-- 筛选与批量 -->
      <div class="toolbar">
        <div class="chips">
          <button
            v-for="f in FILTERS"
            :key="f.key"
            class="chip"
            :class="{ 'chip--on': filter === f.key }"
            type="button"
            :title="f.key === 'all' ? '' : '筛选时不能拖动排序'"
            @click="filter = f.key"
          >
            {{ f.label }}
          </button>
        </div>
        <span class="spacer" />
        <button class="btn btn--ghost btn--sm" type="button" @click="selectAllShown">
          {{ selected.size === shown.length && shown.length ? '取消全选' : '全选' }}
        </button>
        <template v-if="selected.size">
          <span class="tiny dim nowrap">选中 {{ selected.size }}</span>
          <button
            v-for="step in STEPS"
            :key="step.id"
            class="btn btn--ghost btn--sm"
            type="button"
            :disabled="starting"
            @click="batchRerun(step)"
          >
            <AppIcon :name="step.icon" :size="13" />
            重出{{ step.label }}
          </button>
          <button class="btn btn--ghost btn--sm" type="button" :disabled="isBusy('batch')" @click="batch('reset')">
            退回重跑
          </button>
          <button class="btn btn--ghost btn--sm" type="button" :disabled="isBusy('batch')" @click="batch('lock')">
            锁定
          </button>
          <button class="btn btn--ghost btn--sm" type="button" :disabled="isBusy('batch')" @click="batch('unlock')">
            解锁
          </button>
          <button class="btn btn--ghost btn--sm" type="button" :disabled="isBusy('batch')" @click="batch('clear_notes')">
            清备注
          </button>
        </template>
      </div>

      <!-- 镜头墙 -->
      <div class="wall" :style="{ '--cell-ratio': cellRatio }">
        <article
          v-for="s in shown"
          :key="s.shot_id"
          class="cell"
          :class="[
            `cell--${shotTone(s)}`,
            {
              'cell--live': busy(s.shot_id),
              'cell--open': openId === s.shot_id,
              'cell--drag': dragId === s.shot_id,
              'cell--over': dragOverId === s.shot_id,
            },
          ]"
          :data-shot="s.shot_id"
          :draggable="canReorder"
          @dragstart="onDragStart(s, $event)"
          @dragover="onDragOver(s, $event)"
          @dragleave="dragOverId = ''"
          @drop.prevent="onDrop(s)"
          @dragend="((dragId = ''), (dragOverId = ''))"
        >
          <div class="cell__frame">
            <!-- **出好的镜头直接就是播放器，不用点开。**
                 `preload="none"` + `poster`：不点播放就一个字节都不下，
                 所以二十二个播放器和二十二张缩略图一样轻。 -->
            <video
              v-if="s.video_path"
              :key="s.shot_id + ':' + bustOf(s.shot_id)"
              class="cell__video"
              :src="mediaUrl(session.projectPath, s.video_path) + '&_=' + bustOf(s.shot_id)"
              :poster="s.frame_path
                ? mediaUrl(session.projectPath, s.frame_path) + '&_=' + bustOf(s.shot_id)
                : undefined"
              controls
              playsinline
              preload="none"
            />
            <img
              v-else-if="s.frame_path"
              :src="mediaUrl(session.projectPath, s.frame_path) + '&_=' + bustOf(s.shot_id)"
              :alt="s.visual_desc"
              loading="lazy"
            />
            <AppIcon v-else name="image" :size="18" class="cell__blank" />

            <!-- **采样中途的预览。** 引擎每一步把潜空间投影成一张 88×160 的
                 小图推上来，盖在这一格上、放大到格子大小——低分辨率放大本来
                 就是糊的，随着步数推进内容逐渐成形。落定就没了（真图上来）。
                 pointer-events 关掉：底下要是播放器，别挡它的控件。 -->
            <img
              v-if="previewOf(s.shot_id)"
              class="cell__preview"
              :src="previewOf(s.shot_id)"
              alt=""
            />

            <input
              class="cell__check"
              type="checkbox"
              :checked="selected.has(s.shot_id)"
              :aria-label="`选中镜头 ${s.order + 1}`"
              @change="toggleSelect(s.shot_id)"
            />
          </div>

          <!-- **进度就是这一行的底色。** 铺成背景既不占地方，也比一条细线
               看得清——而且"跑到哪了"和"这一镜是什么"本来就该一起看。 -->
          <div class="cell__bottom">
            <span
              v-if="busy(s.shot_id)"
              class="cell__fill"
              :class="{ 'cell__fill--idle': pct(s.shot_id) === null }"
              :style="pct(s.shot_id) !== null ? { width: pct(s.shot_id) + '%' } : null"
            />
            <button class="cell__no numeric" type="button" title="改这一镜" @click="toggle(s)">
              {{ s.order + 1 }}
            </button>
            <!-- 格子上只有序号和状态。原来每格还有三个重出按钮（配音/首帧/
                 成片），16 镜就是 48 个按钮铺在一面什么都还没出的墙上；
                 重出是改的时候才要的事，抽屉页脚有同一排。 -->
            <button class="cell__state tiny truncate" type="button" title="改这一镜" @click="toggle(s)">
              {{ shotState(s) }}
            </button>
          </div>

          <!-- 闸门备注是"这一镜为什么没过"的全部答案（「首帧相似度 0.42
               低于 0.6」这种），而格子只有一行放得下。挂在 title 上，扫墙
               的时候不用一格格点开。抽屉里那份是不截断的。 -->
          <p
            v-if="s.gate_notes?.length"
            class="cell__notes tiny"
            :title="s.gate_notes.join('；')"
          >
            <AppIcon name="warn" :size="12" />
            <span class="truncate">{{ s.gate_notes.join('；') }}</span>
          </p>
        </article>
      </div>
    </template>

    <!-- 详情抽屉。点墙上任意一格滑出来。 -->
    <div v-if="draft && openShot" class="drawer" @click.self="close">
      <aside ref="panel" class="drawer__panel" tabindex="-1">
        <header class="drawer__head">
          <b class="numeric" :title="`${openShot.shot_id}（↑ ↓ 换镜头）`">{{ openShot.order + 1 }}</b>
          <span class="pill nowrap" :class="`pill--${shotTone(openShot)}`">
            {{ shotState(openShot) }}
          </span>
          <span class="spacer" />
          <!-- 触屏上拖不动格子，这一对是给它们留的 -->
          <button
            class="iconbtn"
            type="button"
            title="往前挪一格"
            :disabled="!canReorder || openShot.order === 0 || isBusy('reorder')"
            @click="move(openShot, -1)"
          >
            ▲
          </button>
          <button
            class="iconbtn"
            type="button"
            title="往后挪一格"
            :disabled="!canReorder || openShot.order === shots.length - 1 || isBusy('reorder')"
            @click="move(openShot, 1)"
          >
            ▼
          </button>
          <button class="iconbtn" type="button" title="收起（Esc）" @click="close">
            <AppIcon name="close" :size="15" />
          </button>
        </header>

        <div class="drawer__body stack stack--sm">
          <!-- **试了几次也要说。**
               `attempts` 是**累计**的（跨轮攒着，引擎拿它和设置页那个
               「每镜最多重试几次」比），而它在界面上一直没露过面。后果很
               具体：一镜把次数用完、状态成了「已降级」，人点「重出成片」
               ——引擎照跑（force 绕过状态），但只剩一次机会，失败了又回到
               降级。屏幕上看着就是"点了跟没点一样"。
               把这个数摆出来，那句「重试次数用完了」才有地方对。真要从头
               再来，是选中它按「退回重跑」——那条会把 attempts 清零。 -->
          <p v-if="openShot.gate_notes?.length" class="alert alert--warn">
            <AppIcon name="warn" :size="14" />
            <span>
              {{ openShot.gate_notes.join('；') }}
              <template v-if="openShot.attempts">
                （已试 {{ openShot.attempts }} 次；要从头再来，选中它按「退回重跑」）
              </template>
            </span>
          </p>

          <div class="row row--wrap tiny dim">
            <!-- **用全局那套 pill。** 这两个原来写的是 `tag tag--loc` /
                 `tag tag--char`，而这三个类名在这个组件的 scoped 里和
                 base.css 里都不存在——于是这一行上「安保室」「老王」是两段
                 光秃秃的字，而紧挨着它们的「拍子」用的是真正的 pill：同一
                 行里两种长相，看不出前两个也是牌子。 -->
            <span v-if="locationOf(openShot)" class="pill pill--neutral tiny">
              {{ locName(locationOf(openShot)) }}
            </span>
            <span
              v-for="cid in openShot.char_ids ?? []"
              :key="cid"
              class="pill pill--neutral tiny"
            >
              {{ charName(cid) }}
            </span>
            <span v-if="openShot.beat" class="pill pill--neutral tiny">{{ openShot.beat }}</span>
          </div>

          <p class="group">画面</p>
          <label class="field">
            <span class="field__label">画面描述</span>
            <textarea v-model="draft.visual_desc" class="textarea textarea--tight" rows="2" />
          </label>
          <label class="field">
            <span class="field__label">首帧提示词</span>
            <textarea
              v-model="draft.first_frame_prompt"
              class="textarea textarea--tight mono"
              rows="4"
              title="角色和场景的外观由程序拼进去，这里只写本镜特有的"
            />
          </label>
          <label class="field">
            <span class="field__label">运动提示词</span>
            <textarea v-model="draft.motion_prompt" class="textarea textarea--tight mono" rows="2" />
          </label>
          <p class="group">镜头语言</p>
          <div class="grid grid--pairs">
            <label class="field">
              <span class="field__label">景别</span>
              <select v-model="draft.shot_size" class="select">
                <option v-for="o in SHOT_SIZES" :key="o.value" :value="o.value">{{ o.label }}</option>
              </select>
            </label>
            <label class="field">
              <span class="field__label">机位</span>
              <select v-model="draft.camera_angle" class="select">
                <option v-for="o in CAMERA_ANGLES" :key="o.value" :value="o.value">{{ o.label }}</option>
              </select>
            </label>
            <label class="field">
              <span class="field__label">运镜</span>
              <select v-model="draft.camera_move" class="select">
                <option v-for="o in CAMERA_MOVES" :key="o.value" :value="o.value">{{ o.label }}</option>
              </select>
            </label>
            <label class="field">
              <span class="field__label">
                时长（秒）
                <span v-if="draft.duration_locked" class="tiny dim">配音已锁</span>
              </span>
              <!-- **`max="30"` 是接口的上限，不是模型的。**
                   引擎 `Shot::validate` 收 0~30 秒，所以填 20 存得进去；可真
                   正出多长由出片模型定——帧数要落在它的格子上（Wan 是 4n+1，
                   H3 是 17k+5），还会被它自己的帧数上限和显存压住，
                   `frames_for` 会往下夹。夹完的那个数就是 `real_duration_s`，
                   墙上牌子的 title 和成片页的时间轴用的都是它。
                   这儿不把上限改小：那个数前端拿不到（/bff/project/video 不
                   回 max_shot_s），照一个猜的数去拦反而会拦掉本来能出的。
                   所以是说清楚，让人知道去哪儿看真数。 -->
              <input
                v-model.number="draft.duration_s"
                class="input numeric"
                type="number"
                step="0.5"
                min="0.5"
                max="30"
                title="填的是名义时长。真正出多长由出片模型定：帧数要落在它的格子上，还会被它自己的上限压住（有的模型五秒就封顶）。牌子上那个秒数和成片页的时间轴用的都是真出来的那个数。"
                :disabled="draft.duration_locked"
              />
            </label>
          </div>

          <p class="group">声音与字幕</p>
          <div class="field">
            <span class="field__label">台词</span>
            <div v-if="draft.dialogue_texts?.length" class="stack stack--sm">
              <div v-for="(line, i) in draft.dialogue_texts" :key="i" class="dialogue">
                <!-- **说话人要翻成人话。** 这儿原来直接印 char_id，于是
                     台词行上是一列 `c_lin_wan`，而三行以上那排牌子写的是
                     「林晚」——同一个人在同一个抽屉里两种叫法，而这一块正
                     是要盯着"谁说了什么"改字的地方。charName 就在这一页
                     上（它旁边的注释写着"界面负责把 id 翻回人话"），认不
                     出来的 id 它原样回，比空着强。 -->
                <span class="dialogue__who tiny dim nowrap">
                  {{ charName(draft.dialogue[i]?.char_id) || '旁白' }}
                </span>
                <input v-model="draft.dialogue_texts[i]" class="input" />
                <span v-if="draft.dialogue[i]?.duration_s" class="tiny dim numeric nowrap">
                  {{ draft.dialogue[i].duration_s.toFixed(1) }}s
                </span>
              </div>
            </div>
            <p v-else class="tiny dim">无台词</p>
          </div>
          <label class="field">
            <span class="field__label">字幕</span>
            <input v-model="draft.subtitle_text" class="input" />
          </label>

          <!-- **折起来的三样：两样流水线还不做，一样很少改。**
               转场：装配走的是 `-f concat -c copy` 纯硬切，全树没有一处
               xfade，选了也不会渲染，只是记下来的意图。（2026-09-13 之前
               更糟：时间线按重叠算，字幕比画面早，每个 dissolve 累积 0.4 秒。）
               口型：Stage 枚举里没有这一步，这个勾只说"这一镜适合做"。
               负向提示词：全剧那份在项目页「这部片子」里，单镜的很少动。 -->
          <details class="fold">
            <summary class="fold__t">更多：负向提示词、转场、口型</summary>
            <div class="stack stack--sm">
              <label class="field">
                <span class="field__label">负向提示词</span>
                <textarea v-model="draft.negative_prompt" class="textarea textarea--tight mono" rows="2" />
              </label>
              <div class="grid grid--pairs">
                <label class="field">
                  <span class="field__label">转场</span>
                  <!-- **不用 v-model**：换转场的时候要顺手把时长带对，
                       见 pickTransition。 -->
                  <select
                    :value="draft.transition_in"
                    class="select"
                    title="记下来的意图；装配暂时是纯硬切，转场还没有渲染"
                    @change="pickTransition($event.target.value)"
                  >
                    <option v-for="o in TRANSITIONS" :key="o.value" :value="o.value">{{ o.label }}</option>
                  </select>
                </label>
                <label class="field">
                  <span class="field__label">转场时长</span>
                  <input
                    v-model.number="draft.transition_dur_s"
                    class="input numeric"
                    type="number"
                    step="0.1"
                    min="0"
                    max="2"
                    :disabled="draft.transition_in === 'cut'"
                    :title="draft.transition_in === 'cut' ? '硬切固定为 0' : ''"
                  />
                </label>
              </div>
              <label
                class="switch"
                title="按景别、机位和面朝方向自动推的，一般不用改。流水线暂时没有口型这一步，这个勾只是记下来"
              >
                <input v-model="draft.needs_lipsync" type="checkbox" />
                <span>适合做口型</span>
              </label>
            </div>
          </details>
        </div>

        <footer class="drawer__foot">
          <button
            class="btn btn--primary"
            type="button"
            :disabled="!draftDirty || isBusy('save')"
            @click="saveShot"
          >
            {{ isBusy('save') ? '保存中…' : '保存' }}
          </button>
          <!-- 改完提示词紧接着就想重出，别让人再去墙上找那一格 -->
          <button
            v-for="step in STEPS"
            :key="step.id"
            class="btn btn--ghost btn--sm"
            type="button"
            :title="stepBtn(openShot, step).title"
            @click="shotAction(openShot, step.id)"
          >
            <AppIcon :name="stepBtn(openShot, step).icon" :size="14" />
            {{ stepBtn(openShot, step).label }}
          </button>
        </footer>
      </aside>
    </div>
  </div>
</template>

<style scoped>
/* 按下去之前那一句。**比提醒行轻一档**——它不是坏消息，是一个数。
   `b` 只加重那几个数字，整句不加粗：加粗的是要扫的，不是要读的。 */
.prev {
  margin: calc(var(--s2) * -1) 0 0;
  line-height: 1.5;
}
.prev b {
  color: var(--text);
  font-weight: 600;
}

/* ---- 提醒行（只在坏状态下出现） ---- */
.alert {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin: 0;
  padding: var(--s2) var(--s3);
  border-radius: var(--r);
  font-size: var(--fs-sm);
  line-height: 1.5;
  /* 体检那几条带着多行的「怎么办」，不让换行的话它们会被挤成一条线 */
  flex-wrap: wrap;
}
/* 同设置页的 .check__detail：体检那几条的正文也可能是多行的（缺哪几个
   模型文件是一行一个）。 */
.alert__detail {
  white-space: pre-wrap;
}
/* 同设置页的 .check__fix：引擎把这段按多行写的，塌成一行就没法照着做了 */
.alert__fix {
  flex-basis: 100%;
  white-space: pre-wrap;
}
.alert--warn {
  background: var(--warn-soft);
  color: var(--warn);
}
.alert--bad {
  background: var(--danger-soft, var(--warn-soft));
  color: var(--danger);
}
.alert :deep(svg) { flex: none; }

/* 抽屉里那几对并排的字段（景别 / 机位 / 运镜、转场 / 转场时长）。
   **这一条原来不存在**：模板里写着 `grid grid--pairs`，而全局的 `.grid`
   只有 `display:grid` 和间距、一列都没给——于是那几对字段是竖着一行一个
   排的，抽屉比该有的高出一大截，"成对"这件事也看不出来。
   写法照设置页的 .grid--2 / .grid--3（那两条也是这么定的），下限取小一点：
   抽屉比设置页窄。 */
.grid--pairs {
  grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
}

/* 抽屉里的分段标题：画面 / 镜头语言 / 声音与字幕。
   原来也没有这一条，三段标题和正文一个样子，分段等于没分。
   照 .field__label 那一档（同一个抽屉里的标签就是它）。 */
.group {
  margin: var(--s4) 0 0;
  font-size: var(--fs-sm);
  font-weight: 600;
  color: var(--text-2);
}

/* 「更多：负向提示词、转场、口型」那个折叠。
   一字不差抄角色页那份（同一个控件、同一个位置感），这儿原来没有。 */
.fold__t {
  color: var(--text-3);
  font-size: var(--fs-xs);
  cursor: pointer;
}
.fold[open] .fold__t {
  margin-bottom: 4px;
}

/* ---- 时间轴 ---- */
.timeline {
  display: flex;
  align-items: center;
  gap: var(--s3);
  flex-wrap: wrap;
}
.timeline__bars {
  flex: 1 1 320px;
  min-width: 0;
  display: flex;
  gap: 2px;
  height: 26px;
}
.timeline__legend {
  display: flex;
  gap: var(--s3);
  flex: none;
  white-space: nowrap;
}
.tl {
  border: none;
  border-radius: 3px;
  padding: 0;
  min-width: 10px;
  cursor: pointer;
  background: var(--bg-sunken);
  color: var(--text-3);
  font-size: 10px;
  overflow: hidden;
}
.tl--info { background: color-mix(in srgb, var(--info) 45%, transparent); }
.tl--warn { background: color-mix(in srgb, var(--warn) 45%, transparent); }
.tl--ok   { background: color-mix(in srgb, var(--ok) 45%, transparent); }
.tl--on   { outline: 2px solid var(--accent); }
/* 这儿原来还有 .swatch 和它三个 modifier：时间线那条色带的图例，
   那一小排 2026-09-14 从模板里撤了，样式留了下来——模板里一处都没有
   `swatch`，这四条规则谁也命中不了。 */
/* ---- 筛选片 ---- */
.chips {
  display: flex;
  gap: 6px;
}
.chip {
  padding: 3px var(--s3);
  border-radius: var(--r-pill);
  border: 1px solid var(--line);
  background: var(--surface-2);
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.chip--on {
  background: var(--accent-soft);
  border-color: var(--accent-line);
  color: var(--accent);
  font-weight: 600;
}

/* ---- 镜头墙 ---- */
.wall {
  display: grid;
  /* 格子里是真的播放器，太窄的话浏览器自带的控件会挤成一团、进度条拖不动 */
  grid-template-columns: repeat(auto-fill, minmax(190px, 1fr));
  gap: var(--s3);
}
.cell {
  border: 1px solid var(--line);
  border-radius: var(--r);
  overflow: hidden;
  background: var(--surface);
}
/* ⚠️ **这里的 modifier 要和 SHOT_STATUS 那张表里的 tone 对齐**：
   neutral / info / warn / ok，一共就这四个，没有 bad——`shotTone` 只会
   返回它们（未知状态回落 neutral）。原来还有一条 `.cell--bad`，拼不出来，
   谁也命中不了。info 那几档（配音完成、首帧完成、草稿完成）故意不描边：
   它们是"还在路上"，边框留给终态（ok）和出了问题（warn）。 */
.cell--ok   { border-color: color-mix(in srgb, var(--ok) 35%, transparent); }
.cell--warn { border-color: color-mix(in srgb, var(--warn) 40%, transparent); }
.cell--live { border-color: var(--accent); }
.cell--open { outline: 2px solid var(--accent); }
.cell--drag { opacity: 0.4; }
.cell--over { outline: 2px dashed var(--accent); }

.cell__frame {
  position: relative;
  display: block;
  width: 100%;
  /* 跟项目的画幅走。横屏项目写死 9/16 的话每格都是上下两条黑。 */
  aspect-ratio: var(--cell-ratio, 9 / 16);
  background: var(--bg-sunken);
  color: var(--text-3);
}
.cell__frame img,
.cell__video {
  width: 100%;
  height: 100%;
  display: block;
}
.cell__frame img { object-fit: cover; }
/* **播放器用 contain 不是 cover。** cover 会把横屏片子裁掉两边，
   等于让用户看一个和成片不一样的画幅。留黑边才是这一镜真正的样子。 */
.cell__video {
  object-fit: contain;
  background: #000;
}
.cell__blank {
  position: absolute;
  inset: 0;
  margin: auto;
}
/* 采样中途的预览盖在画面上。88×160 放大到格子大小，浏览器默认的
   平滑缩放正好给出"糊"的样子，别开 pixelated。 */
.cell__preview {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  object-fit: cover;
  z-index: 1;
  pointer-events: none;
}
.cell__check {
  position: absolute;
  top: 6px;
  left: 6px;
  z-index: 2;
}

.cell__bottom {
  position: relative;   /* 进度底色是绝对定位的 */
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 6px;
  border-top: 1px solid var(--line);
  overflow: hidden;     /* 走马灯靠 translateX 走出去，不裁会画到格子外面 */
}
.cell__bottom > :not(.cell__fill) {
  position: relative;
  z-index: 1;
}
.cell__fill {
  position: absolute;
  left: 0;
  top: 0;
  bottom: 0;
  z-index: 0;
  background: color-mix(in srgb, var(--accent) 30%, transparent);
  transition: width 0.3s;
}
/* 不知道跑到哪一步时的走马灯。**别停着不动**——静止的进度条和"卡死了"
   看起来一模一样，而这一步（搬权重、VAE 解码）本来就要几十秒。 */
.cell__fill--idle {
  width: 40%;
  animation: cell-slide 1.6s ease-in-out infinite;
}
@keyframes cell-slide {
  0% { transform: translateX(-100%); }
  100% { transform: translateX(250%); }
}
@media (prefers-reduced-motion: reduce) {
  .cell__fill--idle { animation: none; width: 100%; opacity: 0.6; }
}

.cell__no,
.cell__state {
  border: none;
  background: none;
  padding: 0;
  cursor: pointer;
  color: inherit;
  text-align: left;
  font: inherit;
}
.cell__no { font-weight: 600; }
.cell__state { color: var(--text-2); min-width: 0; }
.cell__notes {
  display: flex;
  align-items: center;
  gap: 4px;
  padding: 3px 6px;
  color: var(--warn);
  border-top: 1px solid var(--line);
}

/* ---- 抽屉 ---- */
.drawer {
  position: fixed;
  inset: 0;
  z-index: 40;
  background: color-mix(in srgb, black 45%, transparent);
  display: flex;
  justify-content: flex-end;
}
.drawer__panel {
  display: flex;
  flex-direction: column;
  width: min(92vw, 460px);
  height: 100%;
  background: var(--surface);
  border-left: 1px solid var(--line);
}
.drawer__head,
.drawer__foot {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3);
  flex-wrap: wrap;
}
.drawer__head { border-bottom: 1px solid var(--line); }
.drawer__foot { border-top: 1px solid var(--line); }
.drawer__body {
  flex: 1;
  overflow-y: auto;
  padding: var(--s3);
}
.dialogue {
  display: flex;
  align-items: center;
  gap: var(--s2);
}
.dialogue__who { width: 5.5em; }
</style>
