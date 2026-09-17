<script setup>
/**
 * 成片。整部剧的最后一步：出了片的章接成一条，选每集多长，切成几集。
 *
 * 用户 2026-09-17：「剧本不应该有集的概念，集是最后用户选择多长时间为一集，
 * 可以是 1 分钟，3 分钟，10 分钟，60 分钟，全部为一集那是用户的事情」。
 * 所以"集"整个应用里只在这一页出现：写作按章、出片按章，这儿把所有章接成
 * 一条、在镜头边界上按时长切。换个时长再切一次，前面什么都不用重跑。
 *
 * 顶栏上这一格有一章出了片就出现（App.vue visibleSteps 看
 * counters.filmedChapters）——用户 2026-09-18：「这一章有片就可以成片了」。
 * 没片的章跳过，出了再切一次；这儿说清这次切的是哪几章。
 *
 * 切在出片那个槽上跑（JobKind::Run），进度走 run store；跑完看的是
 * useLongRunning 那张表，不盯 runner.running（undriven-stores 那条规矩）。
 */
import { computed, onMounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { useAction } from '@/composables/useAction'
import { useLongRunning } from '@/composables/useSystemFeed'
import { pickProjectHint } from '@/composables/pick-project-hint'
import { useProjects } from '@/stores/projects'
import { useRun } from '@/stores/run'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const projects = useProjects()
const ui = useUi()
const runner = useRun()
const { run, isBusy } = useAction()
const longRunning = useLongRunning()

/** 每集多长。0 = 整部一集。 */
const CHOICES = [
  { s: 60, label: '1 分钟' },
  { s: 180, label: '3 分钟' },
  { s: 600, label: '10 分钟' },
  { s: 3600, label: '60 分钟' },
  { s: 0, label: '整部一集' },
]
const per = ref(180)
/** /api/film 的回包：切出来的几集和上一次按什么切的。 */
const film = ref(null)
const loadError = ref('')
/** 正在放的那一集（rel）。 */
const playing = ref(null)

const files = computed(() => film.value?.files ?? [])
const filmed = computed(() => Number(session.counters.filmedChapters ?? 0))
const chapters = computed(
  () => session.episodes.filter((e) => (e.chapter_refs ?? []).length).length,
)
const allFilmed = computed(() => !!session.counters.allFilmed)
/** 有一章出了片就能切。 */
const canCut = computed(() => filmed.value > 0)
const perLabel = (s) => CHOICES.find((c) => c.s === s)?.label ?? `${s} 秒`

async function load() {
  if (!session.projectPath) {
    film.value = null
    return
  }
  const want = session.projectPath
  try {
    const got = await api.film(want)
    if (want !== session.projectPath) return
    film.value = got
    loadError.value = ''
    // 上一次按什么切的，这次默认还是它
    if (got.files?.length && typeof got.per_episode_s === 'number') per.value = got.per_episode_s
  } catch (err) {
    if (want !== session.projectPath) return
    film.value = null
    loadError.value = err?.message || '读不出来'
  }
}

/** 切。会清掉上一次切出来的几集——它们是同一部片子的另一种切法，不是别的东西。 */
async function cut() {
  const project = session.projectPath
  const label = perLabel(per.value)
  if (
    files.value.length &&
    !confirm(`会把现在这 ${files.value.length} 集清掉，按「${label}」重切。确定？`)
  ) {
    return
  }
  const started = await run(
    () => api.cutFilm({ project, per_episode_s: per.value }),
    { key: 'cut' },
  )
  if (!started) return
  ui.ok(`开始切：${label}`)
  runner.start()
}

async function stop() {
  await run(() => api.stopRun(), { key: 'cutstop', quiet: true })
}

function fmt(s) {
  const n = Math.round(s ?? 0)
  return n >= 60 ? `${Math.floor(n / 60)} 分 ${String(n % 60).padStart(2, '0')} 秒` : `${n} 秒`
}
function chapterName(id) {
  const i = session.episodes.findIndex((e) => (e.chapter_refs ?? [])[0] === id)
  const ep = session.episodes[i]
  const n = Number(/^ch(\d+)$/.exec(id || '')?.[1] ?? i + 1)
  return `第 ${n} 章${ep?.title ? ' · ' + ep.title : ''}`
}

onMounted(load)
watch(() => session.projectPath, load)
// 切完了：列表和顶栏的判据一起重拉——这一格打勾看的是切出来的几集在不在
watch(longRunning, (now, before) => {
  if (before === true && now === false) {
    load()
    session.refresh()
  }
})
</script>

<template>
  <div class="film">
    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      :hint="pickProjectHint(projects)"
    />
    <EmptyState
      v-else-if="loadError"
      icon="warn"
      tone="warn"
      title="读不到这部剧的成片"
      :hint="loadError"
    />

    <template v-else>
      <!-- 一行：选多长、切、进度。集这个概念整个应用里只在这一行上。 -->
      <div class="film__bar">
        <span class="small dim">每集多长</span>
        <div class="choices">
          <button
            v-for="c in CHOICES"
            :key="c.s"
            class="choice"
            :class="{ 'is-on': per === c.s }"
            type="button"
            :disabled="runner.running"
            @click="per = c.s"
          >
            {{ c.label }}
          </button>
        </div>
        <button
          class="btn btn--ai btn--sm"
          type="button"
          :disabled="runner.running || isBusy('cut') || !canCut"
          :title="
            !canCut
              ? '还没有一章出片。这一章出了片就能切'
              : allFilmed
                ? '把所有章接成一条，按上面选的时长在镜头边界上切成几集'
                : `出了片的 ${filmed} 章接成一条切；还有 ${chapters - filmed} 章没出片，出了再切一次`
          "
          @click="cut"
        >
          <AppIcon name="film" :size="13" />
          {{ runner.running ? '正在切…' : files.length ? '重切' : '切' }}
        </button>
        <template v-if="runner.running">
          <span class="tiny dim">
            {{ runner.state?.done ?? 0 }}/{{ runner.state?.total ?? 0 }} ·
            {{ runner.state?.message }}
          </span>
          <button class="btn btn--sm btn--ghost" type="button" @click="stop">停下</button>
        </template>
      </div>

      <EmptyState
        v-if="!canCut"
        icon="film"
        title="还没有一章出片"
        hint="去「这一章」把片出来。有一章出了片，这儿就能切"
      />
      <EmptyState
        v-else-if="!files.length"
        icon="film"
        title="还没切过"
        hint="上面选每集多长，点「切」。换个时长再切一次，前面什么都不用重跑"
      />
      <template v-else>
        <div class="small dim">
          共 {{ files.length }} 集 · {{ fmt(film.total_s) }} · 按「{{ perLabel(film.per_episode_s) }}」切的
          <template v-if="film.skipped?.length">
            · 切的是出了片的 {{ film.chapters?.length ?? 0 }} 章，{{ film.skipped.length }} 章还没出片
          </template>
        </div>
        <div class="films">
          <div class="list">
            <button
              v-for="f in files"
              :key="f.name"
              class="item"
              :class="{ 'is-on': playing === f.rel }"
              type="button"
              @click="playing = f.rel"
            >
              <b>{{ f.name.replace(/\.mp4$/, '') }}</b>
              <span class="tiny dim">
                {{ fmt(f.duration_s) }} · {{ f.size_mb }} MB<template v-if="f.from_chapter">
                  · 从{{ chapterName(f.from_chapter) }}起</template>
              </span>
            </button>
          </div>
          <div class="player">
            <video
              v-if="playing"
              :key="playing"
              :src="mediaUrl(session.projectPath, playing)"
              controls
              autoplay
            />
            <EmptyState v-else icon="play" title="左边点一集" />
          </div>
        </div>
      </template>
    </template>
  </div>
</template>

<style scoped>
.film {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
}
.film__bar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--s2);
}
.choices {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
}
.choice {
  padding: 4px 10px;
  border: 1px solid var(--line);
  border-radius: var(--r-sm);
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.choice.is-on {
  background: var(--accent-soft);
  border-color: var(--accent-line);
  color: var(--accent);
}
.choice:disabled {
  opacity: 0.6;
  cursor: default;
}
.films {
  display: grid;
  grid-template-columns: 18rem 1fr;
  gap: var(--s3);
  align-items: start;
}
.list {
  display: grid;
  gap: 4px;
}
.item {
  display: grid;
  gap: 2px;
  padding: 8px 10px;
  border: 1px solid var(--line);
  border-radius: var(--r-sm);
  background: transparent;
  color: var(--text);
  text-align: left;
  cursor: pointer;
}
.item:hover {
  background: var(--surface-2);
}
.item.is-on {
  border-color: var(--accent-line);
  background: var(--accent-soft);
}
.player video {
  width: 100%;
  max-height: 70vh;
  background: #000;
  border-radius: var(--r);
}
@media (max-width: 720px) {
  .films {
    grid-template-columns: 1fr;
  }
}
</style>
