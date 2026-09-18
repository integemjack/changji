<script setup>
/**
 * 成片。整部电影的最后一步：出了片的章按顺序接成**一部完整的电影**，一个文件。
 *
 * 2026-09-17 这一页曾经能「接成一条，再选每段多长、在镜头边界上切成几段」，
 * 切出来的落在盘上叫「第01集.mp4」这种名字——老项目里还留着，见下面 legacy_cut。
 * 2026-09-18 用户把产品定位改成电影制作平台：成片就是一部电影、一个文件，
 * 那排「1 分钟 / 3 分钟 / 10 分钟 / 60 分钟 / 整部一集」和它背后的整条切段链
 * 连根拔掉了（引擎那头是 media::split_into_episodes、pipeline/series_cut）。
 *
 * 顶栏上这一格有一章出了片就出现（App.vue visibleSteps 看
 * counters.filmedChapters）——用户 2026-09-18：「这一章有片就可以成片了」。
 * 没片的章跳过，出了再合成一次；这儿说清这次接的是哪几章。
 *
 * 合成在出片那个槽上跑（JobKind::Run），进度走 run store；跑完看的是
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

/** /api/film 的回包：那部电影，和上一次合成接了哪几章。 */
const film = ref(null)
const loadError = ref('')
/** 正在放的那个文件（rel）。 */
const playing = ref(null)

const files = computed(() => film.value?.files ?? [])
/**
 * 那部电影是回包里 `name` 那一条——**不是 `files[0]`**（口径写在 http/film.hpp）。
 *
 * `output/final` 是个目录，人往里放别的 mp4 不该让这一页打不开，所以回包给的
 * 是整个目录。而这个列表按名字排，「成片」的首字节（0xE6）比任何 ASCII 名字
 * 都大——目录里多一个 raw.mp4，`files[0]` 就是那个杂文件，标题、体积、播放器
 * 一起指错。
 *
 * `name` 是 null 就是**不知道哪个是成片**（清单读不到，或者盘上是上一版切出来
 * 的几段）。这时不挑一个冒充：把目录里的文件整个列出来，人点哪个放哪个。
 */
const movie = computed(() => {
  const want = film.value?.name
  return (want && files.value.find((f) => f.name === want)) || null
})
/** 不知道哪个是成片：那就整个列出来。 */
const listAll = computed(() => !movie.value && files.value.length > 0)
const stem = (name) => name.replace(/\.mp4$/, '')
const filmed = computed(() => Number(session.counters.filmedChapters ?? 0))
const chapters = computed(
  () => session.episodes.filter((e) => (e.chapter_refs ?? []).length).length,
)
const allFilmed = computed(() => !!session.counters.allFilmed)
/** 有一章出了片就能合成。 */
const canJoin = computed(() => filmed.value > 0)

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
  } catch (err) {
    if (want !== session.projectPath) return
    film.value = null
    loadError.value = err?.message || '读不出来'
  }
}

/** 合成。会把上一版清掉——它是同一部电影的上一次合成，不是别的东西。 */
async function join() {
  const project = session.projectPath
  if (files.value.length && !confirm('会把现在这一版清掉，重新合成。确定？')) return
  const started = await run(() => api.joinFilm({ project }), { key: 'join' })
  if (!started) return
  ui.ok('开始合成')
  runner.start()
}

async function stop() {
  await run(() => api.stopRun(), { key: 'joinstop', quiet: true })
}

/**
 * 秒 → 人话。**没量到就说「—」，不写「0 秒」。**
 *
 * `total_s` / `duration_s` 是**真的会没有**的，而且不止一种没有：清单读不到
 * （合成被 kill 在写清单之前、老项目的 cut.json 被删过），或者合成成功而
 * ffprobe 没给出时长（引擎那头写进 film.json 的就是 null，见 http/film.hpp
 * 和 pipeline/film_join）。原来这儿是 `Math.round(s ?? 0)`，于是「不知道」
 * 一律说成「0 秒」——电影好好地躺在 output/final 里，页面却在替引擎报一个
 * 没人量过的数，人看见 0 会以为电影合坏了。
 */
function fmt(s) {
  if (typeof s !== 'number' || !Number.isFinite(s)) return '—'
  const n = Math.round(s)
  return n >= 60 ? `${Math.floor(n / 60)} 分 ${String(n % 60).padStart(2, '0')} 秒` : `${n} 秒`
}

// 进页面 / 换项目 / 刚合成完：挂上要放的那个。**优先那部电影**——files[0]
// 不一定是它。不知道哪个是成片时才退回第一条，那种情况下整个列表都摆出来，
// 人点哪个放哪个（不挂的话播放器永远是空的）。
watch(film, () => {
  playing.value = (movie.value ?? files.value[0])?.rel ?? null
}, { immediate: true })

onMounted(load)
watch(() => session.projectPath, load)
// 合成完了：这一页和顶栏的判据一起重拉——这一格打勾看的是成片在不在
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
      title="读不到这部电影的成片"
      :hint="loadError"
    />

    <template v-else>
      <div class="film__bar">
        <button
          class="btn btn--ai btn--sm"
          type="button"
          :disabled="runner.running || isBusy('join') || !canJoin"
          :title="
            !canJoin
              ? '还没有一章出片。这一章出了片就能合成'
              : allFilmed
                ? '把出了片的章按顺序接成一部完整的电影'
                : `出了片的 ${filmed} 章接成一部电影；还有 ${chapters - filmed} 章没出片，出了再合成一次`
          "
          @click="join"
        >
          <AppIcon name="film" :size="13" />
          {{ runner.running ? '正在合成…' : files.length ? '重新合成' : '合成整部电影' }}
        </button>
        <span v-if="canJoin" class="small dim">
          出了片的 {{ filmed }} 章接成一部完整的电影<template v-if="chapters > filmed">；还有 {{ chapters - filmed }} 章没出片，出了再合成一次</template>
        </span>
        <template v-if="runner.running">
          <span class="tiny dim">
            {{ runner.state?.done ?? 0 }}/{{ runner.state?.total ?? 0 }} ·
            {{ runner.state?.message }}
          </span>
          <button class="btn btn--sm btn--ghost" type="button" @click="stop">停下</button>
        </template>
      </div>

      <EmptyState
        v-if="!canJoin"
        icon="film"
        title="还没有一章出片"
        hint="去「这一章」把片出来。有一章出了片，这儿就能合成"
      />
      <EmptyState
        v-else-if="!files.length"
        icon="film"
        title="还没合成过"
        hint="点「合成整部电影」，出了片的章按顺序接成一部完整的电影"
      />
      <template v-else>
        <!-- 盘上是上一版切出来的那几段（老项目的 cut.json），不是一部电影。
             不说的话，页面会拿这几段的总时长配上第一段的名字和体积，读起来
             像"这就是那部电影"，而另外几段没有任何入口。 -->
        <div v-if="film.legacy_cut" class="small warn-text">
          这 {{ files.length }} 个文件是上一版按时长切出来的几段（盘上叫「第01集.mp4」这种名字），不是一部完整的电影。
          点「重新合成」把出了片的章接成一部。
        </div>
        <div v-else-if="!movie" class="small warn-text">
          读不到这一版的合成清单，下面是 output/final 里的文件。时长和镜数无从得知，重新合成一次就有了。
        </div>

        <div class="small dim">
          <template v-if="movie">
            {{ stem(movie.name) }} · {{ fmt(film.total_s) }} ·
            {{ film.shots ?? '—' }} 镜 · {{ movie.size_mb }} MB
          </template>
          <template v-else>
            {{ files.length }} 个文件 · 共 {{ fmt(film.total_s) }} · {{ film.shots ?? '—' }} 镜
          </template>
          <template v-if="film.skipped?.length">
            · 接的是出了片的 {{ film.chapters?.length ?? 0 }} 章，{{ film.skipped.length }} 章还没出片
          </template>
        </div>

        <!-- 不知道哪个是成片：每一个都点得开。挑一个挂上播放器、其余的连入口
             都没有，等于替人做了一个自己都没把握的决定。 -->
        <div v-if="listAll" class="film__files">
          <button
            v-for="f in files"
            :key="f.rel"
            type="button"
            class="btn btn--sm"
            :class="f.rel === playing ? 'btn--primary' : 'btn--ghost'"
            @click="playing = f.rel"
          >
            {{ stem(f.name) }} · {{ fmt(f.duration_s) }} · {{ f.shots ?? '—' }} 镜 · {{ f.size_mb }} MB
          </button>
        </div>

        <div class="player">
          <video v-if="playing" :key="playing" :src="mediaUrl(session.projectPath, playing)" controls />
          <EmptyState v-else icon="play" title="这一版的文件不见了" />
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
.film__files {
  display: flex;
  flex-wrap: wrap;
  gap: var(--s2);
}
.player video {
  width: 100%;
  max-height: 70vh;
  background: #000;
  border-radius: var(--r);
}
</style>
