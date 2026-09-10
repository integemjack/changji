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
 * 已经交给引擎、但它还没报出第一条进度的那几镜。
 *
 * **点下去到第一条进度之间能隔一分钟**——那段时间引擎在载模型，
 * 一个字都不会报。这期间牌子上显示的还是上一轮的"成片完成"：
 * 用户点了按钮，画面一动不动，只能再点一次。
 */
const sent = ref(new Set())

/**
 * 还没交给引擎、在这儿排着的那几镜。
 *
 * **队列在前端，不在引擎。** `POST /api/run` 在有任务跑着的时候回 409
 * （"已经在跑 ep01 了"），而那条 409 是和 Python 逐字节对拍的，动不得。
 * 所以跑着的时候点别的镜头不发请求，先记在这儿，这一轮完了一次性提交。
 *
 * 这样"点一个别的都点不了"就没有了——用户可以一路点过去，
 * 挑出十几个要重出的，然后走开。
 */
const waiting = ref(new Set())

/** 这一镜正在被处理：引擎在跑、已提交、或者在前端排着。 */
function busy(shotId) {
  return (
    Boolean(inflightBy.value[shotId]) ||
    sent.value.has(shotId) ||
    waiting.value.has(shotId)
  )
}

// 引擎开始报这一镜了，"排队中"就该让位给真进度。**两个集合都要清**：
// 只清 sent 的话，一个既在 waiting 里又被引擎跑着的镜头会同时显示
// "首帧"（状态取 inflight）和"不重出这一镜了"（按钮取 waiting），
// 同一格上两个互相矛盾的说法。
watch(inflightBy, (now) => {
  const ids = Object.keys(now)
  if (!ids.length) return
  for (const set of [sent, waiting]) {
    if (!set.value.size) continue
    const next = new Set(set.value)
    for (const id of ids) next.delete(id)
    if (next.size !== set.value.size) set.value = next
  }
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
      // 这一轮交出去的那几个跑完了（或者失败了），不该再挂着"排队中"。
      sent.value = new Set()
      // 刚跑完，磁盘上那几个 mp4 换过了但路径没变。见 bust 的注释。
      bust.value += 1
      session.refresh()
      if (runStore.state?.error) ui.error(runStore.state.error)
      else ui.ok('这一轮跑完了')
      // 跑的过程中攒下的那几镜，现在一次性交出去。
      flushWaiting()
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
  if (!started) return false
  // **先把牌子点亮，别等引擎。** 见 sent 的注释：载模型那一分钟里
  // 引擎一个字都不报，不先点亮的话用户看到的是"点了没反应"。
  if (one) sent.value = new Set([...sent.value, ...ids])
  runStore.start()
  return true
}

/** 把攒着的那几镜一次性交给引擎。空的就什么都不做。 */
function flushWaiting() {
  const ids = [...waiting.value]
  if (!ids.length) return
  waiting.value = new Set()
  start(ids)
}

/**
 * 牌子上那个按钮。**同一个位置三件事**，看这一镜此刻是什么状态：
 *
 *   在前端排着 → 取消，从队列里拿掉，不发任何请求
 *   引擎正在跑 → 停下这一轮（引擎只有整轮的停，没有单镜的停）
 *   其余       → 重新生成这一镜；正跑着别的就先排队
 */
async function shotAction(s) {
  const id = s.shot_id
  // **先判"引擎正在跑它"。** 反过来的话，一个刚排进队列、紧接着就被
  // 引擎接手的镜头会一直按"排队中"处理——按钮画的是取消，
  // 而它其实已经在跑了，取消什么都不会发生。
  if (inflightBy.value[id] || sent.value.has(id)) {
    await stop()
    return
  }
  if (waiting.value.has(id)) {
    const next = new Set(waiting.value)
    next.delete(id)
    waiting.value = next
    return
  }

  // **这一段必须在 await 之前跑完。** 连着点两下的话，第二下发生在
  // 第一下的请求还没回来的时候，那时 runStore.running 还是假
  // （它要等下一次轮询，最长 1.2 秒），于是第二镜也走了提交那条路——
  // 而那条路上有两道门把它悄悄吃掉：useAction 的 busy 是**全页共用的**，
  // 第二次调用直接返回 undefined 连请求都不发；就算发了，引擎那边
  // `POST /api/run` 有任务跑着时回 409。两种情况都是 `if (!started) return`，
  // 那一格什么都不会显示——用户报的"排队的并没有显示 wait 状态"就是这个。
  //
  // 判据换成"这一轮已经交出去过东西了"（sent 非空），并且**同步**先记上，
  // 那个窗口就不存在了。
  if (runStore.running || sent.value.size > 0) {
    waiting.value = new Set([...waiting.value, id])
    return
  }
  sent.value = new Set([...sent.value, id])
  const ok = await start([id])
  if (!ok) {
    // 没提交上（引擎正忙、或者别的浏览器抢先了）。**别丢掉**，
    // 挪进队列等这一轮完——丢掉的话用户点过的那一下就白点了。
    sent.value = new Set([...sent.value].filter((x) => x !== id))
    waiting.value = new Set([...waiting.value, id])
  }
}

/**
 * 牌子底栏上那句话：**这一镜在干什么，到第几步了。**
 *
 * 只有阶段名（"首帧"）的话，一条几十秒不动的进度条和卡死了看着一样。
 * 带上步数就有了在走的证据。步数只有引擎在报这一镜时才有——
 * 搬权重、VAE 解码那几条不带（见 Event::shot_steps），那时候只写阶段。
 */
function shotState(s) {
  const x = inflightBy.value[s.shot_id]
  if (x) {
    const stage = STAGE_LABELS[x.stage] || x.stage || ''
    if (typeof x.shotStep === 'number' && x.shotSteps) {
      return `${stage} ${x.shotStep}/${x.shotSteps}`
    }
    return stage || '跑着'
  }
  if (busy(s.shot_id)) return '排队中'
  return statusOf(s.status).label
}

/** 这一格的按钮该画成什么。 */
function shotBtn(s) {
  const id = s.shot_id
  // 顺序和 shotAction 一致，理由见那儿。
  if (inflightBy.value[id] || sent.value.has(id)) {
    // 引擎没有"暂停这一镜"，只有停下整轮。**按钮上写清楚它到底做什么**，
    // 画个暂停号却停掉整轮就是骗人。停下之后跑完的镜头都留着，
    // 再点「出片」会从没跑完的那些接着来——所以叫暂停是站得住的。
    return { icon: 'pause', title: '停下这一轮（跑完的镜头留着）' }
  }
  if (waiting.value.has(id)) {
    return { icon: 'close', title: '不重出这一镜了' }
  }
  if (runStore.running) {
    return { icon: 'refresh', title: '排进队列，这一轮跑完就重出这一镜' }
  }
  return { icon: 'refresh', title: '重新生成这一镜' }
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

        </div>

        <!-- **进度就是这一行的底色。** 原来是画面底边上一条 3px 的线，
             牌子里换成真播放器之后那条线和播放器自己的控件挤在一起。
             铺成这一行的背景既不占地方，也比一条细线看得清——
             而且"跑到哪了"和"这一镜叫什么、什么状态"本来就该在一起看。 -->
        <div class="shot__bottom">
          <span
            v-if="busy(s.shot_id)"
            class="shot__fill"
            :class="{ 'shot__fill--idle': pct(s.shot_id) === null }"
            :style="pct(s.shot_id) !== null ? { width: pct(s.shot_id) + '%' } : null"
          />
          <span class="shot__no numeric">{{ s.order + 1 }}</span>
          <!-- 状态和步数都在这儿，见 shotState。 -->
          <span class="shot__state tiny">{{ shotState(s) }}</span>
          <span class="spacer" />
          <!-- **不跟着别人一起变灰。** 原来是 `runStore.running` 一真
               整墙的按钮全禁掉：重出一镜要等一小时的整轮跑完才能点第二个。
               现在跑着的时候点别的镜头是排队（队列在前端，见 waiting），
               点正在跑的那个是停下。 -->
          <button
            class="iconbtn"
            type="button"
            :title="shotBtn(s).title"
            :disabled="isBusy(`re:${s.shot_id}`) || isBusy('stop')"
            @click="shotAction(s)"
          >
            <AppIcon :name="shotBtn(s).icon" :size="14" />
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
/* 进度铺成底部那一行的背景。**在文字后面**，所以是 z-index 0 加上
   兄弟节点提到 1——不这么做的话镜号和状态会被它盖住。 */
.shot__fill {
  position: absolute;
  left: 0;
  top: 0;
  bottom: 0;
  z-index: 0;
  background: color-mix(in srgb, var(--accent) 30%, transparent);
  transition: width 0.3s;
}
/* 不知道跑到哪一步时的走马灯。**别停着不动**——静止的进度条和
   "卡死了"看起来一模一样，而这一步（搬权重、VAE 解码）本来就要几十秒。 */
.shot__fill--idle {
  width: 40%;
  animation: shot-slide 1.6s ease-in-out infinite;
}
@keyframes shot-slide {
  0% { transform: translateX(-100%); }
  100% { transform: translateX(250%); }
}
@media (prefers-reduced-motion: reduce) {
  .shot__fill--idle { animation: none; width: 100%; opacity: 0.6; }
}

.shot__bottom {
  position: relative;   /* 进度底色是绝对定位的，见 .shot__fill */
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 6px;
  border-top: 1px solid var(--line);
  overflow: hidden;     /* 走马灯靠 translateX 走出去，不裁会画到牌子外面 */
}
/* 这一行的内容全部压在进度底色上面 */
.shot__bottom > :not(.shot__fill) {
  position: relative;
  z-index: 1;
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
