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
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import StepHeader from '@/components/StepHeader.vue'
import { api, mediaUrl } from '@/api'
import {
  CAMERA_ANGLES,
  CAMERA_MOVES,
  SHOT_SIZES,
  TRANSITIONS,
  sizeLabel,
  statusOf,
} from '@/api/labels'
import { humanTime, useAction } from '@/composables/useAction'
import { STEPS, useShots } from '@/composables/useShots'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy, error } = useAction()

// 镜头表、每镜进度、重出队列，全在这儿。见 useShots。
const {
  shots, loading, load, bust,
  pct, shotState, busy, shotRunning,
  start, stop, shotAction, stepBtn, running,
} = useShots()

const openId = ref('')
const draft = ref(null)
const selected = ref(new Set())
const filter = ref('all')
const doctor = ref(null)
const video = ref(null)

const blocked = computed(() => doctor.value && doctor.value.can_run === false)
const openShot = computed(
  () => shots.value.find((s) => s.shot_id === openId.value) ?? null,
)

// ---- 概览 ----

const totalDuration = computed(() =>
  shots.value.reduce((a, s) => a + (s.duration_s || 0), 0),
)
const lipsyncCount = computed(() => shots.value.filter((s) => s.needs_lipsync).length)
const problemCount = computed(
  () => shots.value.filter((s) => s.gate_notes?.length).length,
)
/** 还没出片的镜头数。0 就是这一集做完了。 */
const pending = computed(() => shots.value.filter((s) => !s.video_path).length)

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

/** 副标题里直说这一集会出多大的画面——按下去之前该知道。 */
const tagline = computed(() => {
  const v = video.value
  const size = v
    ? `${v.orientation === 'landscape' ? '横屏' : '竖屏'} ${
        v.quality === '2k' ? '2K' : '720p'
      } · ${v.width}×${v.height}`
    : ''
  if (!shots.value.length) return size || '把这一集拆成一个个镜头，然后拍出来'
  return `${shots.value.length} 镜 · ${humanTime(totalDuration.value)}${size ? ' · ' + size : ''}`
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
  { key: 'lipsync', label: '要口型' },
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
  if (filter.value === 'lipsync') return shots.value.filter((s) => s.needs_lipsync)
  return shots.value
})

// ---- 抽屉 ----

const draftDirty = computed(() => {
  if (!draft.value) return false
  const was = shots.value.find((s) => s.shot_id === draft.value.shot_id)
  if (!was) return false
  const keys = [
    'visual_desc', 'first_frame_prompt', 'motion_prompt', 'negative_prompt',
    'subtitle_text', 'beat', 'shot_size', 'camera_angle', 'camera_move',
    'transition_in', 'transition_dur_s', 'duration_s', 'needs_lipsync',
  ]
  if (keys.some((k) => draft.value[k] !== was[k])) return true
  const nowLines = draft.value.dialogue_texts ?? []
  const wasLines = (was.dialogue ?? []).map((d) => d.text)
  return JSON.stringify(nowLines) !== JSON.stringify(wasLines)
})

function openDraft(shot) {
  openId.value = shot.shot_id
  draft.value = {
    ...shot,
    dialogue_texts: (shot.dialogue ?? []).map((d) => d.text),
  }
}

/** 点一格。开着同一镜就收起，否则换过去。 */
function toggle(shot) {
  // 换镜头之前先问一句。改了提示词直接切走，改动就无声无息没了，
  // 而用户以为「点回来还在那儿」。
  if (draftDirty.value && !confirm('这一镜有改动还没保存，切走就没了。确定？')) {
    return
  }
  if (openId.value === shot.shot_id) {
    close()
    return
  }
  openDraft(shot)
}

function close() {
  if (draftDirty.value && !confirm('这一镜有改动还没保存，关掉就没了。确定？')) {
    return
  }
  openId.value = ''
  draft.value = null
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
  if (draftDirty.value) return   // 有改动就不跳，免得静默丢掉
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
  if (shots.value.length && !confirm('重出分镜会覆盖整张表，手改过的镜头会丢。继续？')) {
    return
  }
  const scriptData = await run(
    () => api.getScript(session.projectPath, session.episodeId),
    { key: 'plan', quiet: true },
  )
  if (!scriptData?.script?.trim()) {
    ui.warn('这一集还没有剧本，先回第二步写')
    return
  }
  const result = await run(
    () =>
      api.plan({
        project: session.projectPath,
        script: scriptData.script,
        episode_id: session.episodeId,
        duration_s: scriptData.target_duration_s || 60,
      }),
    { key: 'plan', refresh: true },
  )
  if (result) {
    ui.ok(`出了 ${result.shots} 个镜头，共 ${humanTime(result.duration_s)}`)
    await load()
  }
}

async function planAll() {
  const result = await run(
    () => api.planAll({ project: session.projectPath, overwrite: false }),
    { key: 'planAll' },
  )
  if (result) ui.info(`正在给 ${result.episodes.join('、')} 补分镜，去第二步能看进度`)
}

async function saveShot() {
  if (!draft.value) return
  const patch = {}
  for (const k of [
    'visual_desc', 'first_frame_prompt', 'motion_prompt', 'negative_prompt',
    'subtitle_text', 'beat', 'shot_size', 'camera_angle', 'camera_move',
    'transition_in', 'transition_dur_s', 'duration_s', 'needs_lipsync',
  ]) {
    patch[k] = draft.value[k]
  }
  patch.dialogue_texts = draft.value.dialogue_texts
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
  ui.ok(result.reset ? '已保存，这一镜退回重跑' : '已保存')
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
  ui.ok(`${selected.value.size} 个镜头已${labels[action]}`)
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
  if (!pending.value &&
      !confirm('这一集已经全部出完了。重出会把每一镜从头再跑一遍，确定？')) {
    return
  }
  const r = await start([], null, !pending.value)
  if (r.ok) return
  // 409 = 已经在跑了（多半是另一个浏览器、或者另一个标签页点的）。
  // **那不是错误**，跟着看进度就行——轮询和 WebSocket 进页面就开着了。
  if (r.error?.status === 409) ui.info('已经在跑了，下面跟着看进度就行')
  else ui.error('起不来：' + (r.error?.message ?? '未知原因'))
}

/** 选中的那几镜一起重出某一段。挑十几个要重做的，一次交出去。 */
async function batchRerun(step) {
  const ids = [...selected.value]
  if (!ids.length) return
  selected.value = new Set()
  const r = await start(ids, [step.id])
  if (!r.ok) ui.error('起不来：' + (r.error?.message ?? '未知原因'))
}

// ---- 体检和画幅 ----

async function loadDoctor() {
  try {
    doctor.value = await api.doctor()
  } catch (err) {
    doctor.value = {
      can_run: false,
      checks: [{ name: '体检', level: 'error', detail: err.message }],
    }
  }
}

async function loadVideo() {
  if (!session.projectPath) return
  try {
    video.value = await api.projectVideo(session.projectPath)
  } catch {
    video.value = null
  }
}

watch(() => session.projectPath, loadVideo, { immediate: true })

onMounted(() => {
  loadDoctor()
  window.addEventListener('keydown', onKey)
})
onUnmounted(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader title="镜头" :tagline="tagline">
      <template #actions>
        <button
          v-if="running"
          class="btn btn--ghost"
          type="button"
          @click="stop"
        >
          <AppIcon name="pause" :size="15" />
          停下
        </button>
        <template v-else>
          <button
            class="btn btn--ghost"
            type="button"
            :disabled="!session.hasProject"
            @click="planAll"
          >
            批量补分镜
          </button>
          <button
            class="btn btn--ai"
            type="button"
            :disabled="!session.episodeId || isBusy('plan')"
            @click="generate"
          >
            <AppIcon name="sparkle" :size="15" />
            {{ isBusy('plan') ? '正在拆镜头…' : shots.length ? 'AI 重出分镜' : 'AI 从剧本出分镜' }}
          </button>
          <button
            v-if="shots.length"
            class="btn btn--primary"
            type="button"
            :disabled="blocked || isBusy('start')"
            @click="startAll"
          >
            <AppIcon name="film" :size="15" />
            {{ pending ? `出片（还差 ${pending} 镜）` : '全部重出' }}
          </button>
        </template>
      </template>
      <template v-if="session.hasProject && missingAssets.length" #note>
        <p class="alert alert--warn">
          <AppIcon name="warn" :size="15" />
          <span>
            还没有{{ missingAssets.join('和') }}。分镜表里只能填已注册的 id，
            库是空的话大模型编不出来，这一步会直接报「引用了未注册的资产」。
          </span>
          <RouterLink
            :to="missingAssets[0] === '角色' ? '/characters' : '/scenes'"
            class="btn btn--sm"
          >
            先去出{{ missingAssets[0] }}
          </RouterLink>
        </p>
      </template>
    </StepHeader>

    <!-- **只在拦路时出现。** 全绿的时候一行都不占——
         体检的细节在设置页，这里只管"能不能开工"。 -->
    <section v-if="blocked && shots.length" class="card card--bad">
      <div class="card__head">
        <div>
          <div class="card__title">还不能开工</div>
          <div class="card__sub">下面这些先解决，否则跑起来也是白跑。</div>
        </div>
        <button class="btn btn--ghost btn--sm" type="button" @click="loadDoctor">
          <AppIcon name="refresh" :size="14" />
          重新体检
        </button>
      </div>
      <div class="card__body stack stack--sm">
        <p
          v-for="c in doctor.checks.filter((x) => x.level === 'fail' || x.level === 'error')"
          :key="c.name"
          class="alert alert--bad"
        >
          <strong>{{ c.name }}</strong>
          <span>{{ c.detail }}</span>
          <span v-if="c.fix" class="tiny dim">{{ c.fix }}</span>
        </p>
      </div>
    </section>

    <EmptyState
      v-if="!session.episodeId"
      icon="script"
      tone="warn"
      title="还没选到某一集"
      hint="镜头是针对某一集的。先在上面挑一集，没有的话回第二步写一集。"
    >
      <RouterLink to="/script" class="btn btn--primary">去写剧本</RouterLink>
    </EmptyState>

    <EmptyState
      v-else-if="!loading && !shots.length"
      icon="board"
      title="这一集还没有分镜"
      hint="分镜表决定后面每一个镜头怎么拍。让大模型从剧本拆一版出来，再手工调。"
    >
      <button class="btn btn--ai" type="button" :disabled="isBusy('plan')" @click="generate">
        <AppIcon name="sparkle" :size="15" />
        AI 从剧本出分镜
      </button>
    </EmptyState>

    <template v-else>
      <!-- 时间轴：一集里哪几镜特别长、哪一段全是特写，扫一眼就知道，
           而在一面等大的墙上是看不出来的。 -->
      <section class="card">
        <div class="card__body stack stack--sm">
          <div class="timeline">
            <button
              v-for="s in shots"
              :key="s.shot_id"
              class="tl"
              :class="[`tl--${statusOf(s.status).tone}`, { 'tl--on': openId === s.shot_id }]"
              type="button"
              :style="{ width: Math.max(2, ((s.duration_s || 0) / (totalDuration || 1)) * 100) + '%' }"
              :title="`${s.order + 1}. ${sizeLabel(s.shot_size)} ${s.duration_s}s`"
              @click="toggle(s)"
            >
              <span class="tl__n">{{ s.order + 1 }}</span>
            </button>
          </div>
          <div class="row row--wrap tiny dim">
            <span><i class="swatch swatch--neutral" /> 未开工</span>
            <span><i class="swatch swatch--info" /> 进行中</span>
            <span><i class="swatch swatch--warn" /> 未过闸 / 降级</span>
            <span><i class="swatch swatch--ok" /> 已完成</span>
            <span class="spacer" />
            <span v-if="lipsyncCount">{{ lipsyncCount }} 镜要口型</span>
            <span v-if="problemCount" class="warn-text">{{ problemCount }} 镜有闸门备注</span>
          </div>
        </div>
      </section>

      <!-- 筛选与批量 -->
      <div class="toolbar">
        <div class="chips">
          <button
            v-for="f in FILTERS"
            :key="f.key"
            class="chip"
            :class="{ 'chip--on': filter === f.key }"
            type="button"
            @click="filter = f.key"
          >
            {{ f.label }}
          </button>
        </div>
        <span v-if="filter !== 'all' && shots.length > 1" class="tiny dim nowrap">
          筛选状态下不能调顺序，切回「全部」
        </span>
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
            :disabled="isBusy('start')"
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
            `cell--${statusOf(s.status).tone}`,
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
              :key="s.shot_id + ':' + bust"
              class="cell__video"
              :src="mediaUrl(session.projectPath, s.video_path) + '&_=' + bust"
              :poster="s.frame_path
                ? mediaUrl(session.projectPath, s.frame_path) + '&_=' + bust
                : undefined"
              controls
              playsinline
              preload="none"
            />
            <img
              v-else-if="s.frame_path"
              :src="mediaUrl(session.projectPath, s.frame_path) + '&_=' + bust"
              :alt="s.visual_desc"
              loading="lazy"
            />
            <AppIcon v-else name="image" :size="18" class="cell__blank" />

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
            <button class="cell__state tiny truncate" type="button" title="改这一镜" @click="toggle(s)">
              {{ shotState(s) }}
            </button>
            <span class="spacer" />
            <button
              v-if="shotRunning(s.shot_id)"
              class="iconbtn"
              type="button"
              title="停下这一轮（跑完的镜头留着）"
              @click="shotAction(s, 'final')"
            >
              <AppIcon name="pause" :size="14" />
            </button>
            <template v-else>
              <button
                v-for="step in STEPS"
                :key="step.id"
                class="iconbtn"
                type="button"
                :title="stepBtn(s, step).title"
                @click="shotAction(s, step.id)"
              >
                <AppIcon :name="stepBtn(s, step).icon" :size="14" />
              </button>
            </template>
          </div>

          <p v-if="s.gate_notes?.length" class="cell__notes tiny">
            <AppIcon name="warn" :size="12" />
            <span class="truncate">{{ s.gate_notes.join('；') }}</span>
          </p>
        </article>
      </div>
    </template>

    <!-- 详情抽屉。点墙上任意一格滑出来。 -->
    <div v-if="draft && openShot" class="drawer" @click.self="close">
      <aside class="drawer__panel">
        <header class="drawer__head">
          <b class="numeric">{{ openShot.order + 1 }}</b>
          <span class="pill nowrap" :class="`pill--${statusOf(openShot.status).tone}`">
            {{ shotState(openShot) }}
          </span>
          <span v-if="openShot.needs_lipsync" class="pill pill--info nowrap tiny">口型</span>
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
          <button class="iconbtn" type="button" title="收起" @click="close">
            <AppIcon name="close" :size="15" />
          </button>
        </header>

        <div class="drawer__body stack stack--sm">
          <p v-if="openShot.gate_notes?.length" class="alert alert--warn">
            <AppIcon name="warn" :size="14" />
            <span>{{ openShot.gate_notes.join('；') }}</span>
          </p>

          <div class="row row--wrap tiny dim">
            <span v-if="locationOf(openShot)" class="tag tag--loc">
              {{ locName(locationOf(openShot)) }}
            </span>
            <span v-for="cid in openShot.char_ids ?? []" :key="cid" class="tag tag--char">
              {{ charName(cid) }}
            </span>
            <span v-if="openShot.beat" class="pill pill--neutral tiny">{{ openShot.beat }}</span>
            <span class="spacer" />
            <span class="mono">{{ openShot.shot_id }}</span>
          </div>

          <p class="group">画面</p>
          <label class="field">
            <span class="field__label">画面描述（给人看）</span>
            <textarea v-model="draft.visual_desc" class="textarea textarea--tight" rows="2" />
          </label>
          <label class="field">
            <span class="field__label">首帧提示词</span>
            <textarea v-model="draft.first_frame_prompt" class="textarea textarea--tight mono" rows="4" />
            <span class="field__hint">
              角色和场景的外观由程序拼进去，这里只写本镜特有的部分。
            </span>
          </label>
          <label class="field">
            <span class="field__label">运动提示词</span>
            <textarea v-model="draft.motion_prompt" class="textarea textarea--tight mono" rows="2" />
          </label>
          <label class="field">
            <span class="field__label">负向提示词</span>
            <textarea v-model="draft.negative_prompt" class="textarea textarea--tight mono" rows="2" />
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
              <input
                v-model.number="draft.duration_s"
                class="input numeric"
                type="number"
                step="0.5"
                min="0.5"
                max="30"
                :disabled="draft.duration_locked"
              />
            </label>
            <label class="field">
              <span class="field__label">入场转场</span>
              <select v-model="draft.transition_in" class="select">
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
              />
              <span v-if="draft.transition_in === 'cut'" class="field__hint">硬切必须是 0。</span>
            </label>
          </div>

          <p class="group">声音与字幕</p>
          <div class="field">
            <span class="field__label">台词</span>
            <div v-if="draft.dialogue_texts?.length" class="stack stack--sm">
              <div v-for="(line, i) in draft.dialogue_texts" :key="i" class="dialogue">
                <span class="dialogue__who tiny dim nowrap">
                  {{ draft.dialogue[i]?.char_id || '旁白' }}
                </span>
                <input v-model="draft.dialogue_texts[i]" class="input" />
                <span v-if="draft.dialogue[i]?.duration_s" class="tiny dim numeric nowrap">
                  {{ draft.dialogue[i].duration_s.toFixed(1) }}s
                </span>
              </div>
            </div>
            <p v-else class="field__hint">这一镜没有台词。</p>
          </div>
          <label class="field">
            <span class="field__label">字幕</span>
            <input v-model="draft.subtitle_text" class="input" />
          </label>
          <div class="row">
            <label class="switch">
              <input v-model="draft.needs_lipsync" type="checkbox" />
              <span>做口型</span>
            </label>
            <span class="tiny dim">规则按景别、机位和面朝方向自动推的，一般不用改。</span>
          </div>
        </div>

        <footer class="drawer__foot">
          <button
            class="btn btn--primary"
            type="button"
            :disabled="!draftDirty || isBusy('save')"
            @click="saveShot"
          >
            {{ isBusy('save') ? '保存中…' : '保存这一镜' }}
          </button>
          <!-- 改完提示词紧接着就想重出，别让人再去墙上找那一格 -->
          <button
            v-for="step in STEPS"
            :key="step.id"
            class="btn btn--ghost"
            type="button"
            :title="stepBtn(openShot, step).title"
            @click="shotAction(openShot, step.id)"
          >
            <AppIcon :name="stepBtn(openShot, step).icon" :size="14" />
            重出{{ step.label }}
          </button>
          <span class="spacer" />
          <span class="tiny dim nowrap kbd-hint">
            <kbd>↑</kbd><kbd>↓</kbd> 换镜头 <kbd>Esc</kbd> 收起
          </span>
        </footer>
      </aside>
    </div>
  </div>
</template>

<style scoped>
/* ---- 时间轴 ---- */
.timeline {
  display: flex;
  gap: 2px;
  height: 26px;
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
.swatch {
  display: inline-block;
  width: 9px;
  height: 9px;
  border-radius: 2px;
  background: var(--bg-sunken);
  margin-right: 3px;
}
.swatch--info { background: color-mix(in srgb, var(--info) 45%, transparent); }
.swatch--warn { background: color-mix(in srgb, var(--warn) 45%, transparent); }
.swatch--ok   { background: color-mix(in srgb, var(--ok) 45%, transparent); }
.warn-text { color: var(--warn); }

/* ---- 工具条 ---- */
.toolbar {
  display: flex;
  align-items: center;
  gap: var(--s2);
  flex-wrap: wrap;
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
.cell--ok   { border-color: color-mix(in srgb, var(--ok) 35%, transparent); }
.cell--warn { border-color: color-mix(in srgb, var(--warn) 40%, transparent); }
.cell--bad  { border-color: color-mix(in srgb, var(--danger) 45%, transparent); }
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
.kbd-hint kbd {
  border: 1px solid var(--line);
  border-radius: 3px;
  padding: 0 3px;
  margin-right: 2px;
}
</style>
