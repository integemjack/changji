<script setup>
/**
 * 第七步：成片。
 *
 * 审片页。左边选片，右边播。手机上折成上下两段。
 * 播放器要够大——这一页存在的意义就是让人真的看一遍再发出去。
 */
import { computed, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { humanAgo } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const emit = defineEmits(['go'])
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
 */
const chapters = computed(() => {
  let at = 0
  return shots.value.map((s) => {
    const start = at
    at += s.duration_s || 0
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

async function load() {
  if (!session.projectPath) {
    files.value = []
    return
  }
  loading.value = true
  try {
    const data = await api.outputs(session.projectPath)
    files.value = data.files ?? []
    // 默认选当前这一集的成片，没有就选最新的一条
    const mine = forThisEpisode.value[0] ?? files.value[0]
    currentRel.value = mine?.rel ?? ''
  } catch (err) {
    ui.error(err.message)
    files.value = []
  } finally {
    loading.value = false
  }
  await loadShots()
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

watch(() => [session.projectPath, session.episodeId], load, { immediate: true })

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
</script>

<template>
  <div class="stack stack--lg">
    <div class="toolbar">
      <span v-if="current" class="tiny dim truncate">{{ current.name }}</span>
      <span class="spacer" />
      <button
        class="btn btn--ghost btn--sm"
        type="button"
        :disabled="loading || !session.hasProject"
        @click="load"
      >
        <AppIcon name="refresh" :size="14" />
        刷新
      </button>
      <button
        v-if="current"
        class="btn btn--primary"
        type="button"
        @click="emit('go', 'publish')"
      >
        <AppIcon name="upload" :size="15" />
        去上传
      </button>
    </div>

    <EmptyState v-if="!session.hasProject" icon="folder" tone="warn" title="还没选项目">
      <RouterLink to="/project" class="btn btn--primary">去第一步</RouterLink>
    </EmptyState>

    <EmptyState v-else-if="!loading && !files.length" icon="film" title="还没有成片" hint="先把制作跑完">
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
              :key="current.rel"
              ref="videoEl"
              class="player__video"
              :src="mediaUrl(session.projectPath, current.rel)"
              controls
              preload="metadata"
              playsinline
              @timeupdate="onTimeUpdate"
            />
          </div>

          <!-- 分镜跳转条 -->
          <div v-if="chapters.length" class="strip">
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

        <div v-if="current" class="player__meta row row--wrap tiny dim">
          <span class="numeric">{{ current.size_mb }} MB</span>
          <span>{{ humanAgo(current.mtime) }}</span>
          <span class="mono truncate">{{ current.rel }}</span>
          <span class="spacer" />
          <RouterLink to="/publish" class="btn btn--ghost btn--sm">
            <AppIcon name="upload" :size="14" />
            投递这一条
          </RouterLink>
        </div>
      </section>

      <!-- 片单 -->
      <section class="sec reel">
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
                :src="mediaUrl(session.projectPath, f.rel) + '#t=0.5'"
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
