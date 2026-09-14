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
 *
 * 2026-09-14 按设定页那条规矩重排：
 *   · **tab 就是状态行**——「剧本 1232 字」「镜头 16 · 差 16 首帧」「成片 1」。
 *     站在剧本格也知道镜头到哪一步了。数从这儿拉，不从子视图要：子视图
 *     被 KeepAlive 冻着，没切过去的那格根本没加载。
 *   · **默认落在第一个没做完的那格**。原来写死落在镜头，新的一集进来看到
 *     的是"还没有分镜 / AI 出分镜"，可分镜要先有剧本，那个按钮按了也白按。
 *     有了剧本之后还是落镜头——人盯着看的多半是那一屏。
 *   · **引擎说没有投递就不显示「发布」。** 这个二进制不带投递（要起 Node
 *     那层），原来它照样占一个 tab，点进去是一段道歉、一个灰掉的按钮和一个
 *     空态；成片格里还有两处「去上传」指着它。
 */
import { computed, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import EmptyState from '@/components/EmptyState.vue'
import EpFilm from '@/views/episode/EpFilm.vue'
import EpPublish from '@/views/episode/EpPublish.vue'
import EpScript from '@/views/episode/EpScript.vue'
import EpShots from '@/views/episode/EpShots.vue'
import { api } from '@/api'
import { useSession } from '@/stores/session'

const session = useSession()
const route = useRoute()
const router = useRouter()

// ---- tab 上的数 ----
//
// 三个接口、一次并发。失败的那一个当成"没有"，不该因为成片目录读不到
// 就整页红——剧本和镜头照样能做。
const scriptChars = ref(0)
const shots = ref([])
const outputs = ref(0)
/** 投递这一层在不在。不在就没有那个 tab，成片格里的「去上传」也不显示。 */
const publishOk = ref(false)
const loaded = ref(false)

async function load() {
  if (!session.projectPath || !session.episodeId) {
    scriptChars.value = 0
    shots.value = []
    outputs.value = 0
    loaded.value = false
    return
  }
  const [sc, sh, out, ps] = await Promise.allSettled([
    api.getScript(session.projectPath, session.episodeId),
    api.shots(session.projectPath, session.episodeId),
    api.outputs(session.projectPath),
    api.platforms(),
  ])
  scriptChars.value =
    sc.status === 'fulfilled' ? [...String(sc.value?.script ?? '').trim()].length : 0
  shots.value = sh.status === 'fulfilled' ? (sh.value?.shots ?? []) : []
  const files = out.status === 'fulfilled' ? (out.value?.files ?? []) : []
  outputs.value = files.filter((f) => String(f.name ?? '').includes(session.episodeId)).length
  // 投递那层不在时 platforms 回的是 {error: "…"}，不是抛异常（见 EpPublish）
  publishOk.value = ps.status === 'fulfilled' && !ps.value?.error
  loaded.value = true
}

const pendingFrames = computed(() => shots.value.filter((s) => !s.frame_path).length)
const pendingVideos = computed(() => shots.value.filter((s) => !s.video_path).length)

/**
 * tab 上写什么。**数是答案，缺才是重点**——齐了就只有一个数，缺才多一截。
 * 没剧本、没分镜时只有名字。
 */
const VIEWS = computed(() => {
  const n = shots.value.length
  const gap = !n
    ? ''
    : pendingFrames.value
      ? `差 ${pendingFrames.value} 首帧`
      : pendingVideos.value
        ? `差 ${pendingVideos.value} 视频`
        : ''
  const list = [
    { key: 'script', label: '剧本', n: scriptChars.value ? `${scriptChars.value} 字` : '', gap: '', comp: EpScript },
    { key: 'shots', label: '镜头', n: n ? String(n) : '', gap, comp: EpShots },
    { key: 'film', label: '成片', n: outputs.value ? String(outputs.value) : '', gap: '', comp: EpFilm },
  ]
  if (publishOk.value) list.push({ key: 'publish', label: '发布', n: '', gap: '', comp: EpPublish })
  return list
})

/**
 * 当前视图存在 query 里。
 *
 * 存在组件状态里的话，刷新一次就跳回剧本——而人盯着看的多半是镜头那一屏，
 * 出片跑了一半刷新页面又回到剧本，等于每次都要多点一下。
 *
 * query 里没写时落在第一个没做完的：没剧本去剧本，其余去镜头。
 * 数还没拉回来那一瞬先按镜头，拉回来发现没剧本再换——不会闪，只是首帧。
 */
const view = computed(() => {
  const want = String(route.query.view ?? '')
  if (VIEWS.value.some((v) => v.key === want)) return want
  return loaded.value && !scriptChars.value ? 'script' : 'shots'
})
const current = computed(() => VIEWS.value.find((v) => v.key === view.value) ?? VIEWS.value[1])

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

watch(() => [session.projectPath, session.episodeId], load, { immediate: true })
// 切 tab 的时候顺手重拉一次：子视图在自己格子里干完的活（采用剧本、出了
// 分镜、出完片）这儿才看得到。切 tab 正是人要看另一格答案的那一刻。
watch(view, load)
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
          <span v-if="v.n" class="tab__n">{{ v.n }}</span>
          <span v-if="v.gap" class="tab__gap">· {{ v.gap }}</span>
        </button>
      </nav>

      <!-- keep-alive：切回镜头那一屏时轮询、选中和抽屉都还在。 -->
      <KeepAlive>
        <component
          :is="current.comp"
          :key="current.key"
          :can-publish="publishOk"
          @go="go"
        />
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
/* 「差 16 首帧」那一截。比数字再轻一档——它是提醒，不是标题 */
.tab__gap {
  margin-left: 4px;
  color: var(--text-3);
  font-size: var(--fs-xs);
  font-weight: 400;
}
.tab.is-on .tab__gap {
  color: var(--warn, #f5a524);
}
</style>
