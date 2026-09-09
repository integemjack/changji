<script setup>
/**
 * 第六步：制作。
 *
 * 一按下去就是几十分钟，所以按之前要说清楚这一次会做什么、大概多久。
 * 跑起来之后界面只干两件事：进度看得见，随时能停。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import ProgressBar from '@/components/ProgressBar.vue'
import StepHeader from '@/components/StepHeader.vue'
import { api, mediaUrl } from '@/api'
import { STAGE_LABELS, statusOf } from '@/api/labels'
import { humanTime, useAction } from '@/composables/useAction'
import { useRun } from '@/stores/run'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const runStore = useRun()
const { run, isBusy } = useAction()

const preview = ref(null)
const previewError = ref('')
const doctor = ref(null)
const shots = ref([])

const allEpisodes = ref(false)
const skipFinal = ref(false)
const force = ref(false)

const canStart = computed(
  () => Boolean(session.episodeId || allEpisodes.value) && !runStore.running,
)
const blocked = computed(() => doctor.value && doctor.value.can_run === false)

async function loadPreview() {
  previewError.value = ''
  preview.value = null
  if (!session.projectPath) return
  if (!session.episodeId && !allEpisodes.value) return
  try {
    preview.value = await api.runPreview({
      path: session.projectPath,
      episode_id: session.episodeId,
      all_episodes: allEpisodes.value,
      skip_final: skipFinal.value,
      force: force.value,
    })
  } catch (err) {
    previewError.value = err.message
  }
}

async function loadShots() {
  if (!session.projectPath || !session.episodeId) {
    shots.value = []
    return
  }
  try {
    const data = await api.shots(session.projectPath, session.episodeId)
    shots.value = data.shots ?? []
  } catch {
    shots.value = []
  }
}

async function loadDoctor() {
  try {
    doctor.value = await api.doctor()
  } catch (err) {
    doctor.value = { can_run: false, checks: [{ name: '体检', level: 'error', detail: err.message }] }
  }
}

watch(
  () => [session.projectPath, session.episodeId, allEpisodes.value, skipFinal.value, force.value],
  () => {
    loadPreview()
    loadShots()
  },
  { immediate: true },
)

/**
 * 跑起来的时候定期重读分镜。
 *
 * 缩略图墙是这一页唯一能看见「片子长什么样」的地方。不刷的话它停在
 * 开跑那一刻，几十分钟里画面一动不动，只有一行文字在变——用户没法
 * 判断出来的东西对不对，只能等全跑完才发现方向就错了。
 *
 * 六秒一次。首帧出一张要十几秒，跟得上；比这更频繁只是白问。
 */
let shotTimer = null

function watchShots(on) {
  if (on && !shotTimer) shotTimer = setInterval(loadShots, 6000)
  if (!on && shotTimer) {
    clearInterval(shotTimer)
    shotTimer = null
  }
}

watch(() => runStore.running, (now) => watchShots(now))

onMounted(() => {
  loadDoctor()
  runStore.start()
})
onUnmounted(() => {
  runStore.stop()
  watchShots(false)
})

// 跑完了要刷新分镜状态和整体进度，不然界面停在跑之前那一刻
watch(
  () => runStore.running,
  (now, before) => {
    if (before && !now) {
      loadShots()
      loadPreview()
      session.refresh()
      if (runStore.state?.error) ui.error(runStore.state.error)
      else ui.ok('这一轮跑完了')
    }
  },
)

async function start() {
  const started = await run(
    () =>
      api.run({
        project: session.projectPath,
        episode_id: session.episodeId,
        all_episodes: allEpisodes.value,
        skip_final: skipFinal.value,
        force: force.value,
      }),
    { key: 'start' },
  )
  if (started) {
    ui.info(`开跑：${started.queue.join('、')}`)
    runStore.start()
  }
}

async function stop() {
  await run(() => api.stopRun(), { key: 'stop', success: '已停，跑完的镜头留着' })
  runStore.poll()
}

const eventTone = (kind) =>
  kind === 'error' ? 'danger' : kind === 'warn' ? 'warn' : 'neutral'

const logMode = ref('shot')

/**
 * 事件按镜头归拢。
 *
 * 流水线一个镜头要走配音、首帧、草稿、闸门、成片好几步，八十条事件
 * 平铺下来，想知道「第 5 镜卡在哪」得从头往下数。按镜头归拢之后一行
 * 就是一个镜头的现状，出问题的那几行还能一眼挑出来。
 */
const grouped = computed(() => {
  const byShot = new Map()
  const global = []
  for (const e of runStore.events) {
    if (!e.shot_id) {
      global.push(e)
      continue
    }
    const g = byShot.get(e.shot_id) ?? { shot_id: e.shot_id, events: [], worst: 'info' }
    g.events.push(e)
    if (e.kind === 'error') g.worst = 'error'
    else if (e.kind === 'warn' && g.worst !== 'error') g.worst = 'warn'
    byShot.set(e.shot_id, g)
  }
  // 出问题的排前面，其余保持发生顺序
  const rank = { error: 0, warn: 1, info: 2 }
  const shots = [...byShot.values()].sort((a, b) => rank[a.worst] - rank[b.worst])
  return { shots, global: global.slice(-6).reverse() }
})

const shotNumberOf = (shotId) => {
  const at = shots.value.find((s) => s.shot_id === shotId)
  return at ? at.order + 1 : ''
}

/**
 * 现在在跑哪几镜。多卡时同时有好几镜，缩略图墙上一起点亮。
 *
 * 优先用引擎推上来的"正在跑"表；表是空的（老引擎没带 kind，或者
 * 还没收到第一条）就退回"最后一条带 shot_id 的事件"。
 */
const activeShotIds = computed(() => {
  if (!runStore.running) return new Set()
  if (runStore.inflight.length) return new Set(runStore.inflight.map((x) => x.shot_id))
  for (let i = runStore.events.length - 1; i >= 0; i -= 1) {
    if (runStore.events[i].shot_id) return new Set([runStore.events[i].shot_id])
  }
  return new Set()
})

/** 一镜跑了多久。 */
const sinceText = (ms) => humanTime(Math.max(0, (Date.now() - ms) / 1000))
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader>
      <template #actions>
        <button
          v-if="!runStore.running"
          class="btn btn--primary btn--lg"
          type="button"
          :disabled="!canStart || isBusy('start') || preview?.idle"
          @click="start"
        >
          <AppIcon name="play" :size="15" />
          {{ isBusy('start') ? '正在启动…' : '开始制作' }}
        </button>
        <button v-else class="btn btn--danger btn--lg" type="button" @click="stop">
          <AppIcon name="stop" :size="14" />
          停下
        </button>
      </template>
    </StepHeader>

    <EmptyState
      v-if="!session.episodeId && !allEpisodes"
      icon="board"
      tone="warn"
      title="还没选到某一集"
      hint="制作是针对某一集的分镜表跑的。先选一集，或者勾上「整个项目一起跑」。"
    >
      <RouterLink to="/storyboard" class="btn btn--primary">去出分镜</RouterLink>
    </EmptyState>

    <template v-else>
      <!-- 运行中 -->
      <section v-if="runStore.running || runStore.state?.error" class="card card--live">
        <div class="card__head">
          <div class="card__title row">
            <span class="livedot" :class="{ 'livedot--off': !runStore.running }" />
            {{ runStore.running ? `正在跑 ${runStore.state?.episode_id || ''}` : '上一轮已结束' }}
          </div>
          <span v-if="runStore.state?.elapsed_s" class="card__sub numeric">
            已用 {{ humanTime(runStore.state.elapsed_s) }}
          </span>
        </div>
        <div class="card__body stack">
          <ProgressBar
            :percent="runStore.percent"
            :label="`${runStore.stageLabel}：${runStore.state?.message || ''}`"
            :detail="
              runStore.state?.total
                ? `${runStore.state.current} / ${runStore.state.total}`
                : ''
            "
            :indeterminate="runStore.running && !runStore.state?.total"
            :tone="runStore.state?.error ? 'danger' : 'accent'"
          />
          <ProgressBar
            v-if="(runStore.state?.queue_total ?? 1) > 1"
            :percent="
              ((runStore.state.queue_done || 0) / runStore.state.queue_total) * 100
            "
            label="整个队列"
            :detail="`${runStore.state.queue_done} / ${runStore.state.queue_total} 集`"
            tone="ok"
          />

          <div v-if="runStore.inflight.length" class="inflight">
            <div class="inflight__head tiny dim">
              同时在跑 {{ runStore.inflight.length }} 镜
            </div>
            <div
              v-for="x in runStore.inflight"
              :key="x.shot_id"
              class="inflight__row"
            >
              <span class="inflight__no numeric">#{{ shotNumberOf(x.shot_id) || '?' }}</span>
              <span class="inflight__id mono truncate">{{ x.shot_id }}</span>
              <span class="inflight__stage tiny">{{ STAGE_LABELS[x.stage] || x.stage }}</span>
              <span class="inflight__msg truncate dim">{{ x.message }}</span>
              <span class="inflight__since tiny dim numeric nowrap">{{ sinceText(x.since) }}</span>
            </div>
          </div>

          <p v-if="runStore.state?.error" class="alert alert--bad">
            <AppIcon name="warn" :size="15" />
            {{ runStore.state.error }}
          </p>

          <template v-if="runStore.events.length">
            <div class="row row--between">
              <div class="chips">
                <button
                  v-for="m in [
                    { v: 'shot', l: '按镜头' },
                    { v: 'time', l: '按时间' },
                  ]"
                  :key="m.v"
                  class="chip"
                  :class="{ 'chip--on': logMode === m.v }"
                  type="button"
                  @click="logMode = m.v"
                >
                  {{ m.l }}
                </button>
              </div>
              <span class="tiny dim numeric">{{ runStore.events.length }} 条事件</span>
            </div>

            <!-- 按时间：原始流水 -->
            <div v-if="logMode === 'time'" class="log">
              <div
                v-for="(e, i) in [...runStore.events].reverse()"
                :key="i"
                class="log__row"
                :class="`log__row--${eventTone(e.kind)}`"
              >
                <span class="log__stage tiny">{{ STAGE_LABELS[e.stage] || e.stage }}</span>
                <span class="log__msg truncate">{{ e.message }}</span>
                <span v-if="e.shot_id" class="tiny dim mono nowrap">{{ e.shot_id }}</span>
              </div>
            </div>

            <!-- 按镜头：一行一个镜头的现状，出问题的排前面 -->
            <div v-else class="log">
              <details
                v-for="g in grouped.shots"
                :key="g.shot_id"
                class="glog"
                :class="`glog--${g.worst}`"
              >
                <summary class="glog__head">
                  <span class="glog__no numeric">
                    {{ shotNumberOf(g.shot_id) || '·' }}
                  </span>
                  <span class="glog__stage tiny">
                    {{ STAGE_LABELS[g.events.at(-1).stage] || g.events.at(-1).stage }}
                  </span>
                  <span class="glog__msg truncate">{{ g.events.at(-1).message }}</span>
                  <span class="tiny dim numeric nowrap">{{ g.events.length }} 步</span>
                </summary>
                <div
                  v-for="(e, i) in g.events"
                  :key="i"
                  class="log__row"
                  :class="`log__row--${eventTone(e.kind)}`"
                >
                  <span class="log__stage tiny">{{ STAGE_LABELS[e.stage] || e.stage }}</span>
                  <span class="log__msg">{{ e.message }}</span>
                </div>
              </details>

              <div
                v-for="(e, i) in grouped.global"
                :key="'g' + i"
                class="log__row"
                :class="`log__row--${eventTone(e.kind)}`"
              >
                <span class="log__stage tiny">{{ STAGE_LABELS[e.stage] || e.stage }}</span>
                <span class="log__msg truncate">{{ e.message }}</span>
              </div>
            </div>
          </template>
        </div>
      </section>

      <!-- 开跑之前 -->
      <section v-if="!runStore.running" class="card">
        <div class="card__head">
          <div>
            <div class="card__title">这一次会做什么</div>
            <div class="card__sub">按下之前先看清楚，一跑就是几十分钟。</div>
          </div>
          <button class="btn btn--ghost btn--sm" type="button" @click="loadPreview">
            <AppIcon name="refresh" :size="14" />
          </button>
        </div>
        <div class="card__body stack">
          <p v-if="previewError" class="alert alert--bad">
            <AppIcon name="warn" :size="15" />
            {{ previewError }}
          </p>

          <template v-else-if="preview">
            <p v-if="preview.idle" class="alert alert--ok">
              <AppIcon name="check" :size="15" />
              这一集所有镜头都跑完了。想重来的话勾上「不管状态，全部重跑」。
            </p>
            <template v-else>
              <div class="stages">
                <div v-for="st in preview.stages" :key="st.stage" class="stagebox">
                  <span class="stagebox__n numeric">{{ st.shots }}</span>
                  <span class="stagebox__l">{{ st.label }}</span>
                </div>
              </div>
              <p class="estimate">
                <AppIcon name="info" :size="15" />
                预计 <b>{{ preview.estimate_text || '算不出来' }}</b>
                ，覆盖 {{ preview.episodes.join('、') }} 共 {{ preview.shots }} 个镜头。
                中途可以停，跑完的镜头会留着。
              </p>
            </template>
          </template>

          <div class="opts">
            <label class="switch">
              <input v-model="allEpisodes" type="checkbox" />
              <span>整个项目一起跑</span>
              <span class="field__hint">把所有已出分镜的集排成队列，一集接一集。</span>
            </label>
            <label class="switch">
              <input v-model="skipFinal" type="checkbox" />
              <span>只跑到草稿档</span>
              <span class="field__hint">先看叙事和构图对不对，过了再升成片档。快十几倍。</span>
            </label>
            <label class="switch">
              <input v-model="force" type="checkbox" />
              <span>不管状态，全部重跑</span>
              <span class="field__hint">已完成的也重来。改了风格或角色之后才需要。</span>
            </label>
          </div>
        </div>
      </section>

      <!-- 体检 -->
      <section v-if="doctor" class="card" :class="{ 'card--bad': blocked }">
        <div class="card__head">
          <div>
            <div class="card__title">
              开工体检
              <span class="pill" :class="doctor.can_run ? 'pill--ok' : 'pill--danger'">
                {{ doctor.can_run ? '可以开工' : '还不能跑' }}
              </span>
            </div>
            <div class="card__sub">ComfyUI、大模型、FFmpeg 三样缺一不可。</div>
          </div>
          <button class="btn btn--ghost btn--sm" type="button" @click="loadDoctor">
            <AppIcon name="refresh" :size="14" />
          </button>
        </div>
        <div class="card__body stack stack--sm">
          <div
            v-for="c in doctor.checks"
            :key="c.name"
            class="check"
            :class="`check--${c.level}`"
          >
            <AppIcon :name="c.level === 'ok' ? 'check' : 'warn'" :size="14" />
            <span class="check__name nowrap">{{ c.name }}</span>
            <span class="check__detail">{{ c.detail }}</span>
            <span v-if="c.fix" class="check__fix tiny dim">{{ c.fix }}</span>
          </div>
          <RouterLink v-if="blocked" to="/settings" class="btn btn--primary">
            去设置里改地址
          </RouterLink>
        </div>
      </section>

      <!-- 镜头状态一览 -->
      <section v-if="shots.length" class="card">
        <div class="card__head">
          <div>
            <div class="card__title">镜头状态</div>
            <div class="card__sub">每一格是一个镜头，有首帧的显示首帧。</div>
          </div>
          <RouterLink to="/storyboard" class="btn btn--ghost btn--sm">去改分镜</RouterLink>
        </div>
        <div class="card__body">
          <div class="tiles">
            <div
              v-for="s in shots"
              :key="s.shot_id"
              class="tile"
              :class="[
                `tile--${statusOf(s.status).tone}`,
                { 'tile--active': activeShotIds.has(s.shot_id) },
              ]"
              :title="`${s.order + 1}. ${s.visual_desc || ''}｜${statusOf(s.status).label}`"
            >
              <img
                v-if="s.frame_path"
                :src="mediaUrl(session.projectPath, s.frame_path)"
                :alt="`镜头 ${s.order + 1}`"
                loading="lazy"
              />
              <span v-else class="tile__num numeric">{{ s.order + 1 }}</span>
              <span class="tile__bar" />
            </div>
          </div>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.card--live {
  border-color: var(--accent-line);
  box-shadow: 0 0 0 1px var(--accent-soft), var(--shadow-2);
}
.card--bad {
  border-color: color-mix(in srgb, var(--danger) 40%, transparent);
}

.livedot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--accent);
  box-shadow: 0 0 0 4px var(--accent-soft);
  animation: beat 1.1s var(--ease) infinite;
}
.livedot--off {
  background: var(--text-3);
  box-shadow: none;
  animation: none;
}
@keyframes beat {
  50% {
    opacity: 0.35;
  }
}

.alert {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
  padding: var(--s3);
  border-radius: var(--r);
  font-size: var(--fs-base);
  line-height: 1.6;
}
.alert--bad {
  background: var(--danger-soft);
  color: var(--danger);
}
.alert--ok {
  background: var(--ok-soft);
  color: var(--ok);
}

.log {
  max-height: 240px;
  overflow-y: auto;
  border-radius: var(--r);
  border: 1px solid var(--line);
  background: var(--bg-sunken);
}
.log__row {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: 4px var(--s3);
  font-size: var(--fs-sm);
  border-bottom: 1px solid color-mix(in srgb, var(--line) 60%, transparent);
}
.log__row:last-child {
  border-bottom: none;
}
.log__row--warn {
  color: var(--warn);
}
.log__row--danger {
  color: var(--danger);
  background: var(--danger-soft);
}
.log__stage {
  flex: none;
  width: 4.5em;
  color: var(--text-3);
}
.log__msg {
  flex: 1;
  min-width: 0;
}

.glog {
  border-bottom: 1px solid color-mix(in srgb, var(--line) 60%, transparent);
}
.glog:last-child {
  border-bottom: none;
}
.glog__head {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: 5px var(--s3);
  cursor: pointer;
  font-size: var(--fs-sm);
  list-style: none;
}
.glog__head::-webkit-details-marker {
  display: none;
}
.glog__head:hover {
  background: var(--surface-2);
}
.glog__no {
  flex: none;
  display: grid;
  place-items: center;
  min-width: 20px;
  height: 18px;
  padding: 0 4px;
  border-radius: var(--r-sm);
  background: var(--surface-3);
  color: var(--text-2);
  font-size: 10px;
  font-weight: 700;
}
.glog--warn .glog__no {
  background: var(--warn-soft);
  color: var(--warn);
}
.glog--error .glog__no {
  background: var(--danger-soft);
  color: var(--danger);
}
.glog__stage {
  flex: none;
  width: 4.5em;
  color: var(--text-3);
}
.glog__msg {
  flex: 1;
  min-width: 0;
}
.glog[open] > .glog__head {
  background: var(--surface-2);
  font-weight: 600;
}
.glog .log__row {
  padding-left: var(--s6);
  background: var(--bg-sunken);
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

.stages {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(96px, 1fr));
  gap: var(--s3);
}
.stagebox {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: var(--s3);
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
}
.stagebox__n {
  font-size: var(--fs-xl);
  font-weight: 700;
  color: var(--accent);
}
.stagebox__l {
  font-size: var(--fs-xs);
  color: var(--text-3);
}

.estimate {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
  padding: var(--s3);
  border-radius: var(--r);
  background: var(--info-soft);
  color: var(--text-2);
  font-size: var(--fs-base);
  line-height: 1.6;
}
.estimate :deep(svg) {
  color: var(--info);
  margin-top: 3px;
}
.estimate b {
  color: var(--text);
}

.opts {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(210px, 1fr));
  gap: var(--s4);
  padding-top: var(--s3);
  border-top: 1px solid var(--line);
}
.switch {
  display: grid;
  grid-template-columns: auto 1fr;
  align-items: center;
  gap: var(--s2);
  cursor: pointer;
  font-size: var(--fs-base);
}
.switch input {
  width: 16px;
  height: 16px;
  accent-color: var(--accent);
}
.switch .field__hint {
  grid-column: 2;
  margin-top: -4px;
}

.check {
  display: flex;
  align-items: baseline;
  gap: var(--s2);
  padding: var(--s2) var(--s3);
  border-radius: var(--r-sm);
  background: var(--bg-sunken);
  font-size: var(--fs-sm);
  line-height: 1.5;
  flex-wrap: wrap;
}
.check :deep(svg) {
  align-self: center;
  color: var(--text-3);
}
.check--ok :deep(svg) {
  color: var(--ok);
}
.check--warn {
  background: var(--warn-soft);
}
.check--warn :deep(svg) {
  color: var(--warn);
}
.check--error {
  background: var(--danger-soft);
}
.check--error :deep(svg) {
  color: var(--danger);
}
.check__name {
  font-weight: 600;
  width: 7em;
}
.check__detail {
  flex: 1;
  min-width: 12ch;
}

.tiles {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(64px, 1fr));
  gap: var(--s2);
}
.tile {
  position: relative;
  aspect-ratio: 9 / 16;
  border-radius: var(--r-sm);
  overflow: hidden;
  background: var(--surface-3);
  display: grid;
  place-items: center;
  color: var(--text-3);
  border: 1px solid var(--line);
}
.tile img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.tile__num {
  font-size: var(--fs-sm);
  font-weight: 700;
}
.tile__bar {
  position: absolute;
  left: 0;
  right: 0;
  bottom: 0;
  height: 3px;
  background: var(--text-3);
}
.tile--info .tile__bar {
  background: var(--info);
}
.tile--warn .tile__bar {
  background: var(--warn);
}
.tile--ok .tile__bar {
  background: var(--ok);
}
.tile--active {
  border-color: var(--accent);
  box-shadow: 0 0 0 2px var(--accent-soft);
  animation: working 1.4s var(--ease) infinite;
}
@keyframes working {
  50% {
    box-shadow: 0 0 0 5px var(--accent-soft);
  }
}

/* 正在跑的几镜 */
.inflight {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: 8px 10px;
  border-radius: var(--radius-sm, 6px);
  background: var(--surface-2, rgba(127, 127, 127, 0.08));
}
.inflight__row {
  display: grid;
  grid-template-columns: 3.5em minmax(6em, 12em) 4.5em 1fr auto;
  gap: 8px;
  align-items: center;
  font-size: 0.92em;
}
.inflight__no { text-align: right; }
.inflight__stage {
  padding: 1px 6px;
  border-radius: 999px;
  background: var(--accent-soft, rgba(80, 140, 255, 0.15));
}
</style>
