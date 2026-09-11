<script setup>
/**
 * 这一集。
 *
 * 原来这是四页：剧本大纲（的单集那半）、镜头、成片、上传至平台。合成一页
 * 的理由不是「它们都属于这一集」，是**人在做的事是对照着看**：对着这句台词
 * 看这一镜对不对，看完整集顺手发出去。
 *
 * 四个视图对着同一集，切换不换路由——换路由等于每切一次都重新建组件，
 * 镜头那一页的轮询、选中状态、抽屉全要重来。
 *
 * **子视图各自保留自己的状态机**，这一页只管：选哪个视图、这一集是谁、
 * 以及「没选集」这一种情况。哪一集在顶栏上挑，这里不再写一遍标题。
 */
import { computed, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import EmptyState from '@/components/EmptyState.vue'
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
  <!-- 根元素不能叫 .ep：App.vue 顶栏的集号选择器也叫 .ep，父组件的 scoped
       样式会套到子组件的根上，这一页会被压到 14rem 宽。 -->
  <div class="episode">
    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="在项目库里点一个"
    />

    <!-- 「还没选到某一集」只判这一处。 -->
    <EmptyState
      v-else-if="!session.episodeId"
      icon="script"
      tone="warn"
      title="还没选到某一集"
      hint="顶上挑一集"
    >
      <RouterLink to="/assets?tab=episodes" class="btn btn--sm">去分集</RouterLink>
    </EmptyState>

    <template v-else>
      <nav class="tabs">
        <button
          v-for="v in VIEWS"
          :key="v.key"
          class="tab"
          :class="{ 'is-on': view === v.key }"
          type="button"
          @click="go(v.key)"
        >
          {{ v.label }}
        </button>
      </nav>

      <!-- keep-alive：切回镜头那一屏时轮询、选中和抽屉都还在。 -->
      <KeepAlive>
        <component :is="current.comp" :key="current.key" @go="go" />
      </KeepAlive>
    </template>
  </div>
</template>

<style scoped>
.episode {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
}
</style>
