<script setup>
/**
 * 第五步：分镜。
 *
 * 分镜表是整套系统的中枢，这一页也是整个界面里信息密度最高的地方。
 * 三层结构：上面一条时间轴看整集节奏，中间是镜头卡片，点开是编辑器。
 * 时间轴不是装饰——一集里哪几镜特别长、哪一段全是特写，扫一眼就知道，
 * 而在一张表格里得逐行读数字。
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
  angleLabel,
  moveLabel,
  sizeLabel,
  statusOf,
} from '@/api/labels'
import { humanTime, useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy, error } = useAction()

const shots = ref([])
const loading = ref(false)
const openId = ref('')
const draft = ref(null)
const selected = ref(new Set())
const filter = ref('all')

const totalDuration = computed(() =>
  shots.value.reduce((a, s) => a + (s.duration_s || 0), 0),
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
const lipsyncCount = computed(() => shots.value.filter((s) => s.needs_lipsync).length)
const problemCount = computed(
  () => shots.value.filter((s) => s.gate_notes?.length).length,
)

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

async function load() {
  if (!session.projectPath || !session.episodeId) {
    shots.value = []
    return
  }
  loading.value = true
  try {
    const data = await api.shots(session.projectPath, session.episodeId)
    shots.value = data.shots ?? []
  } catch (err) {
    // 还没出分镜时引擎会 404。这不是错，是流程还没走到。
    if (err.status !== 404) ui.error(err.message)
    shots.value = []
  } finally {
    loading.value = false
  }
}

watch(() => [session.projectPath, session.episodeId], load, { immediate: true })

async function toggle(shot) {
  // 收起或换一镜之前先问一句。改了提示词直接收起来，改动就无声无息没了，
  // 而用户以为「点开还在那儿」。
  if (draftDirty.value && !confirm('这一镜有改动还没保存，收起就没了。确定？')) {
    return
  }
  if (openId.value === shot.shot_id) {
    openId.value = ''
    draft.value = null
    return
  }
  openId.value = shot.shot_id
  draft.value = {
    ...shot,
    dialogue_texts: (shot.dialogue ?? []).map((d) => d.text),
  }
  // 编辑器很高。点列表靠下的镜头时内容全在屏幕外面，像「点了没反应」。
  await nextTick()
  document
    .querySelector(`[data-shot="${shot.shot_id}"]`)
    ?.scrollIntoView({ behavior: 'smooth', block: 'start' })
}

const draftDirty = computed(() => {
  if (!draft.value) return false
  const was = shots.value.find((s) => s.shot_id === draft.value.shot_id)
  if (!was) return false
  const keys = [
    'visual_desc',
    'first_frame_prompt',
    'motion_prompt',
    'negative_prompt',
    'subtitle_text',
    'beat',
    'shot_size',
    'camera_angle',
    'camera_move',
    'transition_in',
    'transition_dur_s',
    'duration_s',
    'needs_lipsync',
  ]
  if (keys.some((k) => draft.value[k] !== was[k])) return true
  const nowLines = draft.value.dialogue_texts ?? []
  const wasLines = (was.dialogue ?? []).map((d) => d.text)
  return JSON.stringify(nowLines) !== JSON.stringify(wasLines)
})

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
    'visual_desc',
    'first_frame_prompt',
    'motion_prompt',
    'negative_prompt',
    'subtitle_text',
    'beat',
    'shot_size',
    'camera_angle',
    'camera_move',
    'transition_in',
    'transition_dur_s',
    'duration_s',
    'needs_lipsync',
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
  // 重新读一遍之后编辑器要跟着新数据走，否则「未保存」的提示会一直挂着
  const fresh = shots.value.find((s) => s.shot_id === openId.value)
  draft.value = fresh
    ? { ...fresh, dialogue_texts: (fresh.dialogue ?? []).map((d) => d.text) }
    : null
}

/**
 * 调镜头顺序。
 *
 * 只在「全部」筛选下开放：筛过之后列表里少了几镜，往上挪一格到底是
 * 挪到相邻那一镜前面、还是挪到被筛掉的那一镜前面，说不清楚。
 * 说不清楚的操作不如不给。
 */
const canReorder = computed(() => filter.value === 'all' && shots.value.length > 1)

async function move(shot, delta) {
  const order = shots.value.map((s) => s.shot_id)
  const at = order.indexOf(shot.shot_id)
  const to = at + delta
  if (at < 0 || to < 0 || to >= order.length) return
  order.splice(to, 0, ...order.splice(at, 1))
  await applyOrder(order)
}

async function applyOrder(order) {
  // 先在本地摆好。等一个来回再动的话，连点两下上移会按旧顺序算第二下。
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

/** 拖拽排序。触屏没有原生拖放，所以上下按钮才是主要手段，这个是桌面上的顺手。 */
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

function toggleSelect(shotId) {
  const next = new Set(selected.value)
  if (next.has(shotId)) next.delete(shotId)
  else next.add(shotId)
  selected.value = next
}

function selectAllShown() {
  selected.value =
    selected.value.size === shown.value.length
      ? new Set()
      : new Set(shown.value.map((s) => s.shot_id))
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

/** 时间轴上每一镜的宽度按时长占比。太窄的给个下限，否则点不着。 */
function widthOf(shot) {
  if (!totalDuration.value) return 0
  return Math.max(1.2, (shot.duration_s / totalDuration.value) * 100)
}

/**
 * 卡片上显示什么。
 *
 * 画面描述是给人看的那一栏，但模型经常留空。留空就退到字幕、台词、
 * 提示词，总有一样能让人认出这是哪一镜。整列显示「（没写画面描述）」
 * 的话，这张表就等于没法用眼睛扫。
 */
function titleOf(shot) {
  const first = shot.dialogue?.[0]?.text
  const text =
    shot.visual_desc?.trim() ||
    shot.subtitle_text?.trim() ||
    (first ? `「${first}」` : '') ||
    shot.first_frame_prompt?.trim() ||
    ''
  if (!text) return '（这一镜还没有任何描述）'
  return text.length > 60 ? text.slice(0, 60) + '…' : text
}

/**
 * 上下键在镜头之间走。
 *
 * 一集几十镜，逐个审的时候手要一直在鼠标和键盘之间来回换。
 * 焦点在输入框里时不接管，否则在提示词里按方向键会跳到下一镜。
 */
function onKey(event) {
  if (!openId.value) return
  const tag = document.activeElement?.tagName
  if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') {
    if (event.key === 'Escape') document.activeElement.blur()
    return
  }
  if (event.key === 'Escape') {
    openId.value = ''
    draft.value = null
    return
  }
  const step = event.key === 'ArrowDown' ? 1 : event.key === 'ArrowUp' ? -1 : 0
  if (!step) return
  event.preventDefault()
  const list = shown.value
  const at = list.findIndex((s) => s.shot_id === openId.value)
  const next = list[at + step]
  if (!next) return
  openId.value = ''
  // toggle 自己会把展开的那一镜滚进视野
  toggle(next)
}

onMounted(() => window.addEventListener('keydown', onKey))
onUnmounted(() => window.removeEventListener('keydown', onKey))
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader>
      <template #actions>
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

    <EmptyState
      v-if="!session.episodeId"
      icon="script"
      tone="warn"
      title="还没选到某一集"
      hint="分镜是针对某一集的。先在上面挑一集，没有的话回第二步写一集。"
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
        AI 出分镜
      </button>
    </EmptyState>

    <template v-else-if="shots.length">
      <!-- 概览 -->
      <section class="card">
        <div class="card__body stack">
          <div class="metrics">
            <div class="metric">
              <span class="metric__n numeric">{{ shots.length }}</span>
              <span class="metric__l">镜头</span>
            </div>
            <div class="metric">
              <span class="metric__n numeric">{{ humanTime(totalDuration) }}</span>
              <span class="metric__l">总时长</span>
            </div>
            <div class="metric">
              <span class="metric__n numeric">{{ lipsyncCount }}</span>
              <span class="metric__l">要做口型</span>
            </div>
            <div class="metric" :class="{ 'metric--bad': problemCount }">
              <span class="metric__n numeric">{{ problemCount }}</span>
              <span class="metric__l">有闸门备注</span>
            </div>
          </div>

          <!-- 时间轴 -->
          <div class="timeline">
            <button
              v-for="s in shots"
              :key="s.shot_id"
              class="tl"
              :class="[`tl--${statusOf(s.status).tone}`, { 'tl--on': openId === s.shot_id }]"
              type="button"
              :style="{ width: widthOf(s) + '%' }"
              :title="`${s.order + 1}. ${sizeLabel(s.shot_size)} ${s.duration_s}s ${titleOf(s)}`"
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
        <span v-if="openId" class="tiny dim nowrap kbd-hint">
          <kbd>↑</kbd><kbd>↓</kbd> 换镜头 <kbd>Esc</kbd> 收起
        </span>
        <button class="btn btn--ghost btn--sm" type="button" @click="selectAllShown">
          {{ selected.size === shown.length && shown.length ? '取消全选' : '全选' }}
        </button>
        <template v-if="selected.size">
          <span class="pill pill--accent">选中 {{ selected.size }}</span>
          <button class="btn btn--sm" type="button" @click="batch('reset')">退回重跑</button>
          <button class="btn btn--sm" type="button" @click="batch('lock')">锁定</button>
          <button class="btn btn--sm" type="button" @click="batch('unlock')">解锁</button>
          <button class="btn btn--sm btn--ghost" type="button" @click="batch('clear_notes')">
            清备注
          </button>
        </template>
      </div>

      <!-- 镜头卡片 -->
      <div class="stack stack--sm">
        <article
          v-for="s in shown"
          :key="s.shot_id"
          class="shot card"
          :class="{
            'shot--open': openId === s.shot_id,
            'shot--dragging': dragId === s.shot_id,
            'shot--dragover': dragOverId === s.shot_id,
          }"
          :data-shot="s.shot_id"
          :draggable="canReorder"
          @dragstart="onDragStart(s, $event)"
          @dragover="onDragOver(s, $event)"
          @dragleave="dragOverId = ''"
          @drop.prevent="onDrop(s)"
          @dragend="((dragId = ''), (dragOverId = ''))"
        >
          <div class="shot__row">
            <input
              class="shot__check"
              type="checkbox"
              :checked="selected.has(s.shot_id)"
              :aria-label="`选中镜头 ${s.order + 1}`"
              @change="toggleSelect(s.shot_id)"
            />

            <span v-if="canReorder" class="shot__move">
              <button
                class="shot__movebtn"
                type="button"
                :disabled="s.order === 0 || isBusy('reorder')"
                :aria-label="`把第 ${s.order + 1} 镜往前挪`"
                title="往前挪一格"
                @click.stop="move(s, -1)"
              >
                ▲
              </button>
              <button
                class="shot__movebtn"
                type="button"
                :disabled="s.order === shots.length - 1 || isBusy('reorder')"
                :aria-label="`把第 ${s.order + 1} 镜往后挪`"
                title="往后挪一格"
                @click.stop="move(s, 1)"
              >
                ▼
              </button>
            </span>

            <button class="shot__main" type="button" @click="toggle(s)">
              <span class="shot__thumb">
                <img
                  v-if="s.frame_path"
                  :src="mediaUrl(session.projectPath, s.frame_path)"
                  :alt="`镜头 ${s.order + 1} 首帧`"
                  loading="lazy"
                />
                <span v-else class="shot__num numeric">{{ s.order + 1 }}</span>
                <span v-if="s.has_video" class="shot__play"><AppIcon name="play" :size="11" /></span>
              </span>

              <span class="shot__text">
                <span class="shot__title">
                  <b class="numeric">{{ s.order + 1 }}</b>
                  <span class="truncate">{{ titleOf(s) }}</span>
                </span>
                <span class="shot__tags tiny dim">
                  <span v-if="locationOf(s)" class="tag tag--loc">
                    {{ locName(locationOf(s)) }}
                  </span>
                  <span
                    v-for="cid in s.char_ids ?? []"
                    :key="cid"
                    class="tag tag--char"
                  >
                    {{ charName(cid) }}
                  </span>
                  <span class="t-size">{{ sizeLabel(s.shot_size) }}</span>
                  <span class="t-angle">{{ angleLabel(s.camera_angle) }}</span>
                  <span class="t-move">{{ moveLabel(s.camera_move) }}</span>
                  <span class="numeric">{{ s.duration_s }}s</span>
                  <span v-if="s.beat" class="pill pill--neutral tiny">{{ s.beat }}</span>
                </span>
              </span>

              <span class="spacer" />
              <span v-if="s.needs_lipsync" class="pill pill--info nowrap shot__flag">口型</span>
              <span v-if="s.duration_locked" class="pill pill--neutral nowrap shot__flag">
                时长已锁
              </span>
              <span
                class="pill nowrap shot__status"
                :class="`pill--${statusOf(s.status).tone}`"
              >
                {{ statusOf(s.status).label }}
              </span>
            </button>
          </div>

          <div v-if="s.gate_notes?.length" class="shot__notes">
            <AppIcon name="warn" :size="13" />
            <span>{{ s.gate_notes.join('；') }}</span>
          </div>

          <!-- 编辑器 -->
          <div v-if="openId === s.shot_id && draft" class="shot__edit">
            <div class="edit__cols">
              <div class="stack stack--sm">
                <p class="group">画面</p>
                <label class="field">
                  <span class="field__label">画面描述（给人看）</span>
                  <textarea v-model="draft.visual_desc" class="textarea textarea--tight" rows="2" />
                </label>
                <label class="field">
                  <span class="field__label">首帧提示词</span>
                  <textarea
                    v-model="draft.first_frame_prompt"
                    class="textarea textarea--tight mono"
                    rows="4"
                  />
                  <span class="field__hint">
                    角色和场景的外观由程序拼进去，这里只写本镜特有的部分。
                  </span>
                </label>
                <label class="field">
                  <span class="field__label">运动提示词</span>
                  <textarea
                    v-model="draft.motion_prompt"
                    class="textarea textarea--tight mono"
                    rows="2"
                  />
                </label>
                <label class="field">
                  <span class="field__label">负向提示词</span>
                  <textarea
                    v-model="draft.negative_prompt"
                    class="textarea textarea--tight mono"
                    rows="2"
                  />
                </label>
              </div>

              <div class="stack stack--sm">
                <p class="group">镜头语言</p>
                <div class="grid grid--pairs">
                  <label class="field">
                    <span class="field__label">景别</span>
                    <select v-model="draft.shot_size" class="select">
                      <option v-for="o in SHOT_SIZES" :key="o.value" :value="o.value">
                        {{ o.label }}
                      </option>
                    </select>
                  </label>
                  <label class="field">
                    <span class="field__label">机位</span>
                    <select v-model="draft.camera_angle" class="select">
                      <option v-for="o in CAMERA_ANGLES" :key="o.value" :value="o.value">
                        {{ o.label }}
                      </option>
                    </select>
                  </label>
                  <label class="field">
                    <span class="field__label">运镜</span>
                    <select v-model="draft.camera_move" class="select">
                      <option v-for="o in CAMERA_MOVES" :key="o.value" :value="o.value">
                        {{ o.label }}
                      </option>
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
                      <option v-for="o in TRANSITIONS" :key="o.value" :value="o.value">
                        {{ o.label }}
                      </option>
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
                    <span v-if="draft.transition_in === 'cut'" class="field__hint">
                      硬切必须是 0。
                    </span>
                  </label>
                </div>

                <p class="group">声音与字幕</p>

                <div class="field">
                  <span class="field__label">台词</span>
                  <div v-if="draft.dialogue_texts?.length" class="stack stack--sm">
                    <div
                      v-for="(line, i) in draft.dialogue_texts"
                      :key="i"
                      class="dialogue"
                    >
                      <span class="dialogue__who tiny dim nowrap">
                        {{ draft.dialogue[i]?.char_id || '旁白' }}
                      </span>
                      <input v-model="draft.dialogue_texts[i]" class="input" />
                      <span
                        v-if="draft.dialogue[i]?.duration_s"
                        class="tiny dim numeric nowrap"
                      >
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

                <p v-if="s.frame_path || s.video_path" class="group">已经跑出来的</p>
                <div v-if="s.frame_path || s.video_path" class="preview">
                  <video
                    v-if="s.video_path"
                    class="preview__media"
                    :src="mediaUrl(session.projectPath, s.video_path)"
                    controls
                    preload="metadata"
                    :poster="s.frame_path ? mediaUrl(session.projectPath, s.frame_path) : undefined"
                  />
                  <img
                    v-else
                    class="preview__media"
                    :src="mediaUrl(session.projectPath, s.frame_path)"
                    alt="首帧"
                  />
                </div>
              </div>
            </div>

            <div class="edit__foot">
              <button
                class="btn btn--primary"
                type="button"
                :disabled="!draftDirty || isBusy('save')"
                @click="saveShot"
              >
                {{ isBusy('save') ? '保存中…' : '保存这一镜' }}
              </button>
              <button class="btn btn--ghost" type="button" @click="toggle(s)">收起</button>
              <span class="spacer" />
              <span class="tiny dim mono">{{ s.shot_id }} · {{ s.scene_id }}</span>
            </div>
          </div>
        </article>
      </div>
    </template>
  </div>
</template>

<style scoped>
.metrics {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(110px, 1fr));
  gap: var(--s3);
}
.metric {
  display: flex;
  flex-direction: column;
  padding: var(--s3);
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
}
.metric--bad {
  border-color: color-mix(in srgb, var(--warn) 40%, transparent);
  background: var(--warn-soft);
}
.metric__n {
  font-size: var(--fs-xl);
  font-weight: 700;
  line-height: 1.2;
}
.metric__l {
  font-size: var(--fs-xs);
  color: var(--text-3);
}

.timeline {
  display: flex;
  gap: 2px;
  height: 34px;
  padding: 3px;
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
  overflow: hidden;
}
.tl {
  min-width: 0;
  border: none;
  border-radius: 3px;
  cursor: pointer;
  background: var(--surface-3);
  color: var(--text-3);
  font-size: 9px;
  font-weight: 700;
  padding: 0;
  transition: filter 0.14s var(--ease), transform 0.1s var(--ease);
  display: grid;
  place-items: center;
  overflow: hidden;
}
.tl:hover {
  filter: brightness(1.35);
  transform: translateY(-1px);
}
.tl--info {
  background: color-mix(in srgb, var(--info) 55%, transparent);
  color: #04121f;
}
.tl--warn {
  background: color-mix(in srgb, var(--warn) 60%, transparent);
  color: #22190a;
}
.tl--ok {
  background: color-mix(in srgb, var(--ok) 60%, transparent);
  color: #062210;
}
.tl--on {
  outline: 2px solid var(--accent);
  outline-offset: -2px;
}
.tl__n {
  pointer-events: none;
}

.swatch {
  display: inline-block;
  width: 8px;
  height: 8px;
  border-radius: 2px;
  margin-right: 4px;
  vertical-align: -1px;
  background: var(--surface-3);
}
.swatch--info {
  background: color-mix(in srgb, var(--info) 55%, transparent);
}
.swatch--warn {
  background: color-mix(in srgb, var(--warn) 60%, transparent);
}
.swatch--ok {
  background: color-mix(in srgb, var(--ok) 60%, transparent);
}

.toolbar {
  display: flex;
  align-items: center;
  gap: var(--s2);
  flex-wrap: wrap;
  position: sticky;
  top: 0;
  z-index: 5;
  padding: var(--s2) 0;
  background: var(--bg);
}
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

.alert {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3) var(--s4);
  border-radius: var(--r);
  font-size: var(--fs-base);
  line-height: 1.6;
}
.alert--warn {
  background: var(--warn-soft);
  color: var(--warn);
  border: 1px solid color-mix(in srgb, var(--warn) 32%, transparent);
}
.alert--warn span {
  flex: 1;
}

.kbd-hint kbd {
  display: inline-block;
  min-width: 17px;
  padding: 0 4px;
  margin-right: 2px;
  border-radius: 4px;
  border: 1px solid var(--line-strong);
  border-bottom-width: 2px;
  background: var(--surface-2);
  font-family: var(--font);
  font-size: 10px;
  line-height: 15px;
  text-align: center;
  color: var(--text-2);
}

.shot--open {
  border-color: var(--accent-line);
}
.shot__row {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding-left: var(--s3);
}
.shot__check {
  width: 15px;
  height: 15px;
  accent-color: var(--accent);
  cursor: pointer;
  flex: none;
}

/* 上下挪一格。触屏上没有原生拖放，这两个按钮才是主要手段，
   拖拽只是桌面上的顺手。 */
.shot__move {
  display: flex;
  flex-direction: column;
  flex: none;
  gap: 1px;
}
.shot__movebtn {
  width: 18px;
  height: 15px;
  padding: 0;
  border: none;
  border-radius: 3px;
  background: transparent;
  color: var(--text-3);
  font-size: 8px;
  line-height: 1;
  cursor: pointer;
}
.shot__movebtn:hover:not(:disabled) {
  background: var(--surface-3);
  color: var(--accent);
}
.shot__movebtn:disabled {
  opacity: 0.25;
  cursor: default;
}

.shot--dragging {
  opacity: 0.4;
}
.shot--dragover {
  border-color: var(--accent);
  box-shadow: 0 -2px 0 var(--accent);
}
[draggable='true'] .shot__thumb {
  cursor: grab;
}

@media (pointer: coarse) {
  .shot__movebtn {
    width: 26px;
    height: 22px;
    font-size: 10px;
  }
}
.shot__main {
  flex: 1;
  min-width: 0;
  display: flex;
  align-items: center;
  gap: var(--s3);
  padding: var(--s2) var(--s4) var(--s2) var(--s2);
  background: none;
  border: none;
  cursor: pointer;
  text-align: left;
}
.shot__main:hover {
  background: var(--surface-2);
}

.shot__thumb {
  position: relative;
  flex: none;
  display: grid;
  place-items: center;
  width: 54px;
  height: 40px;
  border-radius: var(--r-sm);
  background: var(--surface-3);
  overflow: hidden;
  color: var(--text-3);
}
.shot__thumb img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.shot__num {
  font-weight: 700;
  font-size: var(--fs-md);
}
.shot__play {
  position: absolute;
  right: 2px;
  bottom: 2px;
  display: grid;
  place-items: center;
  width: 16px;
  height: 16px;
  border-radius: 50%;
  background: rgba(0, 0, 0, 0.65);
  color: #fff;
}

.shot__text {
  display: flex;
  flex-direction: column;
  min-width: 0;
  gap: 2px;
}
.shot__title {
  display: flex;
  align-items: baseline;
  gap: var(--s2);
  font-size: var(--fs-base);
  min-width: 0;
}
.shot__title b {
  color: var(--text-3);
  font-size: var(--fs-sm);
  flex: none;
}
.shot__tags {
  display: flex;
  gap: var(--s3);
  align-items: center;
}
/* 场景和角色单独给个底色。这两样是分镜表里唯一的引用，
   一列扫下去能看出「这一段全在同一个房间」「这几镜只有一个人」。 */
.tag {
  padding: 0 5px;
  border-radius: 3px;
  font-weight: 600;
}
.tag--loc {
  background: color-mix(in srgb, var(--info) 16%, transparent);
  color: var(--info);
}
.tag--char {
  background: color-mix(in srgb, var(--accent) 16%, transparent);
  color: var(--accent);
}

.shot__notes {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
  margin: 0 var(--s4) var(--s3) 46px;
  padding: var(--s2) var(--s3);
  border-radius: var(--r-sm);
  background: var(--warn-soft);
  color: var(--warn);
  font-size: var(--fs-sm);
  line-height: 1.5;
}

.shot__edit {
  border-top: 1px solid var(--line);
  padding: var(--s5);
  background: var(--surface-2);
}
.edit__cols {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  gap: var(--s5);
}
.grid--pairs {
  grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
  gap: var(--s3);
}
/* 保存条钉在视口底部。
 *
 * 展开的编辑器比屏幕高，保存按钮在最下面——改完上面那几个提示词，
 * 得先滚到底才找得到它。梗概那个框就是这么被人以为「存不上」的。
 * 粘在 .main__scroll 的底边，正好落在上一步/下一步那条之上。 */
.edit__foot {
  position: sticky;
  bottom: 0;
  z-index: 4;
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin: var(--s4) calc(var(--s5) * -1) calc(var(--s5) * -1);
  padding: var(--s3) var(--s5);
  border-top: 1px solid var(--line);
  background: color-mix(in srgb, var(--surface-2) 94%, transparent);
  backdrop-filter: blur(8px);
}

.textarea--tight {
  min-height: 0;
}

/* 编辑器里十几个输入框堆在一起，不分组就得逐个读标签才知道在改什么。
   分成画面 / 镜头语言 / 声音与字幕 / 已经跑出来的四段。 */
.group {
  margin: var(--s3) 0 2px;
  padding-bottom: 4px;
  border-bottom: 1px solid var(--line);
  font-size: var(--fs-xs);
  font-weight: 700;
  letter-spacing: 0.12em;
  color: var(--text-3);
}
.group:first-child {
  margin-top: 0;
}

.dialogue {
  display: flex;
  align-items: center;
  gap: var(--s2);
}
.dialogue__who {
  width: 6em;
  overflow: hidden;
  text-overflow: ellipsis;
}

.switch {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  cursor: pointer;
  font-size: var(--fs-base);
}
.switch input {
  width: 15px;
  height: 15px;
  accent-color: var(--accent);
}

.preview {
  border-radius: var(--r);
  overflow: hidden;
  border: 1px solid var(--line);
  background: #000;
}
.preview__media {
  display: block;
  width: 100%;
  max-height: 320px;
  object-fit: contain;
}

@media (max-width: 900px) {
  .edit__cols {
    grid-template-columns: 1fr;
  }
}
@media (max-width: 640px) {
  /* 一行放不下这么多。机位和运镜是扫列表时最不需要的两项，
     点开编辑器里都在。按类名藏，不按位置——按 nth-child 藏过一次，
     后来在前面插了场景和角色标签，藏掉的就变成了别的东西。 */
  .t-angle,
  .t-move {
    display: none;
  }
  .shot__tags {
    gap: var(--s2);
  }
  /* 状态标留着，那是扫列表时唯一要看的；口型和时长锁在编辑器里能看到 */
  .shot__flag {
    display: none;
  }
  .shot__row {
    gap: 4px;
    padding-left: var(--s2);
  }
  .shot__main {
    gap: var(--s2);
    padding-right: var(--s2);
  }
  .shot__thumb {
    width: 44px;
    height: 33px;
  }
  .shot__notes {
    margin-left: var(--s4);
  }
  .shot__edit {
    padding: var(--s4);
  }
}
</style>
