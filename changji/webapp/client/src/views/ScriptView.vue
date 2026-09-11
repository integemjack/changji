<script setup>
/**
 * 第二步：剧本大纲。全剧阶段。
 *
 * 这一页管的是整部剧：一句梗概定调子，然后分成若干集，每集一份剧本。
 * 不是「当前这一集的编辑器」——那样的话写第五集时得先去顶栏换集号，
 * 而这时候第五集还不存在。
 *
 * 剧本是整条流水线的源头。源头没审过就往下跑，后面几十分钟的渲染全是
 * 白跑，所以 AI 写完先摆出来给人看，点了采用才落库。
 */
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import ProgressBar from '@/components/ProgressBar.vue'
import ScriptReader from '@/components/ScriptReader.vue'
import StepHeader from '@/components/StepHeader.vue'
import { api } from '@/api'
import { humanTime, useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'
import { useWriter } from '@/stores/run'

const session = useSession()
const ui = useUi()
const writer = useWriter()
const { run, isBusy } = useAction()

const premise = ref('')
const savedPremise = ref('')
const durationS = ref(60)
const continueFromPrevious = ref(true)
const reuseCharacters = ref(true)
const seriesCount = ref(3)

const draft = ref(null) // AI 刚写完、还没采用的一集
const draftFor = ref('') // 这份草稿是给哪一集写的，空表示新的一集

const openEp = ref('') // 展开在编辑的那一集
const script = ref('')
const savedScript = ref('')
const scriptLoading = ref(false)
const mode = ref('read')

const DURATIONS = [30, 60, 90, 120, 180]
const TRAILER_DURATIONS = [15, 20, 30, 45]
// 预告片挂在固定集号上，只有一条，重剪覆盖上一条
const TRAILER_ID = 'trailer'

const trailerDurationS = ref(20)

// 选题。「这部剧讲什么」是整条流水线的源头，也是最难从零开始的一步，
// 所以给三个方案挑，而不是让人对着空白框发呆。
const ideas = ref([])
const ideaKeywords = ref('')

async function suggestPremises() {
  const result = await run(
    () =>
      api.suggestPremises({
        project: session.projectPath,
        keywords: ideaKeywords.value.trim(),
        count: 3,
      }),
    { key: 'ideas' },
  )
  if (result) ideas.value = result.ideas ?? []
}

async function useIdea(idea) {
  premise.value = idea.premise
  ideas.value = []
  // 挑完直接存。挑一个选题本身就是个决定，还要再点一次「保存」
  // 是多余的一步，而漏掉那一步的人会以为选题丢了。
  await savePremise()
}

// 预告片也是一集，但它不该混在正片列表里——那是「第几集」的清单，
// 预告片不占集号，也不参与「接着前几集写」的上下文。
const episodes = computed(() =>
  session.episodes.filter((e) => e.episode_id !== TRAILER_ID),
)
const trailer = computed(
  () => session.episodes.find((e) => e.episode_id === TRAILER_ID) ?? null,
)
const dirty = computed(() => script.value !== savedScript.value)
const premiseDirty = computed(() => premise.value.trim() !== savedPremise.value.trim())
const wordCount = computed(() => script.value.replace(/\s/g, '').length)
const totalDuration = computed(() =>
  episodes.value.reduce((a, e) => a + (e.duration_s || 0), 0),
)
const writtenCount = computed(() => episodes.value.filter((e) => e.synopsis).length)

const fitTone = (fit) => (fit === '合适' ? 'ok' : fit === '偏长' ? 'warn' : 'info')

watch(
  () => session.project?.premise,
  (value) => {
    if (value !== undefined && !premiseDirty.value) {
      premise.value = value ?? ''
      savedPremise.value = value ?? ''
    }
  },
  { immediate: true },
)

onMounted(() => writer.poll())
onUnmounted(() => writer.stop())

// 整季写完要把新出来的几集刷进列表
watch(
  () => writer.running,
  (now, before) => {
    if (before && !now) session.refresh()
  },
)

async function openEpisode(episodeId) {
  // 改了剧本直接收起来，改动就没了。先问一句。
  if (dirty.value && !confirm('这一集的剧本有改动还没保存，收起就没了。确定？')) {
    return
  }
  if (openEp.value === episodeId) {
    openEp.value = ''
    return
  }
  openEp.value = episodeId
  script.value = ''
  savedScript.value = ''
  scriptLoading.value = true
  try {
    const data = await api.getScript(session.projectPath, episodeId)
    script.value = data.script ?? ''
    savedScript.value = script.value
    mode.value = script.value.trim() ? 'read' : 'edit'
    if (data.target_duration_s) durationS.value = data.target_duration_s
  } catch (err) {
    ui.error(err.message)
  } finally {
    scriptLoading.value = false
  }
  // 展开的剧本很长，点列表靠下那几集时内容全在屏幕外面，
  // 看上去像「点了没反应」。渲染完把它拉回视野。
  await nextTick()
  document
    .querySelector(`[data-ep="${episodeId}"]`)
    ?.scrollIntoView({ behavior: 'smooth', block: 'start' })
}

/** 写一集。episodeId 留空表示写新的一集。 */
async function writeOne(episodeId = '') {
  if (!premise.value.trim()) {
    ui.warn('先写一句梗概，比如「深夜便利店，前任突然推门进来」')
    return
  }
  const result = await run(
    () =>
      api.writeScript({
        project: session.projectPath,
        episode_id: episodeId,
        premise: premise.value.trim(),
        duration_s: durationS.value,
        continue_from_previous: continueFromPrevious.value,
        reuse_characters: reuseCharacters.value,
      }),
    { key: 'write' },
  )
  if (result) {
    draft.value = result
    draftFor.value = episodeId
    savedPremise.value = premise.value.trim()
  }
}

/**
 * 剪一条预告片。
 *
 * 对流水线来说预告片就是特别短的一集：采用之后照样走场景、分镜、制作、
 * 成片、上传。区别只在写的时候——要的是钩子不是完整故事。
 */
async function writeTrailer() {
  const result = await run(
    () =>
      api.writeTrailer({
        project: session.projectPath,
        duration_s: trailerDurationS.value,
        reuse_characters: reuseCharacters.value,
      }),
    { key: 'trailer' },
  )
  if (result) {
    draft.value = result
    draftFor.value = TRAILER_ID
  }
}

/** 把草稿落到项目里。没指定集号就新建一集；集号不存在就按它新建。 */
async function adoptDraft() {
  if (!draft.value) return
  const isTrailer = draftFor.value === TRAILER_ID
  const targetDuration = isTrailer ? trailerDurationS.value : durationS.value
  await run(
    async () => {
      let episodeId = draftFor.value
      const exists = session.episodes.some((e) => e.episode_id === episodeId)
      if (!episodeId || !exists) {
        const created = await api.newEpisode({
          project: session.projectPath,
          episode_id: episodeId,
          title: draft.value.title,
          target_duration_s: targetDuration,
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
        duration_s: targetDuration,
        // 一句话梗概跟着剧本一起存。不存的话这一集在项目里就是"没梗概"，
        // 下面的「已写 N 集」和预告片那个按钮都当它不存在，
        // 下次写新一集也接不上前文。
        synopsis: draft.value.logline,
      })
      // 后面几步跟着这一集走
      session.selectEpisode(episodeId)
      openEp.value = episodeId
      script.value = draft.value.script
      savedScript.value = script.value
      mode.value = 'read'
      draft.value = null
      draftFor.value = ''
    },
    { key: 'adopt', success: '已采用，写进项目了', refresh: true },
  )
}

async function saveScript() {
  const done = await run(
    () =>
      api.saveScript({
        project: session.projectPath,
        episode_id: openEp.value,
        script: script.value,
        duration_s: durationS.value,
      }),
    { key: 'save', success: '剧本已保存', refresh: true },
  )
  if (done) savedScript.value = script.value
}

/**
 * 存梗概。
 *
 * 也挂在输入框的 blur 上。原来只有卡片底栏一个按钮，而那个底栏里还放着
 * 「一口气写几集 / 批量写整季」，按钮读起来不像是在存上面那个框——
 * 有人填完直接去点了别的，回头发现没存上。
 *
 * 存完照样弹一句：用户抱怨的就是「不知道到底存没存上」，这时候安静
 * 反而是错的。
 */
async function savePremise() {
  if (!premiseDirty.value || !session.projectPath) return
  const target = premise.value.trim()
  const done = await run(
    () => api.savePremise({ project: session.projectPath, premise: target }),
    { key: 'premise', success: '梗概已存到项目上', refresh: true },
  )
  if (done) savedPremise.value = target
}

async function addEpisode() {
  const created = await run(
    () =>
      api.newEpisode({ project: session.projectPath, target_duration_s: durationS.value }),
    { key: 'add', success: '新建了一集' },
  )
  if (created) {
    await session.refresh()
    openEpisode(created.episode_id)
  }
}

async function episodeAction(episodeId, action) {
  if (action === 'delete' && !confirm(`删掉 ${episodeId}？剧本和分镜一起没。`)) return
  const result = await run(
    () => api.episodeAction({ project: session.projectPath, episode_id: episodeId, action }),
    { key: action },
  )
  if (!result) return
  if (action === 'delete' && openEp.value === episodeId) openEp.value = ''
  if (result.episode_id && action === 'duplicate') openEpisode(result.episode_id)
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
  if (started) {
    savedPremise.value = premise.value.trim()
    writer.start()
  }
}

async function stopSeries() {
  await run(() => api.stopSeries(), { key: 'stopSeries', success: '已停' })
  writer.poll()
}

/** 让后面几步对准这一集。 */
function makeCurrent(episodeId) {
  session.selectEpisode(episodeId)
  ui.ok(`后面几步现在对着 ${episodeId}`)
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
          @click="addEpisode"
        >
          <AppIcon name="plus" :size="15" />
          手动加一集
        </button>
        <button
          class="btn btn--ai"
          type="button"
          :disabled="!session.hasProject || isBusy('write')"
          @click="writeOne('')"
        >
          <AppIcon name="sparkle" :size="15" />
          {{ isBusy('write') ? '大模型正在写…' : 'AI 写新的一集' }}
        </button>
      </template>
    </StepHeader>

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="剧本挂在项目上。先回第一步选一个，或者新建一个。"
    >
      <RouterLink to="/project" class="btn btn--primary">去第一步</RouterLink>
    </EmptyState>

    <template v-else>
      <!-- 全剧梗概 -->
      <section class="card">
        <div class="card__head">
          <div>
            <div class="card__title">这部剧讲什么</div>
            <div class="card__sub">
              一两句话就够。整部剧的调子，每一集都从它出发；角色也是从剧本里提的。
            </div>
          </div>
        </div>
        <div class="card__body stack">
          <textarea
            v-model="premise"
            class="textarea"
            rows="3"
            placeholder="例如：深夜便利店，前任推门进来，手里拿着五年前她送的那把伞。"
            @blur="savePremise"
          />
          <div class="row row--between">
            <span v-if="premiseDirty" class="pill pill--warn">
              还没保存，点开别处会自动存
            </span>
            <span v-else-if="savedPremise" class="tiny dim">
              <AppIcon name="check" :size="12" class="inline-icon" />
              已存到项目上
            </span>
            <span v-else class="tiny dim">还没写</span>
            <button
              class="btn btn--sm"
              type="button"
              :disabled="!premiseDirty || isBusy('premise')"
              @click="savePremise"
            >
              {{ isBusy('premise') ? '保存中…' : '保存' }}
            </button>
          </div>

          <!-- 想不出来就让它想 -->
          <div class="ideabar">
            <input
              v-model="ideaKeywords"
              class="input"
              placeholder="想往哪个方向？例如：职场 / 悬疑 / 婆媳。留空就自由发挥"
              @keyup.enter="suggestPremises"
            />
            <button
              class="btn btn--ai nowrap"
              type="button"
              :disabled="isBusy('ideas')"
              @click="suggestPremises"
            >
              <AppIcon name="wand" :size="15" />
              {{ isBusy('ideas') ? '正在想…' : 'AI 想几个' }}
            </button>
          </div>

          <div v-if="ideas.length" class="ideas">
            <button
              v-for="(idea, i) in ideas"
              :key="i"
              class="idea"
              type="button"
              @click="useIdea(idea)"
            >
              <span class="idea__title">
                {{ idea.title || '未命名' }}
                <span class="idea__use tiny">用这个</span>
              </span>
              <span class="idea__premise">{{ idea.premise }}</span>
              <span v-if="idea.hook" class="idea__hook tiny">
                <AppIcon name="sparkle" :size="12" />
                {{ idea.hook }}
              </span>
            </button>
            <button
              class="btn btn--ghost btn--sm"
              type="button"
              :disabled="isBusy('ideas')"
              @click="suggestPremises"
            >
              <AppIcon name="refresh" :size="14" />
              再想几个
            </button>
          </div>

          <div class="opts">
            <div class="field">
              <span class="field__label">每集目标时长</span>
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
        <div class="card__foot">
          <span class="tiny dim">梗概定下来之后，就能一次写好几集</span>
          <span class="spacer" />
          <label class="field field--inline">
            <span class="field__label nowrap">一口气写</span>
            <input
              v-model.number="seriesCount"
              class="input input--num"
              type="number"
              min="1"
              max="20"
              :disabled="writer.running"
            />
            <span class="field__label">集</span>
          </label>
          <button
            v-if="!writer.running"
            class="btn btn--ai"
            type="button"
            :disabled="isBusy('series')"
            @click="writeSeries"
          >
            <AppIcon name="sparkle" :size="15" />
            批量写整季
          </button>
          <button v-else class="btn btn--danger" type="button" @click="stopSeries">
            <AppIcon name="stop" :size="14" />
            停下
          </button>
        </div>
      </section>

      <!-- 批量进度 -->
      <section v-if="writer.running || writer.state?.episodes?.length" class="card">
        <div class="card__head">
          <div class="card__title">批量写整季</div>
          <span class="card__sub">边写边存，中途能停，已写好的留着。</span>
        </div>
        <div class="card__body stack stack--sm">
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
      </section>

      <!-- AI 草稿 -->
      <Transition name="fold">
        <section v-if="draft" class="card card--draft">
          <div class="card__head">
            <div>
              <div class="card__title">
                <AppIcon name="sparkle" :size="15" class="inline-icon" />
                {{ draft.title || 'AI 写的一集' }}
                <span class="pill" :class="draftFor === TRAILER_ID ? 'pill--info' : 'pill--neutral'">
                  {{
                    draftFor === TRAILER_ID
                      ? '预告片'
                      : draftFor
                        ? `改写 ${draftFor}`
                        : '新的一集'
                  }}
                </span>
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

            <ScriptReader :text="draft.script" />
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
              :disabled="isBusy('write') || isBusy('trailer')"
              @click="draftFor === TRAILER_ID ? writeTrailer() : writeOne(draftFor)"
            >
              <AppIcon name="refresh" :size="15" />
              {{ draftFor === TRAILER_ID ? '让它重剪' : '让它重写' }}
            </button>
            <button class="btn btn--ghost" type="button" @click="draft = null">丢弃</button>
          </div>
        </section>
      </Transition>

      <!-- 预告片 -->
      <section class="card card--trailer">
        <div class="card__head">
          <div>
            <div class="card__title">
              预告片
              <span v-if="trailer" class="pill pill--ok">已有一条</span>
              <span v-else class="pill pill--neutral">还没剪</span>
            </div>
            <div class="card__sub">
              从写好的正片里挑最抓人的瞬间剪成蒙太奇。只给钩子不给答案，
              不剧透结局。往后照样走分镜、制作、成片，跟一集正片一样。
            </div>
          </div>
        </div>
        <div class="card__body stack">
          <div v-if="trailer" class="trailer__now">
            <span class="ep__no mono">{{ TRAILER_ID }}</span>
            <span class="ep__text">
              <span class="ep__title truncate">{{ trailer.title || '未命名预告' }}</span>
              <span class="tiny dim truncate">{{ trailer.synopsis || '没有钩子文案' }}</span>
            </span>
            <span class="spacer" />
            <span class="pill nowrap" :class="trailer.shots ? 'pill--ok' : 'pill--neutral'">
              {{ trailer.shots ? `${trailer.shots} 镜` : '未分镜' }}
            </span>
            <span class="tiny dim numeric nowrap">{{ humanTime(trailer.duration_s) }}</span>
            <button
              v-if="session.episodeId !== TRAILER_ID"
              class="btn btn--sm"
              type="button"
              @click="makeCurrent(TRAILER_ID)"
            >
              拿它做后面几步
            </button>
            <button class="btn btn--ghost btn--sm" type="button" @click="openEpisode(TRAILER_ID)">
              {{ openEp === TRAILER_ID ? '收起' : '看剧本' }}
            </button>
          </div>

          <div v-if="openEp === TRAILER_ID" class="stack stack--sm">
            <ScriptReader :text="script" />
          </div>

          <div class="field">
            <span class="field__label">预告时长</span>
            <div class="chips">
              <button
                v-for="d in TRAILER_DURATIONS"
                :key="d"
                class="chip"
                :class="{ 'chip--on': trailerDurationS === d }"
                type="button"
                @click="trailerDurationS = d"
              >
                {{ d }} 秒
              </button>
            </div>
            <span class="field__hint">
              短平台的预告 15 到 20 秒最常见。写长了刷到第三秒还没看到钩子，人就划走了。
            </span>
          </div>
        </div>
        <div class="card__foot">
          <button
            class="btn btn--ai"
            type="button"
            :disabled="!writtenCount || isBusy('trailer')"
            @click="writeTrailer"
          >
            <AppIcon name="sparkle" :size="15" />
            {{
              isBusy('trailer')
                ? '正在挑素材…'
                : trailer
                  ? 'AI 重剪预告片'
                  : 'AI 剪一条预告片'
            }}
          </button>
          <span v-if="!writtenCount" class="tiny dim">
            先写至少一集正片，预告是从正片里剪出来的
          </span>
          <span v-else class="tiny dim">
            素材来自已写好的 {{ writtenCount }} 集
          </span>
        </div>
      </section>

      <!-- 分集列表 -->
      <section class="stack">
        <div class="row row--between">
          <div class="row">
            <h2 class="section-title">分集</h2>
            <span class="pill pill--neutral">{{ episodes.length }} 集</span>
            <span v-if="episodes.length" class="tiny dim numeric">
              已写 {{ writtenCount }} 集 · 计划共 {{ humanTime(totalDuration) }}
            </span>
          </div>
        </div>

        <EmptyState
          v-if="!episodes.length"
          icon="script"
          title="还一集都没有"
          hint="写一句梗概，然后用「AI 写新的一集」出第一集，或者「批量写整季」一次出好几集。"
        >
          <button
            class="btn btn--ai"
            type="button"
            :disabled="isBusy('write')"
            @click="writeOne('')"
          >
            <AppIcon name="sparkle" :size="15" />
            写第一集
          </button>
        </EmptyState>

        <div v-else class="stack stack--sm">
          <article
            v-for="ep in episodes"
            :key="ep.episode_id"
            class="ep card"
            :class="{
              'ep--open': openEp === ep.episode_id,
              'ep--current': session.episodeId === ep.episode_id,
            }"
            :data-ep="ep.episode_id"
          >
            <button class="ep__head" type="button" @click="openEpisode(ep.episode_id)">
              <span class="ep__no mono">{{ ep.episode_id }}</span>
              <span class="ep__text">
                <span class="ep__title truncate">{{ ep.title || '未命名' }}</span>
                <span class="ep__logline truncate tiny dim">
                  {{ ep.synopsis || '还没有梗概' }}
                </span>
              </span>
              <span class="spacer" />
              <span
                v-if="session.episodeId === ep.episode_id"
                class="pill pill--accent nowrap"
              >
                后面几步对着它
              </span>
              <span class="pill nowrap" :class="ep.shots ? 'pill--ok' : 'pill--neutral'">
                {{ ep.shots ? `${ep.shots} 镜` : '未分镜' }}
              </span>
              <span class="tiny dim numeric nowrap">{{ humanTime(ep.duration_s) }}</span>
              <AppIcon
                class="ep__chev"
                :name="openEp === ep.episode_id ? 'arrowLeft' : 'arrowRight'"
                :size="15"
              />
            </button>

            <div v-if="openEp === ep.episode_id" class="ep__body">
              <div class="row row--between">
                <div class="chips">
                  <button
                    v-for="m in [
                      { v: 'read', l: '阅读' },
                      { v: 'edit', l: '编辑' },
                    ]"
                    :key="m.v"
                    class="chip"
                    :class="{ 'chip--on': mode === m.v }"
                    type="button"
                    @click="mode = m.v"
                  >
                    {{ m.l }}
                  </button>
                </div>
                <span class="small dim numeric">{{ wordCount }} 字</span>
              </div>

              <ScriptReader v-if="mode === 'read'" :text="script" />
              <textarea
                v-else
                v-model="script"
                class="textarea textarea--script"
                :placeholder="
                  scriptLoading
                    ? '读取中…'
                    : '还没有剧本。用下面的「让 AI 写这一集」，或者直接在这里写。'
                "
                spellcheck="false"
              />

              <p class="field__hint">
                格式是定死的：对白写成「名字：台词」，动作单独一行。下一步认角色靠它。
              </p>

              <div class="ep__foot">
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
                <button
                  class="btn btn--ai"
                  type="button"
                  :disabled="isBusy('write')"
                  @click="writeOne(ep.episode_id)"
                >
                  <AppIcon name="sparkle" :size="14" />
                  让 AI 写这一集
                </button>
                <span class="spacer" />
                <button
                  v-if="session.episodeId !== ep.episode_id"
                  class="btn btn--sm"
                  type="button"
                  @click="makeCurrent(ep.episode_id)"
                >
                  拿它做后面几步
                </button>
                <button
                  class="btn btn--ghost btn--sm"
                  type="button"
                  @click="episodeAction(ep.episode_id, 'duplicate')"
                >
                  复制
                </button>
                <button
                  v-if="episodes.length > 1"
                  class="btn btn--ghost btn--sm"
                  type="button"
                  @click="episodeAction(ep.episode_id, 'delete')"
                >
                  <AppIcon name="trash" :size="14" />
                </button>
              </div>
            </div>
          </article>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.section-title {
  font-size: var(--fs-lg);
  font-weight: 600;
}
.inline-icon {
  display: inline-block;
  vertical-align: -2px;
  color: var(--accent);
}

.ideabar {
  display: flex;
  gap: var(--s2);
}

.ideas {
  display: flex;
  flex-direction: column;
  gap: var(--s2);
}
.idea {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: var(--s3) var(--s4);
  border-radius: var(--r);
  border: 1px solid var(--line);
  background: var(--bg-sunken);
  text-align: left;
  cursor: pointer;
  transition: border-color 0.14s var(--ease), background 0.14s var(--ease);
}
.idea:hover {
  border-color: var(--accent-line);
  background: var(--accent-soft);
}
.idea__title {
  display: flex;
  align-items: center;
  gap: var(--s2);
  font-size: var(--fs-md);
  font-weight: 600;
}
.idea__use {
  padding: 0 6px;
  border-radius: var(--r-pill);
  background: var(--surface-3);
  color: var(--text-3);
  font-weight: 500;
  opacity: 0;
  transition: opacity 0.14s var(--ease);
}
.idea:hover .idea__use {
  opacity: 1;
  background: var(--accent);
  color: var(--accent-text);
}
.idea__premise {
  font-size: var(--fs-base);
  color: var(--text-2);
  line-height: 1.7;
}
.idea__hook {
  display: flex;
  align-items: center;
  gap: 5px;
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
  gap: var(--s2);
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
  gap: var(--s2);
}
.input--num {
  width: 66px;
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

.card--trailer {
  border-color: color-mix(in srgb, var(--info) 30%, transparent);
}
.trailer__now {
  display: flex;
  align-items: center;
  gap: var(--s3);
  padding: var(--s3);
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
}

/* ---------- 分集 ---------- */

.ep {
  overflow: hidden;
}
.ep--open {
  border-color: var(--accent-line);
}
.ep--current {
  border-left: 3px solid var(--accent);
}
.ep__head {
  display: flex;
  align-items: center;
  gap: var(--s3);
  width: 100%;
  padding: var(--s3) var(--s4);
  background: none;
  border: none;
  cursor: pointer;
  text-align: left;
}
.ep__head:hover {
  background: var(--surface-2);
}
.ep__no {
  flex: none;
  padding: 2px var(--s2);
  border-radius: var(--r-sm);
  background: var(--surface-3);
  color: var(--text-2);
  font-size: var(--fs-sm);
  font-weight: 600;
}
.ep__text {
  display: flex;
  flex-direction: column;
  min-width: 0;
  line-height: 1.3;
}
.ep__title {
  font-size: var(--fs-md);
  font-weight: 600;
}
.ep__logline {
  max-width: 52ch;
}
.ep__chev {
  color: var(--text-3);
}

.ep__body {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
  border-top: 1px solid var(--line);
  padding: var(--s4) var(--s5) var(--s5);
  background: var(--surface-2);
}
/* 保存条钉在视口底部。整集剧本比屏幕长得多，保存按钮在最下面，
   改完开头几句得先滚到底才找得到。 */
.ep__foot {
  position: sticky;
  bottom: 0;
  z-index: 4;
  display: flex;
  align-items: center;
  gap: var(--s2);
  flex-wrap: wrap;
  margin: 0 calc(var(--s5) * -1) calc(var(--s5) * -1);
  padding: var(--s3) var(--s5);
  border-top: 1px solid var(--line);
  background: color-mix(in srgb, var(--surface-2) 94%, transparent);
  backdrop-filter: blur(8px);
}

.textarea--script {
  min-height: 300px;
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

@media (max-width: 720px) {
  .ep__logline {
    display: none;
  }
  .ep__head .pill:not(.pill--accent) {
    display: none;
  }
  .ep__body {
    padding: var(--s3);
  }
}
</style>
