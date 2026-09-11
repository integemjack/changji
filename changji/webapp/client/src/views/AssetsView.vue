<script setup>
/**
 * 设定。把故事变成能拍的东西。
 *
 * 三格，回答三个问题：
 *
 *     角色   谁      长什么样、什么声音、什么关系
 *     场景   哪儿    什么空间、什么光、什么色
 *     分集   怎么切  每集多长 → 切成几集，每一集停在哪
 *
 * **分成 tab 不是分成三页。** 三格共享同一个项目、同一份故事，切 tab 不该
 * 换地址、也不该重新拉一遍——它们是同一件事的三个面。tab 记在
 * `?tab=` 上，刷新和分享链接才停在原地。
 *
 * 关系摆在角色那一格里，不再单独占一条。它是**定妆的依据**（前任和母女的
 * 眼神不一样），而依据该挨着它服务的那件事。
 *
 * 2026-09-11 起照故事页的样子：没有页头，tab 就是这一页顶上的第一行。
 */
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import EmptyState from '@/components/EmptyState.vue'
import AssetCharacters from '@/views/assets/AssetCharacters.vue'
import AssetEpisodes from '@/views/assets/AssetEpisodes.vue'
import AssetLocations from '@/views/assets/AssetLocations.vue'
import { api } from '@/api'
import { useSession } from '@/stores/session'

const session = useSession()
const route = useRoute()
const router = useRouter()
const story = ref(null)

const TABS = [
  { key: 'characters', label: '角色' },
  { key: 'locations', label: '场景' },
  { key: 'episodes', label: '分集' },
]

const tab = computed(() => {
  const want = String(route.query.tab ?? '')
  return TABS.some((t) => t.key === want) ? want : 'characters'
})

function pick(key) {
  // replace 不是 push：切 tab 不该在浏览器的后退历史里堆一串
  router.replace({ query: { ...route.query, tab: key } })
}

const relations = computed(() => story.value?.relations ?? [])
const plan = computed(() => story.value?.plan ?? [])

/** tab 上那个小数字：这一格有多少东西。空的一眼看得出来。 */
function countOf(key) {
  if (key === 'episodes') return plan.value.length
  return 0
}

async function loadStory() {
  if (!session.projectPath) {
    story.value = null
    return
  }
  try {
    const data = await api.getStory(session.projectPath)
    story.value = data.story ?? null
  } catch {
    // 没有故事的老项目走到这儿是正常的，关系那一块不显示就是了
    story.value = null
  }
}

onMounted(loadStory)
watch(() => session.projectPath, loadStory)
</script>

<template>
  <div class="assets">
    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="在项目库里点一个"
    />

    <template v-else>
      <nav class="tabs">
        <button
          v-for="t in TABS"
          :key="t.key"
          class="tab"
          :class="{ 'is-on': tab === t.key }"
          type="button"
          @click="pick(t.key)"
        >
          {{ t.label }}
          <span v-if="countOf(t.key)" class="tab__n">{{ countOf(t.key) }}</span>
        </button>
      </nav>

      <!-- KeepAlive：切回来时还停在原来展开的那个角色上。三格各自都有
           一堆展开状态和没保存的编辑，切一下就丢的话没人敢切。 -->
      <KeepAlive>
        <div v-if="tab === 'characters'" class="stack">
          <!-- 关系。**定妆的依据，不是装饰**——想复仇的人和想赎罪的人
               眼神不一样，而这件事只有故事层知道。 -->
          <section v-if="relations.length" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">人物关系</h2>
            </div>
            <div class="rels">
              <div v-for="(r, i) in relations" :key="i" class="rel">
                <b class="rel__who">{{ r.a }} — {{ r.b }}</b>
                <span v-if="r.kind" class="pill pill--neutral tiny nowrap">{{ r.kind }}</span>
                <span v-if="r.tension" class="rel__why small">{{ r.tension }}</span>
              </div>
            </div>
          </section>

          <AssetCharacters />
        </div>

        <AssetLocations v-else-if="tab === 'locations'" />

        <AssetEpisodes v-else />
      </KeepAlive>
    </template>
  </div>
</template>

<style scoped>
.assets {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
}
.rels {
  display: grid;
  gap: var(--s1);
}
.rel {
  display: flex;
  align-items: baseline;
  gap: var(--s3);
  flex-wrap: wrap;
  font-size: var(--fs-sm);
}
.rel__who {
  flex: none;
}
.rel__why {
  color: var(--text-2);
  min-width: 0;
}
</style>
