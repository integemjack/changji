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
import { humanAgo } from '@/composables/useAction'
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

const files = ref([])
const loading = ref(false)
const currentRel = ref('')
const shots = ref([])
const videoEl = ref(null)
const playhead = ref(0)

/**
 * 镜头在成片里的起止时间。
 *
 * 装配是按分镜顺序首尾相接拼的，所以累加时长就是每一镜的入点。
 * 有了它，审片时点缩略图就能跳到那一镜，而不是拖进度条来回找。
 * 转场会让实际入点差零点几秒，审片够用了，不拿它做剪辑依据。
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
const forThisEpisode = computed(() =>
  session.episodeId ? files.value.filter((f) => f.name.includes(session.episodeId)) : [],
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
  () =>
    !!current.value &&
    !!session.episodeId &&
    String(current.value.name ?? '').includes(session.episodeId),
)

async function load() {
  if (!session.projectPath) {
    files.value = []
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
  await loadShots()
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

    <EmptyState v-else-if="!loading && !files.length" icon="film" title="还没有成片" hint="这一集的镜头还没跑出来，跑完自动装配成片">
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
          <div v-if="chapters.length && isMine" class="strip">
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
                :src="mediaUrl(session.projectPath, c.frame_path)"
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
              v-if="session.episodeId && f.name.includes(session.episodeId)"
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
  aspect-ratio: 9 / 16;
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
