<script setup>
/**
 * 第二步：剧本大纲。
 *
 * 剧本是整条流水线的源头。源头没审过就往下跑，后面几十分钟的渲染
 * 全是白跑，所以 AI 写完先摆在这儿给人看，点了「采用」才落库。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import ProgressBar from '@/components/ProgressBar.vue'
import StepHeader from '@/components/StepHeader.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'
import { useWriter } from '@/stores/run'

const session = useSession()
const ui = useUi()
const writer = useWriter()
const { run, isBusy } = useAction()

const premise = ref('')
const durationS = ref(60)
const continueFromPrevious = ref(true)
const reuseCharacters = ref(true)

const draft = ref(null) // AI 刚写完、还没采用的一集
const script = ref('')
const savedScript = ref('')
const scriptLoading = ref(false)

const seriesOpen = ref(false)
const seriesCount = ref(3)

const dirty = computed(() => script.value !== savedScript.value)
const wordCount = computed(() => script.value.replace(/\s/g, '').length)

const DURATIONS = [30, 60, 90, 120, 180]

const fitTone = (fit) =>
  fit === '合适' ? 'ok' : fit === '偏长' ? 'warn' : 'info'

async function loadScript() {
  if (!session.projectPath || !session.episodeId) {
    script.value = ''
    savedScript.value = ''
    return
  }
  scriptLoading.value = true
  try {
    const data = await api.getScript(session.projectPath, session.episodeId)
    script.value = data.script ?? ''
    savedScript.value = script.value
    if (data.target_duration_s) durationS.value = data.target_duration_s
  } catch (err) {
    ui.error(err.message)
  } finally {
    scriptLoading.value = false
  }
}

watch(() => [session.projectPath, session.episodeId], loadScript, { immediate: true })
watch(
  () => session.project?.premise,
  (value) => {
    // 项目上存着上次写的梗概。回填省得用户凭记忆重打一遍。
    if (value && !premise.value) premise.value = value
  },
  { immediate: true },
)

onMounted(() => {
  writer.poll()
})
onUnmounted(() => writer.stop())

// 整季写完要把新出来的几集刷进侧边栏
watch(
  () => writer.running,
  (now, before) => {
    if (before && !now) session.refresh()
  },
)

async function writeOne() {
  if (!premise.value.trim()) {
    ui.warn('先写一句梗概，比如「深夜便利店，前任突然推门进来」')
    return
  }
  const result = await run(
    () =>
      api.writeScript({
        project: session.projectPath,
        episode_id: session.episodeId,
        premise: premise.value.trim(),
        duration_s: durationS.value,
        continue_from_previous: continueFromPrevious.value,
        reuse_characters: reuseCharacters.value,
      }),
    { key: 'write' },
  )
  if (result) draft.value = result
}

/** 把 AI 写的这一集落到项目里。没有剧集就先建一集。 */
async function adoptDraft() {
  if (!draft.value) return
  await run(
    async () => {
      let episodeId = session.episodeId
      if (!episodeId) {
        const created = await api.newEpisode({
          project: session.projectPath,
          title: draft.value.title,
          target_duration_s: durationS.value,
        })
        episodeId = created.episode_id
      } else if (draft.value.title) {
        await api.episodeAction({
          project: session.projectPath,
          episode_id: episodeId,
          action: 'rename',
          new_title: draft.value.title,
        })
      }
      await api.saveScript({
        project: session.projectPath,
        episode_id: episodeId,
        script: draft.value.script,
        duration_s: durationS.value,
      })
      session.selectEpisode(episodeId)
      script.value = draft.value.script
      savedScript.value = script.value
      draft.value = null
    },
    { key: 'adopt', success: '已采用，写进这一集了', refresh: true },
  )
}

async function saveScript() {
  if (!session.episodeId) {
    ui.warn('先建一集再存')
    return
  }
  const done = await run(
    () =>
      api.saveScript({
        project: session.projectPath,
        episode_id: session.episodeId,
        script: script.value,
        duration_s: durationS.value,
      }),
    { key: 'save', success: '剧本已保存', refresh: true },
  )
  if (done) savedScript.value = script.value
}

async function addEpisode() {
  const created = await run(
    () => api.newEpisode({ project: session.projectPath, target_duration_s: durationS.value }),
    { key: 'add', success: '新建了一集' },
  )
  if (created) {
    session.selectEpisode(created.episode_id)
    await session.refresh()
  }
}

async function episodeAction(action) {
  if (action === 'delete' && !confirm('删掉这一集？分镜和剧本一起没。')) return
  const result = await run(
    () =>
      api.episodeAction({
        project: session.projectPath,
        episode_id: session.episodeId,
        action,
      }),
    { key: action, refresh: true },
  )
  if (result?.episode_id && action === 'duplicate') {
    session.selectEpisode(result.episode_id)
  }
  if (action === 'delete') session.selectEpisode('')
  await session.refresh()
}

async function writeSeries() {
  if (!premise.value.trim()) {
    ui.warn('整季也得先有一句梗概')
    return
  }
  const started = await run(
    () =>
      api.writeSeries({
        project: session.projectPath,
        premise: premise.value.trim(),
        episodes: seriesCount.value,
        duration_s: durationS.value,
        reuse_characters: reuseCharacters.value,
      }),
    { key: 'series', success: `开始写 ${seriesCount.value} 集` },
  )
  if (started) writer.start()
}

async function stopSeries() {
  await run(() => api.stopSeries(), { key: 'stopSeries', success: '已停' })
  writer.poll()
}
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader>
      <template #actions>
        <button
          class="btn btn--ghost"
          type="button"
          :disabled="!session.hasProject"
          @click="seriesOpen = !seriesOpen"
        >
          <AppIcon name="board" :size="15" />
          批量写整季
        </button>
        <button
          class="btn btn--ai"
          type="button"
          :disabled="!session.hasProject || isBusy('write')"
          @click="writeOne"
        >
          <AppIcon name="sparkle" :size="15" />
          {{ isBusy('write') ? '大模型正在写…' : 'AI 写这一集' }}
        </button>
      </template>
    </StepHeader>

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="剧本要挂在某个项目上。先回第一步选一个，或者新建一个。"
    >
      <RouterLink to="/project" class="btn btn--primary">去第一步</RouterLink>
    </EmptyState>

    <template v-else>
      <!-- 梗概与设定 -->
      <section class="card">
        <div class="card__head">
          <div>
            <div class="card__title">这部剧讲什么</div>
            <div class="card__sub">一两句话就够。它会被存在项目上，写下一集时接着用。</div>
          </div>
        </div>
        <div class="card__body stack">
          <textarea
            v-model="premise"
            class="textarea"
            rows="3"
            placeholder="例如：深夜便利店，前任推门进来，手里拿着五年前她送的那把伞。"
          />

          <div class="opts">
            <div class="field">
              <span class="field__label">目标时长</span>
              <div class="chips">
                <button
                  v-for="d in DURATIONS"
                  :key="d"
                  class="chip"
                  :class="{ 'chip--on': durationS === d }"
                  type="button"
                  @click="durationS = d"
                >
                  {{ d }} 秒
                </button>
              </div>
            </div>

            <label class="switch">
              <input v-model="continueFromPrevious" type="checkbox" />
              <span>接着前几集写</span>
              <span class="field__hint">把最近三集当上下文，人物关系不会断。</span>
            </label>

            <label class="switch">
              <input v-model="reuseCharacters" type="checkbox" />
              <span>沿用已有角色</span>
              <span class="field__hint">名字不变，角色设定和参考图才能复用。</span>
            </label>
          </div>
        </div>
      </section>

      <!-- 批量写整季 -->
      <Transition name="fold">
        <section v-if="seriesOpen || writer.running" class="card">
          <div class="card__head">
            <div>
              <div class="card__title">批量写整季</div>
              <div class="card__sub">边写边存，写完可以逐集再改。中途能停，已写好的留着。</div>
            </div>
            <button class="btn btn--ghost btn--sm" type="button" @click="seriesOpen = false">
              <AppIcon name="close" :size="14" />
            </button>
          </div>
          <div class="card__body stack">
            <div v-if="writer.running || writer.state?.total" class="stack stack--sm">
              <ProgressBar
                :percent="writer.percent"
                :label="writer.state?.message || '准备中'"
                :detail="`${writer.state?.done ?? 0} / ${writer.state?.total ?? 0} 集`"
                :indeterminate="writer.running && !writer.state?.total"
              />
              <div v-if="writer.state?.episodes?.length" class="serieslist">
                <div
                  v-for="(ep, i) in writer.state.episodes"
                  :key="i"
                  class="serieslist__row"
                  :class="{ 'serieslist__row--bad': ep.error }"
                >
                  <span class="pill" :class="ep.error ? 'pill--danger' : 'pill--ok'">
                    {{ ep.episode_id || '失败' }}
                  </span>
                  <span class="truncate">{{ ep.error || ep.title || ep.logline }}</span>
                  <span v-if="ep.dialogue_chars" class="tiny dim numeric nowrap">
                    {{ ep.dialogue_chars }} 字台词
                  </span>
                </div>
              </div>
              <p v-if="writer.state?.error" class="small" style="color: var(--danger)">
                {{ writer.state.error }}
              </p>
            </div>

            <div class="row row--wrap">
              <label class="field field--inline">
                <span class="field__label">写几集</span>
                <input
                  v-model.number="seriesCount"
                  class="input input--num"
                  type="number"
                  min="1"
                  max="20"
                  :disabled="writer.running"
                />
              </label>
              <button
                v-if="!writer.running"
                class="btn btn--ai"
                type="button"
                :disabled="isBusy('series')"
                @click="writeSeries"
              >
                <AppIcon name="sparkle" :size="15" />
                开始写
              </button>
              <button v-else class="btn btn--danger" type="button" @click="stopSeries">
                <AppIcon name="stop" :size="14" />
                停下
              </button>
            </div>
          </div>
        </section>
      </Transition>

      <!-- AI 草稿 -->
      <Transition name="fold">
        <section v-if="draft" class="card card--draft">
          <div class="card__head">
            <div>
              <div class="card__title">
                <AppIcon name="sparkle" :size="15" class="inline-icon" />
                {{ draft.title || 'AI 写的一集' }}
              </div>
              <div class="card__sub">{{ draft.logline }}</div>
            </div>
            <span class="pill" :class="`pill--${fitTone(draft.fit)}`">篇幅{{ draft.fit }}</span>
          </div>
          <div class="card__body stack">
            <div class="metrics">
              <div class="metric">
                <span class="metric__n numeric">{{ draft.dialogue_chars }}</span>
                <span class="metric__l">台词字数</span>
              </div>
              <div class="metric">
                <span class="metric__n numeric">{{ draft.budget_chars }}</span>
                <span class="metric__l">这个时长的预算</span>
              </div>
              <div class="metric">
                <span class="metric__n numeric">{{ draft.beats }}</span>
                <span class="metric__l">段落</span>
              </div>
              <div class="metric">
                <span class="metric__n numeric">{{ draft.speakers?.length ?? 0 }}</span>
                <span class="metric__l">出场人物</span>
              </div>
            </div>

            <p v-if="draft.fit !== '合适'" class="hintline small">
              <AppIcon name="info" :size="14" />
              {{
                draft.fit === '偏长'
                  ? '台词写多了，配音会把镜头撑爆。要么调长目标时长，要么让它重写。'
                  : '台词偏少，成片可能凑不够时长。可以调短目标时长，或者重写一遍。'
              }}
            </p>

            <div class="row row--wrap">
              <span v-for="s in draft.speakers" :key="s" class="pill pill--neutral">{{ s }}</span>
            </div>

            <pre class="script-preview">{{ draft.script }}</pre>
          </div>
          <div class="card__foot">
            <button
              class="btn btn--primary"
              type="button"
              :disabled="isBusy('adopt')"
              @click="adoptDraft"
            >
              <AppIcon name="check" :size="15" />
              {{ isBusy('adopt') ? '正在写入…' : '采用这一版' }}
            </button>
            <button
              class="btn"
              type="button"
              :disabled="isBusy('write')"
              @click="writeOne"
            >
              <AppIcon name="refresh" :size="15" />
              让它重写
            </button>
            <button class="btn btn--ghost" type="button" @click="draft = null">丢弃</button>
          </div>
        </section>
      </Transition>

      <!-- 当前这一集 -->
      <section class="card">
        <div class="card__head">
          <div>
            <div class="card__title">
              {{ session.episodeId ? `${session.episodeId} 的剧本` : '还没有剧集' }}
            </div>
            <div class="card__sub">
              {{
                session.episodeId
                  ? '手改也行。存下来之后再去下一步出角色和分镜。'
                  : '这个项目还一集都没有。让 AI 写一集，或者手动建一集。'
              }}
            </div>
          </div>
          <div class="row">
            <button class="btn btn--ghost btn--sm" type="button" @click="addEpisode">
              <AppIcon name="plus" :size="14" />
              新建一集
            </button>
            <button
              v-if="session.episodeId"
              class="btn btn--ghost btn--sm"
              type="button"
              @click="episodeAction('duplicate')"
            >
              复制
            </button>
            <button
              v-if="session.episodeId && session.episodes.length > 1"
              class="btn btn--ghost btn--sm"
              type="button"
              @click="episodeAction('delete')"
            >
              <AppIcon name="trash" :size="14" />
            </button>
          </div>
        </div>

        <div v-if="session.episodeId" class="card__body stack">
          <textarea
            v-model="script"
            class="textarea textarea--script"
            :placeholder="scriptLoading ? '读取中…' : '还没有剧本。用上面的「AI 写这一集」，或者直接在这里写。'"
            spellcheck="false"
          />
          <div class="row row--between small dim">
            <span class="numeric">{{ wordCount }} 字</span>
            <span v-if="dirty" class="pill pill--warn">有未保存的改动</span>
          </div>
        </div>
        <div v-else class="card__body">
          <p class="muted small">先建一集，或者让 AI 写一集，采用之后会自动建。</p>
        </div>

        <div v-if="session.episodeId" class="card__foot">
          <button
            class="btn btn--primary"
            type="button"
            :disabled="!dirty || isBusy('save')"
            @click="saveScript"
          >
            {{ isBusy('save') ? '保存中…' : '保存剧本' }}
          </button>
          <button
            class="btn btn--ghost"
            type="button"
            :disabled="!dirty"
            @click="script = savedScript"
          >
            撤销改动
          </button>
          <span class="spacer" />
          <RouterLink class="btn" to="/characters">
            下一步：角色
            <AppIcon name="arrowRight" :size="14" />
          </RouterLink>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.inline-icon {
  display: inline-block;
  vertical-align: -2px;
  color: var(--accent);
}

.opts {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: var(--s4);
  padding-top: var(--s2);
  border-top: 1px solid var(--line);
}

.chips {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}
.chip {
  padding: 4px var(--s3);
  border-radius: var(--r-pill);
  border: 1px solid var(--line);
  background: var(--surface-2);
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
  transition: all 0.14s var(--ease);
}
.chip:hover {
  border-color: var(--line-strong);
  color: var(--text);
}
.chip--on {
  background: var(--accent-soft);
  border-color: var(--accent-line);
  color: var(--accent);
  font-weight: 600;
}

.switch {
  display: grid;
  grid-template-columns: auto 1fr;
  align-items: center;
  gap: var(--s2) var(--s2);
  cursor: pointer;
  font-size: var(--fs-base);
}
.switch input {
  width: 16px;
  height: 16px;
  accent-color: var(--accent);
  cursor: pointer;
}
.switch .field__hint {
  grid-column: 2;
  margin-top: -4px;
}

.field--inline {
  flex-direction: row;
  align-items: center;
}
.input--num {
  width: 76px;
}

.card--draft {
  border-color: var(--accent-line);
  box-shadow: 0 0 0 1px var(--accent-soft), var(--shadow-2);
}

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
.metric__n {
  font-size: var(--fs-xl);
  font-weight: 700;
  line-height: 1.2;
}
.metric__l {
  font-size: var(--fs-xs);
  color: var(--text-3);
}

.hintline {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
  padding: var(--s3);
  border-radius: var(--r);
  background: var(--warn-soft);
  color: var(--warn);
  line-height: 1.6;
}

.script-preview {
  margin: 0;
  max-height: 340px;
  overflow: auto;
  padding: var(--s4);
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
  font-family: var(--font);
  font-size: var(--fs-base);
  line-height: 1.85;
  white-space: pre-wrap;
  word-break: break-word;
}

.textarea--script {
  min-height: 340px;
  line-height: 1.9;
}

.serieslist {
  display: flex;
  flex-direction: column;
  gap: 4px;
  max-height: 200px;
  overflow-y: auto;
}
.serieslist__row {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: 4px var(--s2);
  border-radius: var(--r-sm);
  background: var(--bg-sunken);
  font-size: var(--fs-sm);
}
.serieslist__row--bad {
  background: var(--danger-soft);
}

.fold-enter-active,
.fold-leave-active {
  transition: opacity 0.18s var(--ease), transform 0.18s var(--ease);
}
.fold-enter-from,
.fold-leave-to {
  opacity: 0;
  transform: translateY(-8px);
}
</style>
