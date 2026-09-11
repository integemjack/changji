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
 * 用户 2026-09-11 的划分。之前这三样散在两页：人和地方挤在这一页上下叠着
 * （得滚很久才看得完），而分集混在「故事」那一页里——那一页的重点是创作，
 * 而分集是在看创作出来的东西被切成什么样，两件事互相干扰。
 *
 * **分成 tab 不是分成三页。** 三格共享同一个项目、同一份故事，切 tab 不该
 * 换地址、也不该重新拉一遍——它们是同一件事的三个面。tab 记在
 * `?tab=` 上，刷新和分享链接才停在原地。
 *
 * 关系摆在角色那一格里，不再单独占一条。它是**定妆的依据**（前任和母女的
 * 眼神不一样），而依据该挨着它服务的那件事，不是挨着页头。
 */
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import EmptyState from '@/components/EmptyState.vue'
import StepHeader from '@/components/StepHeader.vue'
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
  { key: 'characters', label: '角色', hint: '谁' },
  { key: 'locations', label: '场景', hint: '哪儿' },
  { key: 'episodes', label: '分集', hint: '怎么切' },
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
const chapters = computed(() => story.value?.chapters ?? [])
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
  <div class="stack stack--lg">
    <StepHeader />

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="设定挂在项目上。在项目库那条栏里点一个。"
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
          <span class="tab__hint">{{ t.hint }}</span>
          <span v-if="countOf(t.key)" class="tab__n numeric">{{ countOf(t.key) }}</span>
        </button>
      </nav>

      <!-- KeepAlive：切回来时还停在原来展开的那个角色上。三格各自都有
           一堆展开状态和没保存的编辑，切一下就丢的话没人敢切。 -->
      <KeepAlive>
        <div v-if="tab === 'characters'" class="stack stack--lg">
          <!-- 关系。**定妆的依据，不是装饰**——想复仇的人和想赎罪的人
               眼神不一样，而这件事只有故事层知道。 -->
          <section v-if="relations.length" class="card">
            <div class="card__head">
              <div>
                <div class="card__title">人物关系</div>
                <div class="card__sub">
                  从故事里提的。定妆时照着它拿捏气质——只写「前任」不够，
                  绷着的是什么才是要画出来的东西。
                </div>
              </div>
            </div>
            <div class="card__body">
              <div class="rels">
                <div v-for="(r, i) in relations" :key="i" class="rel">
                  <b class="rel__who">{{ r.a }} — {{ r.b }}</b>
                  <span v-if="r.kind" class="pill pill--neutral tiny nowrap">{{ r.kind }}</span>
                  <span v-if="r.tension" class="rel__why small">{{ r.tension }}</span>
                </div>
              </div>
            </div>
          </section>

          <AssetCharacters />
        </div>

        <AssetLocations v-else-if="tab === 'locations'" />

        <AssetEpisodes v-else />
      </KeepAlive>

      <p v-if="tab === 'episodes' && chapters.length" class="tiny dim">
        正文在「故事」那一页改。这一格只看它被切成什么样——同一段字两个地方
        都能改的话，迟早对不上。
      </p>
    </template>
  </div>
</template>

<style scoped>
.tabs {
  display: flex;
  gap: 2px;
  border-bottom: 1px solid var(--line);
}
.tab {
  display: inline-flex;
  align-items: baseline;
  gap: 6px;
  padding: 8px 14px;
  border: 0;
  border-bottom: 2px solid transparent;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-md);
  cursor: pointer;
}
.tab:hover {
  color: var(--text-1);
}
.tab.is-on {
  color: var(--accent);
  border-bottom-color: var(--accent);
}
/* 谁 / 哪儿 / 怎么切。三个字说清这一格管什么，比图标管用 */
.tab__hint {
  font-size: var(--fs-xs);
  color: var(--text-3);
}
.tab__n {
  font-size: var(--fs-xs);
  color: var(--text-3);
}

.rels {
  display: grid;
  gap: var(--s2);
}
.rel {
  display: flex;
  align-items: baseline;
  gap: var(--s3);
  flex-wrap: wrap;
}
.rel__who {
  flex: none;
}
.rel__why {
  color: var(--text-2);
  min-width: 0;
}
</style>
