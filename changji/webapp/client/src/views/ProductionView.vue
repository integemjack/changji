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
const blocked = computed(() => doctor.value && doctor.value.can_run === false)

/**
 * 播放器的换代号。跑完一轮加一，拼进 `src` 里。
 *
 * **重出一镜之后文件路径一个字都没变**（还是 shots/final/ep01_sh002.mp4），
 * 浏览器于是把缓存里那份旧的接着放——用户点了「重新生成」、等了两分钟、
 * 看到的还是原来那段，而且没有任何东西提示他看的是旧的。
 *
 * 只在跑完时加一，不是每次刷新都加：跑的过程中加会把正在看的那一镜
 * 从头打断。`preload="none"` 让这次换代几乎不花钱——没点播放的那些
 * 一个字节都不会重下。
 */
const bust = ref(0)

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

/**
 * 刚点过「重新生成」、但引擎还没报出第一条进度的那几镜。
 *
 * **点下去到第一条进度之间能隔一分钟**——那段时间引擎在载模型，
 * 一个字都不会报。这期间牌子上显示的还是上一轮的"成片完成"：
 * 用户点了按钮，画面一动不动，只能再点一次。
 *
 * 进：点「重新生成」并且接口回了 started。
 * 出：这一镜的第一条进度到了（换成真进度），或者整轮跑完了。
 */
const queued = ref(new Set())

/** 这一镜正在被处理——不管是引擎已经报了进度，还是刚点完还在等。 */
function busy(shotId) {
  return Boolean(inflightBy.value[shotId]) || queued.value.has(shotId)
}

// 引擎开始报这一镜了，"排队中"就该让位给真进度。
watch(inflightBy, (now) => {
  if (!queued.value.size) return
  const next = new Set(queued.value)
  for (const id of Object.keys(now)) next.delete(id)
  if (next.size !== queued.value.size) queued.value = next
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
      // 跑完了就没有"排队中"了。不清的话那几格会一直挂着，
      // 而它们其实已经跑完（或者失败）了。
      queued.value = new Set()
      // 刚跑完，磁盘上那几个 mp4 换过了但路径没变。见 bust 的注释。
      bust.value += 1
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
  if (!started) return
  // **先把牌子点亮，别等引擎。** 见 queued 的注释：载模型那一分钟里
  // 引擎一个字都不报，不先点亮的话用户看到的是"点了没反应"。
  if (one) queued.value = new Set([...queued.value, ...ids])
  runStore.start()
}

/**
 * 每张牌的画幅。**跟着项目的 [video] 走，不是写死竖屏。**
 *
 * 牌子里现在是真的播放器，`object-fit: contain`——槽的比例和片子对不上
 * 就会留黑边。写死 9:16 的话，横屏项目的每张牌都是上下两条黑、
 * 中间一小条画面，一屏看不了几镜。
 *
 * 读不到就退回竖屏：这是短剧工具，竖屏是常态。
 */
const cellRatio = computed(() => {
  const v = video.value
  if (!v?.width || !v?.height) return '9 / 16'
  return `${v.width} / ${v.height}`
})

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
    <div v-else class="wall" :style="{ '--cell-ratio': cellRatio }">
      <article
        v-for="s in shots"
        :key="s.shot_id"
        class="shot"
        :class="[
          `shot--${statusOf(s.status).tone}`,
          { 'shot--live': busy(s.shot_id) },
        ]"
      >
        <div class="shot__frame">
          <!-- **出好的镜头直接就是播放器，不用点开。**
               原来是点一格弹一个浮层。一集二十二镜要一格格点开再关掉，
               而看片子这件事恰恰要来回比对相邻两镜接不接得上——浮层
               每次只给看一镜，正好把这件事挡住了。

               `preload="none"` + `poster`：不点播放就一个字节都不下，
               所以二十二个播放器和二十二张缩略图一样轻。海报用的就是
               这一镜的首帧，画面和以前一模一样。
               `playsinline` 是给手机的，不加会被系统全屏播放器接管。 -->
          <video
            v-if="s.video_path"
            :key="s.shot_id + ':' + bust"
            class="shot__video"
            :src="mediaUrl(session.projectPath, s.video_path) + '&_=' + bust"
            :poster="s.frame_path ? mediaUrl(session.projectPath, s.frame_path) : undefined"
            controls
            playsinline
            preload="none"
          />
          <img
            v-else-if="s.frame_path"
            :src="mediaUrl(session.projectPath, s.frame_path)"
            :alt="s.visual_desc"
            loading="lazy"
          />
          <AppIcon v-else name="image" :size="18" class="shot__blank" />

          <!-- **进度画在镜头上。** 这一镜跑到哪了，看它自己就够，
               不用去别处对。 -->
          <span v-if="busy(s.shot_id)" class="shot__live">
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
        </div>

        <div class="shot__bottom">
          <span class="shot__no numeric">{{ s.order + 1 }}</span>
          <!-- 说明也跟着走：正在跑就报阶段，刚点完还没轮到就说"排队中"。
               不这么分的话，那一分钟里显示的是上一轮的"成片完成"——
               和"没点上"看起来一模一样。 -->
          <span class="shot__state tiny">
            {{
              inflightBy[s.shot_id]
                ? STAGE_LABELS[inflightBy[s.shot_id].stage] || inflightBy[s.shot_id].stage
                : queued.has(s.shot_id)
                  ? '排队中'
                  : statusOf(s.status).label
            }}
          </span>
          <span class="spacer" />
          <button
            class="iconbtn"
            type="button"
            :title="busy(s.shot_id) ? '这一镜正在跑' : '重新生成这一镜'"
            :disabled="runStore.running || isBusy(`re:${s.shot_id}`)"
            @click="start([s.shot_id])"
          >
            <AppIcon name="refresh" :size="14" />
          </button>
        </div>
      </article>
    </div>

  </div>
</template>

<style scoped>
.wall {
  display: grid;
  /* 148 → 190：格子里现在是真的播放器，原来那个宽度下浏览器自带的
     控件会挤成一团，进度条拖不动。 */
  grid-template-columns: repeat(auto-fill, minmax(190px, 1fr));
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
  /* 跟项目的画幅走，见 cellRatio。横屏项目写死 9/16 的话每张牌
     都是上下两条黑。 */
  aspect-ratio: var(--cell-ratio, 9 / 16);
  background: var(--bg-sunken);
  color: var(--text-3);
}
.shot__frame img,
.shot__video {
  width: 100%;
  height: 100%;
  display: block;
}
.shot__frame img {
  object-fit: cover;
}
/* **播放器用 contain 不是 cover。** 牌子是 9:16 的槽，而横屏项目出来的
   片子是 16:9——cover 会把它裁掉两边，等于让用户看一个和成片不一样的
   画幅。留黑边是对的：那就是这一镜真正的样子。 */
.shot__video {
  object-fit: contain;
  background: #000;
}
.shot__blank {
  position: absolute;
  inset: 0;
  margin: auto;
}


/* 进度条贴在缩略图底边。**画在镜头上**，不去别处看 */
/* **进度条贴上边，不是下边。** 下边被播放器自己的控件占了——重出一镜
   时那一镜的旧片子还在（路径没变），于是控件和进度条会叠在一起。 */
.shot__live {
  position: absolute;
  left: 0;
  right: 0;
  top: 0;
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

</style>
