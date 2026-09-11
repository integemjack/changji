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

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import AssetCharacters from '@/views/assets/AssetCharacters.vue'
import AssetEpisodes from '@/views/assets/AssetEpisodes.vue'
import AssetLocations from '@/views/assets/AssetLocations.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { runAsyncJob } from '@/composables/useAsyncJob'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const route = useRoute()
const router = useRouter()
const ui = useUi()
const { run, isBusy } = useAction()
const story = ref(null)

/** 一键出图跑到第几张。空 = 没在跑。 */
const bulk = ref(null)

/**
 * 把参考图一次画完。
 *
 * `force = false`（「一键出图」）：**只补缺的，不重画已有的。** 已经画好的
 * 那些多半是挑过的——有的还是手传上去的真人照片。一键把它们全顶掉，等于
 * 一次点击毁掉半小时的挑选，而这种事没有撤销。
 *
 * `force = true`（「全部重画」）：连已有的一起重画，问一句再动手。
 * **改了画风之后需要它**：那时候在磁盘上的每一张都还是老提示词出的，
 * 只补缺的等于什么都没变——而"改了设置却看不出变化"是最容易让人以为
 * 功能坏了的一种。
 *
 * **一张一张来。** 显存只够一张，并发只会在引擎那边排队（现在是真排队
 * 了），而排着的看不出进度。
 */
async function genAll(force = false) {
  const data = await api.assets(session.projectPath)
  const jobs = []
  for (const c of data.characters ?? []) {
    for (const slot of ['front', 'three_quarter', 'back']) {
      if (force || !c['ref_' + slot]) {
        jobs.push({ kind: 'char', id: c.char_id, slot, name: c.name || c.char_id })
      }
    }
  }
  for (const l of data.locations ?? []) {
    if (force || !l.ref_empty) {
      jobs.push({ kind: 'loc', id: l.location_id, name: l.name || l.location_id })
    }
  }
  if (!jobs.length) {
    ui.ok('参考图都齐了。要换某一张，在那一格点「重画」')
    return
  }
  if (
    force &&
    !confirm(
      `会把 ${jobs.length} 张参考图全部重画，手传上去的也会被顶掉。` +
        `一张几十秒，大约 ${Math.ceil((jobs.length * 30) / 60)} 分钟。确定？`,
    )
  ) {
    return
  }

  let made = 0
  for (let i = 0; i < jobs.length; i += 1) {
    const j = jobs[i]
    bulk.value = { at: i + 1, total: jobs.length, name: j.name, pct: 0 }
    const ok = await run(
      () =>
        runAsyncJob(
          (extra) =>
            j.kind === 'char'
              ? api.generateReference({
                  project: session.projectPath,
                  char_id: j.id,
                  slot: j.slot,
                  ...extra,
                })
              : api.generateLocationReference({
                  project: session.projectPath,
                  location_id: j.id,
                  ...extra,
                }),
          {
            prefix: 'ref',
            onProgress: (cur, total) => {
              if (bulk.value) bulk.value.pct = total > 0 ? Math.round((cur / total) * 100) : 0
            },
          },
        ),
      { key: 'genall' },
    )
    // 中间砸了就停：后面那些多半栽在同一件事上（模型没配、显存不够），
    // 接着画只是让人多等十几分钟再看到同一句报错。
    if (!ok) break
    made += 1
    // 不用在这儿招呼两格重拉：引擎画完每一张都会往 refs 频道播一条
    // ref_done，那两格订着它。见 useRefStream。
  }
  bulk.value = null
  if (made) ui.ok(`画好了 ${made} 张`)
}

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

        <!-- 出图的两个总开关。摆在这一行右边，因为它们管的是整页，
             不属于某一格。
             ⚠️ **这儿不要再放"种子"。** 放过一次，用户 2026-09-12 说不用：
             种子是"这一张不满意，换一张脸"，是一张图的事；而这一行上的
             东西一按就是十几张，给它们定同一个种子既没意义也没人想要。
             要换某一张，那一格自己有「重画」。 -->
        <span class="tabs__gap" />

        <button
          class="btn btn--sm btn--ai"
          type="button"
          :disabled="isBusy('genall')"
          title="把还缺的参考图一次画完。已经有的不动——那些多半是挑过的"
          @click="genAll(false)"
        >
          <AppIcon name="sparkle" :size="13" />
          <template v-if="bulk">
            {{ bulk.at }}/{{ bulk.total }} {{ bulk.name }}
            <span v-if="bulk.pct" class="numeric">{{ bulk.pct }}%</span>
          </template>
          <template v-else>一键出图</template>
        </button>

        <!-- 改了画风之后要用它：那时候磁盘上每一张都还是老提示词出的，
             只补缺的等于什么都没变。**单独一个按钮而不是一个开关**——
             它会顶掉手传的照片，那种事不该藏在一个勾选框后面。 -->
        <button
          v-if="!bulk"
          class="btn btn--sm btn--ghost"
          type="button"
          :disabled="isBusy('genall')"
          title="连已经有的一起重画。改了画风之后用——已有的图还是老提示词出的"
          @click="genAll(true)"
        >
          全部重画
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
.tabs__gap {
  flex: 1 1 auto;
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
