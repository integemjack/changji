<script setup>
/**
 * 第六步：制作。
 *
 * **这一页只有一面镜头墙。**
 *
 * 上一版是四张卡片：运行中、开跑之前（四个开关加预览）、体检、镜头一览。
 * 问题不是"信息多"，是**同一件事分散在四个地方**——某一镜跑到哪了，
 * 要在进度条、事件流、镜头格子之间来回对。
 *
 * 现在每个镜头就是一张牌：缩略图、状态、进度、按钮，全在自己身上。
 * 其余的东西（体检、错误、日志）**只在出问题时出现**，平时一行都不占。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import StepHeader from '@/components/StepHeader.vue'
import { api, mediaUrl } from '@/api'
import { STAGE_LABELS, statusOf } from '@/api/labels'
import { useAction } from '@/composables/useAction'
import { useRun } from '@/stores/run'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const runStore = useRun()
const { run, isBusy } = useAction()

const shots = ref([])
const doctor = ref(null)
const video = ref(null)
/** 点开在看的那一镜。null = 没在看。 */
const playing = ref(null)

const blocked = computed(() => doctor.value && doctor.value.can_run === false)

/**
 * 这一镜跑到百分之几。**没在跑、或者引擎没给这一镜的步数就返回 null**，
 * 由模板决定画走马灯还是画具体进度。
 *
 * 别用 inflight 里的 step/total：那是整集的位置（第 21 镜 / 共 22 镜）。
 */
function pct(shotId) {
  const x = inflightBy.value[shotId]
  if (!x || typeof x.shotStep !== 'number' || !x.shotSteps) return null
  return Math.min(100, Math.round((x.shotStep / x.shotSteps) * 100))
}

/** 还没出片的镜头数。0 就是这一集做完了。 */
const pending = computed(
  () => shots.value.filter((s) => !s.video_path).length,
)

/** 正在跑的那几镜，按 shot_id 索引，牌子上直接取。 */
const inflightBy = computed(() => {
  const m = {}
  for (const x of runStore.inflight) m[x.shot_id] = x
  return m
})

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

async function loadVideo() {
  if (!session.projectPath) return
  try {
    video.value = await api.projectVideo(session.projectPath)
  } catch {
    video.value = null
  }
}

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

watch(
  () => [session.projectPath, session.episodeId],
  () => {
    loadShots()
    loadVideo()
  },
  { immediate: true },
)

/**
 * 跑起来的时候定期重读分镜。
 *
 * 镜头墙是这一页唯一能看见「片子长什么样」的地方。不刷的话它停在开跑
 * 那一刻，几十分钟里画面一动不动——用户没法判断出来的东西对不对，
 * 只能等全跑完才发现方向就错了。
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

watch(
  () => runStore.running,
  (now, before) => {
    if (before && !now) {
      loadShots()
      session.refresh()
      if (runStore.state?.error) ui.error(runStore.state.error)
      else ui.ok('这一轮跑完了')
    }
  },
)

onMounted(() => {
  loadDoctor()
  runStore.start()
})
onUnmounted(() => {
  runStore.stop()
  watchShots(false)
})

/** 开跑。`ids` 为空是整集，非空是只跑那几镜（镜头牌上的「重新生成」）。 */
async function start(ids = []) {
  const one = ids.length > 0
  const started = await run(
    () =>
      api.run({
        project: session.projectPath,
        episode_id: session.episodeId,
        // 重跑单镜时**一定要带 force**：那一镜已经是完成状态，
        // 不带的话它不在待办里，跑完什么都没变而且不报错。
        force: one,
        ...(one ? { shot_ids: ids } : {}),
      }),
    { key: one ? `re:${ids[0]}` : 'start' },
  )
  if (started) runStore.start()
}

/** 副标题里直说这一集会出多大的画面——按下去之前该知道。 */
const tagline = computed(() => {
  const v = video.value
  if (!v) return '配音、首帧、成片，一步跑完'
  const o = v.orientation === 'landscape' ? '横屏' : '竖屏'
  const q = v.quality === '2k' ? '2K' : '720p'
  return `${o} ${q} · ${v.width}×${v.height}`
})

async function stop() {
  await run(() => api.stopRun(), { key: 'stop', success: '已停，跑完的镜头留着' })
  runStore.poll()
}
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader title="制作" :tagline="tagline">
      <template #actions>
        <button
          v-if="runStore.running"
          class="btn btn--ghost"
          type="button"
          :disabled="isBusy('stop')"
          @click="stop"
        >
          <AppIcon name="close" :size="15" />
          停下
        </button>
        <button
          v-else
          class="btn btn--primary"
          type="button"
          :disabled="!shots.length || blocked || isBusy('start')"
          @click="start()"
        >
          <AppIcon name="film" :size="15" />
          {{ pending ? `出片（还差 ${pending} 镜）` : '全部重出' }}
        </button>
      </template>
    </StepHeader>

    <!-- **只在拦路时出现。** 全绿的时候一行都不占——
         体检的细节在设置页，这里只管"能不能开工"。 -->
    <section v-if="blocked" class="card card--bad">
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

    <!-- 出错的镜头挑出来放最前面，不用在墙上找 -->
    <p v-if="runStore.state?.error" class="alert alert--bad">
      <AppIcon name="warn" :size="15" />
      {{ runStore.state.error }}
    </p>

    <EmptyState
      v-if="!shots.length"
      icon="board"
      title="这一集还没有分镜"
      hint="制作是照着分镜表做的。先回上一步把这一集拆成镜头。"
    />

    <!-- 镜头墙。**这一页的全部内容。** -->
    <div v-else class="wall">
      <article
        v-for="s in shots"
        :key="s.shot_id"
        class="shot"
        :class="[
          `shot--${statusOf(s.status).tone}`,
          { 'shot--live': inflightBy[s.shot_id] },
        ]"
      >
        <button
          class="shot__frame"
          type="button"
          :disabled="!s.video_path"
          :title="s.video_path ? '点开看这一镜' : statusOf(s.status).label"
          @click="s.video_path && (playing = s)"
        >
          <img
            v-if="s.frame_path"
            :src="mediaUrl(session.projectPath, s.frame_path)"
            :alt="s.visual_desc"
            loading="lazy"
          />
          <AppIcon v-else name="image" :size="18" class="shot__blank" />

          <!-- 出好的才有播放标；没出的不给，免得点了没反应 -->
          <span v-if="s.video_path" class="shot__play">
            <AppIcon name="film" :size="16" />
          </span>

          <!-- **进度画在镜头上。** 这一镜跑到哪了，看它自己就够，
               不用去别处对。 -->
          <span v-if="inflightBy[s.shot_id]" class="shot__live">
            <!-- **用 shotStep 不是 step。** step/total 是整集的位置
                 （第 21 镜 / 共 22 镜）——拿它画单镜的条，正在跑的那一镜
                 一出现就是 95%，六步走完还是 95%。一条不动而且是错的
                 进度条比没有更糟。
                 引擎没给这一镜的步数时（"准备中"那几条、老引擎）画走马灯，
                 别硬凑一个百分比。 -->
            <span
              v-if="pct(s.shot_id) !== null"
              class="shot__bar"
              :style="{ width: pct(s.shot_id) + '%' }"
            />
            <span v-else class="shot__bar shot__bar--idle" />
          </span>
        </button>

        <div class="shot__bottom">
          <span class="shot__no numeric">{{ s.order + 1 }}</span>
          <span class="shot__state tiny">
            {{
              inflightBy[s.shot_id]
                ? STAGE_LABELS[inflightBy[s.shot_id].stage] || inflightBy[s.shot_id].stage
                : statusOf(s.status).label
            }}
          </span>
          <span class="spacer" />
          <button
            class="iconbtn"
            type="button"
            title="重新生成这一镜"
            :disabled="runStore.running || isBusy(`re:${s.shot_id}`)"
            @click="start([s.shot_id])"
          >
            <AppIcon name="refresh" :size="14" />
          </button>
        </div>
      </article>
    </div>

    <!-- 看片。点墙上任意一格出好的镜头 -->
    <div v-if="playing" class="viewer" @click.self="playing = null">
      <div class="viewer__box">
        <video
          :src="mediaUrl(session.projectPath, playing.video_path)"
          controls
          autoplay
          class="viewer__video"
        />
        <div class="viewer__foot">
          <span class="numeric">{{ playing.order + 1 }}</span>
          <span class="truncate muted small">{{ playing.visual_desc }}</span>
          <span class="spacer" />
          <button
            class="btn btn--ghost btn--sm"
            type="button"
            :disabled="runStore.running"
            @click="start([playing.shot_id])"
          >
            <AppIcon name="refresh" :size="14" />
            重新生成
          </button>
          <button class="btn btn--ghost btn--sm" type="button" @click="playing = null">
            关掉
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.wall {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(148px, 1fr));
  gap: var(--s3);
}

.shot {
  border: 1px solid var(--line);
  border-radius: var(--r);
  overflow: hidden;
  background: var(--surface);
}
.shot--live {
  border-color: var(--accent-line);
}
.shot--bad {
  border-color: color-mix(in srgb, var(--danger) 45%, transparent);
}

.shot__frame {
  position: relative;
  display: block;
  width: 100%;
  aspect-ratio: 9 / 16;
  padding: 0;
  border: none;
  background: var(--bg-sunken);
  color: var(--text-3);
  cursor: pointer;
}
.shot__frame:disabled {
  cursor: default;
}
.shot__frame img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: block;
}
.shot__blank {
  position: absolute;
  inset: 0;
  margin: auto;
}

/* 播放标只在能播的时候出现。悬停才显形，免得盖住画面 */
.shot__play {
  position: absolute;
  inset: 0;
  display: grid;
  place-items: center;
  background: color-mix(in srgb, black 35%, transparent);
  color: #fff;
  opacity: 0;
  transition: opacity 0.12s;
}
.shot__frame:hover .shot__play {
  opacity: 1;
}

/* 进度条贴在缩略图底边。**画在镜头上**，不去别处看 */
.shot__live {
  position: absolute;
  left: 0;
  right: 0;
  bottom: 0;
  height: 3px;
  background: color-mix(in srgb, var(--accent) 25%, transparent);
  /* 走马灯靠 translateX 走出去，不裁的话会画到牌子外面 */
  overflow: hidden;
}
.shot__bar {
  display: block;
  height: 100%;
  background: var(--accent);
  transition: width 0.3s;
}
/* 不知道跑到哪一步时的走马灯。**别停着不动**——静止的进度条和
   "卡死了"看起来一模一样，而这一步（搬权重、VAE 解码）本来就要几十秒。 */
.shot__bar--idle {
  width: 35%;
  animation: shot-slide 1.4s ease-in-out infinite;
}
@keyframes shot-slide {
  0% { transform: translateX(-100%); }
  100% { transform: translateX(286%); }
}
@media (prefers-reduced-motion: reduce) {
  .shot__bar--idle { animation: none; width: 100%; opacity: 0.5; }
}

.shot__bottom {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 6px;
  border-top: 1px solid var(--line);
}
.shot__no {
  color: var(--text-3);
  font-size: var(--fs-sm);
}
.shot__state {
  color: var(--text-2);
}

.iconbtn {
  display: grid;
  place-items: center;
  width: 24px;
  height: 24px;
  border: none;
  border-radius: 6px;
  background: none;
  color: var(--text-3);
  cursor: pointer;
}
.iconbtn:hover:not(:disabled) {
  background: var(--surface-3);
  color: var(--text);
}
.iconbtn:disabled {
  opacity: 0.35;
  cursor: default;
}

.viewer {
  position: fixed;
  inset: 0;
  z-index: 50;
  display: grid;
  place-items: center;
  padding: var(--s5);
  background: color-mix(in srgb, black 70%, transparent);
}
.viewer__box {
  display: flex;
  flex-direction: column;
  gap: var(--s2);
  max-width: min(92vw, 520px);
}
.viewer__video {
  width: 100%;
  max-height: 78vh;
  border-radius: var(--r);
  background: #000;
}
.viewer__foot {
  display: flex;
  align-items: center;
  gap: var(--s2);
}
</style>
