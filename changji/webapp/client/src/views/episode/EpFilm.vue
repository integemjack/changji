<script setup>
/**
 * 第七步：成片。
 *
 * 审片页。左边选片，右边播。手机上折成上下两段。
 * 播放器要够大——这一页存在的意义就是让人真的看一遍再发出去。
 */
import { computed, onActivated, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { isFilmOf } from '@/api/labels'
import { humanAgo } from '@/composables/useAction'
import { useRun } from '@/stores/run'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const emit = defineEmits(['go'])
/** 投递那一层在不在。不在就没有「去上传」——它指向一个不存在的 tab。 */
const props = defineProps({ canPublish: { type: Boolean, default: false } })

// 这一格被 KeepAlive 冻着：在镜头格出完片切过来，看到的还是切走时那份。
// 切过来时自己重拉，就不用摆一个「刷新」按钮让人替它记着。
onActivated(load)
const session = useSession()
const ui = useUi()
const runner = useRun()

/**
 * 跳转条上那张首帧该用哪一代。
 *
 * 首帧写在 `frames/<shot_id>.png` 上，重出是原地覆盖、地址不变，而
 * `/api/media` 一个缓存头都不发（见下面 srcOf 那段）——不挂换代号的话，
 * 重出过的那几镜在这条跳转条上一直是老图。片子本身按 mtime 换代，条上
 * 的缩略图原来什么都没有。
 *
 * 用 run store 按镜记的那个数（`settledBy`），只换这一集里真重跑过的那
 * 几张；整条无条件换代的话，每切一次这一格就要重拉十几张全尺寸首帧。
 */
const frameBust = (shotId) => runner.settledBy.get(shotId) ?? 0

const files = ref([])
const loading = ref(false)
const currentRel = ref('')
const shots = ref([])
const videoEl = ref(null)
const playhead = ref(0)
/** 这部剧的画面规格。只用来定跳转条上那排格子的长宽比，见 stripRatio。 */
const spec = ref(null)

/**
 * 跳转条每格的长宽比跟项目的 [video] 走，不是写死竖屏。
 *
 * 格子里是 `object-fit: cover` 的首帧：槽的比例和首帧对得上就一个像素都
 * 不裁，对不上是**裁**，不是留黑边。竖屏项目（默认那一种）正好对上，所以
 * 写死的 9:16 一直没露馅；**横屏项目整条就废了**——16:9 的首帧塞进 9:16
 * 的槽，左右各切掉三分之一还多，条上剩下一溜画面正中间的竖条，认不出哪
 * 一镜是哪一镜。而这条跳转条存在的全部意义就是"看着缩略图跳到那一镜"。
 *
 * 镜头墙那一页早就为同一件事改过（EpShots 的 cellRatio：「写死 9:16 的
 * 话，横屏项目的每张牌都是上下两条黑、中间一小条画面」），做法照它。
 */
const stripRatio = computed(() => {
  const v = spec.value
  if (!v?.width || !v?.height) return '9 / 16'
  return `${v.width} / ${v.height}`
})

/**
 * 镜头在成片里的起止时间。
 *
 * 装配是按分镜顺序首尾相接拼的，所以累加时长就是每一镜的入点。
 * 有了它，审片时点缩略图就能跳到那一镜，而不是拖进度条来回找。
 * **这里不要为转场去减重叠。** 看着像该减——分镜里有 transition_in 和
 * transition_dur_s——但装配根本不渲染转场：拼接走的是 `-f concat -c copy`，
 * 纯硬切，全树一处 xfade / acrossfade 都没有。引擎那边 media/assemble.cpp
 * 用一整段记着这件事：它原来就是按"溶解会让两镜重叠"把起点往前拉的，
 * 结果时间线比成片短、字幕比画面早，两镜 dissolve 到片尾差了将近一秒，
 * 而每一条单看都"差不多对"。**模拟一个不渲染的东西，错的是两处。**
 *
 * **累加的必须是 `real_duration_s`。** `duration_s` 是名义值，而模型只能
 * 按格子出帧（名义 2 秒实际出 56 帧 = 2.333 秒），**差值逐镜累积**：
 * walk_c ep01 实测，到第 18 镜时名义累加是 56 秒、真实是 59 秒，
 * 点那个刻度会跳进上一镜里去。装配那边（media/assemble.cpp 的
 * build_timeline）一直是按真值排的，这儿跟上它。
 */
const chapters = computed(() => {
  let at = 0
  return shots.value.map((s) => {
    const start = at
    at += s.real_duration_s ?? s.duration_s ?? 0
    return { ...s, start, end: at }
  })
})
const activeShotId = computed(
  () => chapters.value.find((c) => playhead.value >= c.start && playhead.value < c.end)?.shot_id ?? '',
)

const current = computed(
  () => files.value.find((f) => f.rel === currentRel.value) ?? null,
)

/**
 * 这一集还差几镜没出视频。空状态那句话按它分两种说法。
 *
 * 一律说「镜头还没跑出来」是不对的：镜头全跑完、只差装配那一步的情况真实
 * 存在（装配要 ffmpeg，缺了引擎会跳过并明说"各镜头的视频已经在 shots/ 下"；
 * 装配自己也可能失败，比如某一镜降级了没留下视频）。那时候把人送去镜头页
 * 是送到一个没事可做的地方——项目库那条栏早就分得清，它写的是「镜头出完了，
 * 还没装配」（见 project-stage.js 里 assemble 那一档）。
 */
const shotsLeft = computed(
  () => shots.value.filter((s) => !s.video_path).length,
)
const forThisEpisode = computed(() =>
  session.episodeId
    ? files.value.filter((f) => isFilmOf(f.name, session.episodeId))
    : [],
)

/**
 * 正在播的这条，是不是**这一集**的片。
 *
 * 片单是全项目的（有意的，见下面那段），所以这一页上完全可能在播别的集：
 * 手点了片单里的另一条，或者这一集还没出片、load() 回落到了最新的那条。
 *
 * 两处都要认它：
 *   · 分镜跳转条画的是 `shots`——**永远是这一集的**。播着 ep02 的片、
 *     底下摆着 ep01 的缩略图，点一下按 ep01 的时长跳，跳到哪儿全凭巧合。
 *   · 片名原来只在最上面那条工具栏里出现，而那条整条挂在 `canPublish` 上；
 *     投递那层不在（现在的默认构建就是）时它根本不渲染——于是页面上播着
 *     别的集的片，一个字都不说。
 */
const isMine = computed(
  () => !!current.value && isFilmOf(current.value.name, session.episodeId),
)

async function load() {
  if (!session.projectPath) {
    files.value = []
    loading.value = false // 理由同镜头墙那处：被顶掉的那趟不会清它
    return
  }
  // 这一趟是给哪部剧读的。换剧时两趟会叠在一起，慢的那趟后落地就把上
  // 一部的片单摆在这一部下面——而这一页是"审完再发出去"用的。
  const want = session.projectPath
  loading.value = true
  try {
    const data = await api.outputs(session.projectPath)
    if (want !== session.projectPath) return
    files.value = data.files ?? []
    // 默认选当前这一集的成片，没有就选最新的一条
    const mine = forThisEpisode.value[0] ?? files.value[0]
    currentRel.value = mine?.rel ?? ''
  } catch (err) {
    if (want !== session.projectPath) return
    ui.error(err.message)
    files.value = []
  } finally {
    if (want === session.projectPath) loading.value = false
  }
  await Promise.all([loadShots(), loadSpec()])
}

/** 画面规格。读不到就退回竖屏——这一页不该因为它打不开。 */
async function loadSpec() {
  if (!session.projectPath) {
    spec.value = null
    return
  }
  // 换剧时两趟会叠，慢的那趟后落地就是拿上一部的画幅去排这一部的格子
  const want = session.projectPath
  try {
    const got = await api.projectVideo(want)
    if (want !== session.projectPath) return
    spec.value = got
  } catch {
    if (want === session.projectPath) spec.value = null
  }
}

async function loadShots() {
  if (!session.projectPath || !session.episodeId) {
    shots.value = []
    return
  }
  const want = `${session.projectPath}::${session.episodeId}`
  const mine = () => want === `${session.projectPath}::${session.episodeId}`
  try {
    const data = await api.shots(session.projectPath, session.episodeId)
    if (!mine()) return
    shots.value = data.shots ?? []
  } catch {
    if (mine()) shots.value = []
  }
}

watch(() => [session.projectPath, session.episodeId], load, { immediate: true })
/**
 * 那一轮跑完，片子就是这一刻落盘的——这一页要自己看见。
 *
 * 这一格被 KeepAlive 冻着，进来时靠 `onActivated(load)` 重拉；但"开跑之后
 * 切到这一格等着看成片"是很自然的一种用法，而那样 onActivated 早就过去了
 * ——装配写盘时这一页一动不动，人以为没出来，其实文件已经在了。
 * 只订下降沿：跑的过程中这一页没有任何东西会变。
 */
watch(
  () => runner.running,
  (now, before) => {
    if (before && !now) load()
  },
)

/**
 * 成片的地址，**带上这个文件的 mtime**。
 *
 * `/api/media` 一个 `Cache-Control` / `ETag` / `Last-Modified` 都不发，而
 * 装配出来的片子**每次都落在同一个路径上**（output/ep01.mp4）。地址一个字
 * 不变的话，重出一版之后在这一页看到的还是浏览器缓存里的上一版——而这一页
 * 存在的全部意义就是"让人真的看一遍再发出去"。
 *
 * 镜头墙早就在做这件事（useShots 的 bust：「刚跑完，磁盘上那几个 mp4 换过了
 * 但路径没变」），朗读和试听音色也各自加了时间戳。只有审片这一屏漏了。
 *
 * **用 mtime 不用 Date.now()**：随机数每次 load 都换一次地址，正在看的片子
 * 会被打回开头重新缓冲；mtime 只在文件真换过之后才变。
 */
function srcOf(f) {
  if (!f?.rel) return ''
  const u = mediaUrl(session.projectPath, f.rel)
  return f.mtime ? `${u}&v=${f.mtime}` : u
}

function seekTo(chapter) {
  const el = videoEl.value
  if (!el) return
  el.currentTime = chapter.start
  el.play().catch(() => {
    // 浏览器不让自动播就算了，跳过去这件事已经做到了
  })
}

function onTimeUpdate(event) {
  playhead.value = event.target.currentTime
}

// 换一条片，播放头得跟着回零：video 换了 key 是重新挂的，在第一次
// timeupdate 之前 playhead 还停在上一条的位置上，跳转条会把一个毫不相干
// 的格子标成「正在播」。
watch(currentRel, () => {
  playhead.value = 0
})
</script>

<template>
  <div class="stack stack--lg">
    <div v-if="current && props.canPublish" class="toolbar">
      <span class="tiny dim truncate">{{ current.name }}</span>
      <span class="spacer" />
      <button class="btn btn--primary" type="button" @click="emit('go', 'publish')">
        <AppIcon name="upload" :size="15" />
        去上传
      </button>
    </div>

    <EmptyState v-if="!session.hasProject" icon="folder" tone="warn" title="还没选项目">
      <RouterLink to="/project" class="btn btn--primary">去项目页</RouterLink>
    </EmptyState>

    <EmptyState
      v-else-if="!loading && !files.length && shots.length && !shotsLeft"
      icon="film"
      tone="warn"
      title="镜头都出完了，还没装配成片"
      hint="装配跟在出片那一轮后面做。缺 ffmpeg 的话设置页的体检会说；装好之后回镜头页跑一轮（哪怕只重出一镜），那一轮末尾就会把成片拼出来"
    >
      <button class="btn btn--primary" type="button" @click="emit('go', 'shots')">
        去镜头页
      </button>
    </EmptyState>

    <EmptyState
      v-else-if="!loading && !files.length"
      icon="film"
      title="还没有成片"
      :hint="shotsLeft ? `这一集还差 ${shotsLeft} 镜没出视频，跑完自动装配成片` : '这一集的镜头还没跑出来，跑完自动装配成片'"
    >
      <button class="btn btn--primary" type="button" @click="emit('go', 'shots')">
        去做镜头
      </button>
    </EmptyState>

    <div v-else-if="files.length" class="film">
      <!-- 播放器 -->
      <section class="player">
        <div class="player__box">
          <div class="player__stage">
            <video
              v-if="current"
              :key="current.rel + ':' + (current.mtime ?? '')"
              ref="videoEl"
              class="player__video"
              :src="srcOf(current)"
              controls
              preload="metadata"
              playsinline
              @timeupdate="onTimeUpdate"
            />
          </div>

          <!-- 分镜跳转条 -->
          <div v-if="chapters.length && isMine" class="strip" :style="{ '--cell-ratio': stripRatio }">
            <button
              v-for="c in chapters"
              :key="c.shot_id"
              class="strip__cell"
              :class="{ 'strip__cell--on': c.shot_id === activeShotId }"
              type="button"
              :title="`第 ${c.order + 1} 镜 · ${Math.round(c.start)}s 起`"
              @click="seekTo(c)"
            >
              <img
                v-if="c.frame_path"
                :src="mediaUrl(session.projectPath, c.frame_path) + '&_=' + frameBust(c.shot_id)"
                :alt="`第 ${c.order + 1} 镜`"
                loading="lazy"
              />
              <span v-else class="strip__num numeric">{{ c.order + 1 }}</span>
              <span class="strip__t tiny numeric">{{ Math.round(c.start) }}s</span>
            </button>
          </div>
        </div>

        <!-- 完整路径删了（一年用一次，见项目页那条），要抄放在 title 里 -->
        <div v-if="current" class="player__meta row row--wrap tiny dim" :title="current.rel">
          <span class="truncate">{{ current.name }}</span>
          <span class="numeric">{{ current.size_mb }} MB</span>
          <span>{{ humanAgo(current.mtime) }}</span>
          <!-- 播的不是这一集时要挑明：它同时也是上面那条跳转条不见了的原因 -->
          <span v-if="!isMine" class="pill pill--warn tiny nowrap">别的集</span>
        </div>
      </section>

      <!-- 片单。**只有一条时不显示**：这是全项目的成片清单摆在一集的页面上，
           一条的时候播放器本身就是答案，清单只是把同一件事再说一遍。 -->
      <section v-if="files.length > 1" class="sec reel">
        <div class="sec__head">
          <h2 class="sec__t">已出的片</h2>
          <span class="tiny dim">{{ files.length }} 条</span>
        </div>
        <div class="reel__list">
          <button
            v-for="f in files"
            :key="f.rel"
            class="reelrow"
            :class="{ 'reelrow--on': f.rel === currentRel }"
            type="button"
            @click="currentRel = f.rel"
          >
            <!--
              拿视频自己当缩略图。#t=0.5 让浏览器定位到第 0.5 秒并把那一帧
              画出来——第 0 帧常常是黑场。preload="metadata" 只拉文件头，
              十几条片也不会把带宽吃光。（这一条依赖转发层支持 Range，
              早先不支持的时候这里只会是一块黑。）
            -->
            <span class="reelrow__icon">
              <video
                class="reelrow__thumb"
                :src="srcOf(f) + '#t=0.5'"
                preload="metadata"
                muted
                playsinline
                tabindex="-1"
              />
              <AppIcon class="reelrow__fallback" name="film" :size="15" />
            </span>
            <span class="reelrow__text">
              <span class="reelrow__name truncate">{{ f.name }}</span>
              <span class="tiny dim">
                <span class="numeric">{{ f.size_mb }} MB</span> ·
                {{ humanAgo(f.mtime) }}
              </span>
            </span>
            <span
              v-if="isFilmOf(f.name, session.episodeId)"
              class="pill pill--accent tiny nowrap"
            >
              本集
            </span>
          </button>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.film {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 300px;
  gap: var(--s5);
  align-items: start;
}

.player__box {
  overflow: hidden;
  border-radius: var(--r);
}
.player__stage {
  background: #000;
  display: grid;
  place-items: center;
  /* 竖屏片在宽屏上不该拉伸，两边留黑边就好 */
  max-height: 68vh;
}
.player__video {
  display: block;
  max-height: 68vh;
  max-width: 100%;
}
.strip {
  display: flex;
  gap: 4px;
  padding: var(--s2);
  overflow-x: auto;
  background: var(--bg-sunken);
  scrollbar-width: thin;
}
.strip__cell {
  position: relative;
  flex: none;
  width: 46px;
  /* 跟项目的画幅走，不写死竖屏。见 stripRatio 那段。 */
  aspect-ratio: var(--cell-ratio, 9 / 16);
  padding: 0;
  border: 1px solid var(--line);
  border-radius: var(--r-sm);
  background: var(--surface-3);
  overflow: hidden;
  cursor: pointer;
  display: grid;
  place-items: center;
  color: var(--text-3);
  transition: border-color 0.14s var(--ease), transform 0.1s var(--ease);
}
.strip__cell:hover {
  border-color: var(--line-strong);
  transform: translateY(-2px);
}
.strip__cell--on {
  border-color: var(--accent);
  box-shadow: 0 0 0 1px var(--accent);
}
.strip__cell img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.strip__num {
  font-size: var(--fs-sm);
  font-weight: 700;
}
.strip__t {
  position: absolute;
  left: 0;
  right: 0;
  bottom: 0;
  padding: 1px 0;
  background: rgba(0, 0, 0, 0.62);
  color: #fff;
  font-size: 9px;
  text-align: center;
}

.player__meta {
  padding: var(--s2) 0;
}
/* 片名是 flex 里的一项：min-width 默认是 auto，而 .truncate 又把它设成
   nowrap，于是长文件名不缩、直接把这一行顶出屏幕。给它一条能缩的下限，
   省略号才真的省得掉。 */
.player__meta .truncate {
  min-width: 0;
}

.reel {
  padding-top: 0;
  border-top: 0;
}
.reel__list {
  display: flex;
  flex-direction: column;
  max-height: 60vh;
  overflow-y: auto;
}
.reelrow {
  display: flex;
  align-items: center;
  gap: var(--s3);
  padding: var(--s2);
  background: none;
  border: none;
  border-radius: var(--r);
  cursor: pointer;
  text-align: left;
  color: var(--text-2);
}
.reelrow:hover {
  background: var(--surface-2);
  color: var(--text);
}
.reelrow--on {
  background: var(--accent-soft);
  color: var(--text);
}
.reelrow__icon {
  position: relative;
  flex: none;
  display: grid;
  place-items: center;
  width: 34px;
  height: 46px;
  border-radius: var(--r-sm);
  overflow: hidden;
  background: var(--surface-3);
  color: var(--text-3);
}
.reelrow__thumb {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  object-fit: cover;
  pointer-events: none;
}
/* 缩略图拉不出来时露出下面的胶片图标，而不是留一块空白 */
.reelrow__fallback {
  position: relative;
  z-index: -1;
}
.reelrow--on .reelrow__icon {
  outline: 2px solid var(--accent);
  outline-offset: -2px;
}
.reelrow__text {
  display: flex;
  flex-direction: column;
  min-width: 0;
  line-height: 1.3;
}
.reelrow__name {
  font-size: var(--fs-base);
  font-weight: 500;
}

@media (max-width: 980px) {
  .film {
    grid-template-columns: 1fr;
  }
  .player__stage,
  .player__video {
    max-height: 52vh;
  }
  .reel {
    padding-top: var(--s4);
    border-top: 1px solid var(--line);
  }
}
</style>
