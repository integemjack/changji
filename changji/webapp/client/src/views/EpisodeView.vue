<script setup>
/**
 * 这一集。
 *
 * 原来这是四页：剧本大纲（的单集那半）、镜头、成片、上传至平台。合成一页
 * 的理由不是「它们都属于这一集」，是**人在做的事是对照着看**：对着这句台词
 * 看这一镜对不对，看完整集顺手发出去。分成四页，来回换页才知道这一镜出自
 * 哪句话。
 *
 * 三个视图对着同一集，切换不换路由——换路由等于每切一次都重新建组件，
 * 镜头那一页的轮询、选中状态、抽屉全要重来。
 *
 * **子视图各自保留自己的状态机**，这一页只管：选哪个视图、这一集是谁、
 * 以及「没选集」这一种情况。ShotsView 那 1200 行原样搬进 EpShots，
 * 内部一行没动——这一轮只动结构。
 */
import { computed, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import EmptyState from '@/components/EmptyState.vue'
import StepHeader from '@/components/StepHeader.vue'
import EpFilm from '@/views/episode/EpFilm.vue'
import EpPublish from '@/views/episode/EpPublish.vue'
import EpScript from '@/views/episode/EpScript.vue'
import EpShots from '@/views/episode/EpShots.vue'
import { useSession } from '@/stores/session'

const session = useSession()
const route = useRoute()
const router = useRouter()

const VIEWS = [
  { key: 'script', label: '剧本', comp: EpScript },
  { key: 'shots', label: '镜头', comp: EpShots },
  { key: 'film', label: '成片', comp: EpFilm },
  { key: 'publish', label: '发布', comp: EpPublish },
]

/**
 * 当前视图存在 query 里。
 *
 * 存在组件状态里的话，刷新一次就跳回剧本——而人盯着看的多半是镜头那一屏，
 * 出片跑了一半刷新页面又回到剧本，等于每次都要多点一下。
 */
const view = computed(() => {
  const want = String(route.query.view ?? '')
  return VIEWS.some((v) => v.key === want) ? want : 'shots'
})
const current = computed(() => VIEWS.find((v) => v.key === view.value) ?? VIEWS[1])

function go(key) {
  if (key === view.value) return
  router.replace({ query: { ...route.query, view: key } })
}

const title = computed(() => {
  const ep = session.episode
  if (!ep) return '这一集'
  return ep.title ? `${ep.episode_id} · ${ep.title}` : ep.episode_id
})

// 旧路径 /shots /film /publish 直接进来时，把视图对上
const LEGACY = { '/shots': 'shots', '/film': 'film', '/publish': 'publish' }
watch(
  () => route.path,
  (path) => {
    const want = LEGACY[path]
    if (want) router.replace({ path: '/episode', query: { view: want } })
  },
  { immediate: true },
)
</script>

<template>
  <div class="stack stack--lg">
    <StepHeader :title="title">
      <template #actions>
        <nav class="tabs">
          <button
            v-for="v in VIEWS"
            :key="v.key"
            class="tabs__btn"
            :class="{ 'is-on': view === v.key }"
            type="button"
            @click="go(v.key)"
          >
            {{ v.label }}
          </button>
        </nav>
      </template>
    </StepHeader>

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="先回第一步选一个项目，或者新建一个。"
    >
      <RouterLink to="/project" class="btn btn--primary">去第一步</RouterLink>
    </EmptyState>

    <!-- 「还没选到某一集」只判这一处。原来四个页面各判一遍，是同一句话
         写四遍，而且措辞已经分叉了。 -->
    <EmptyState
      v-else-if="!session.episodeId"
      icon="script"
      tone="warn"
      title="还没选到某一集"
      hint="这一页是对着某一集干活的。顶上挑一集，还没有的话去故事页把分集落成剧集。"
    >
      <RouterLink to="/story" class="btn btn--primary">去故事页</RouterLink>
    </EmptyState>

    <!-- keep-alive：切回镜头那一屏时轮询、选中和抽屉都还在。
         不留的话每切一次都是重建，出片跑着的时候尤其明显。 -->
    <KeepAlive v-else>
      <component :is="current.comp" :key="current.key" @go="go" />
    </KeepAlive>
  </div>
</template>

<style scoped>
.tabs {
  display: flex;
  gap: 2px;
  padding: 2px;
  background: var(--surface-2);
  border: 1px solid var(--line);
  border-radius: 10px;
}
.tabs__btn {
  padding: 6px 16px;
  border: 0;
  border-radius: 8px;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.tabs__btn:hover {
  color: var(--text);
}
.tabs__btn.is-on {
  background: var(--surface);
  color: var(--accent);
  font-weight: 600;
  box-shadow: 0 1px 2px rgb(0 0 0 / 8%);
}
</style>
