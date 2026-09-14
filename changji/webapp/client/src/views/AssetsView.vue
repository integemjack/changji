<script setup>
/**
 * 设定。把故事变成能拍的东西。
 *
 * 三格，回答三个问题：
 *
 *     角色   谁      长什么样、什么声音
 *     场景   哪儿    什么空间、什么光、什么色
 *     分集   怎么切  每集多长 → 切成几集，每一集停在哪
 *
 * **这一页的活只有一件：看图对不对。** 2026-09-14 按项目页那条规矩重排：
 * 每次打开都要回答的问题才留在页面上。落下来是三层——
 *
 *   · **tab 就是状态行**：「角色 2 · 缺 6」「场景 25 · 缺 25」「分集 10 ·
 *     8 章没正文」。站在角色格也知道场景格一张都没画。
 *   · **右上一排是流水线**：定妆 → 出图（→ 分集格换成落成剧集）。三个都是
 *     页级动作，原来「照故事定妆」在角色格和场景格各有一份、按的是同一个
 *     接口。「覆盖已有」一个勾管两个按钮——对定妆和出图它是同一个意思：
 *     已有的也顶掉。勾上，定妆变「重新定妆」、出图变「全部重画」。
 *   · **格子里只剩图和一个搜索框。** 刷新（页面订着 refs 频道，画完自动
 *     刷）、只看缺图（一键出图本来就把缺的全画了，没人需要手动找）、卡片上
 *     和图说同一件事的 pill（0/3、有空景图）——都删了。
 *
 * **分成 tab 不是分成三页。** 三格共享同一个项目、同一份故事，切 tab 不该
 * 换地址、也不该重新拉一遍——它们是同一件事的三个面。tab 记在
 * `?tab=` 上，刷新和分享链接才停在原地。
 *
 * 人物关系不再横在角色墙上头。它是定妆的依据，依据该挨着它服务的那个人
 * ——进了每个角色的抽屉，只列跟这个人有关的那几条。
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
import { useRefStream } from '@/composables/useRefStream'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const route = useRoute()
const router = useRouter()
const ui = useUi()
const { run, isBusy } = useAction()
const { finished, touch } = useRefStream()

const story = ref(null)
/**
 * 资产库，只为 tab 上那几个数。
 *
 * 两格各自也拉一份，这儿是第三份——但 tab 上的数是这一页的答案，不能
 * 等用户切到那一格才知道。三份读的是同一个接口、同一时刻，靠 refs
 * 频道的 `finished` 一起重拉，不会各说各的。
 */
const assets = ref(null)

/** 一键出图跑到第几张。空 = 没在跑。 */
const bulk = ref(null)

/**
 * 「覆盖已有」。**一个勾，两个按钮。**
 *
 * 对定妆：同名的角色/场景用新出的顶掉旧的（手改的设定和参考图会丢）。
 * 对出图：已经有的也重画（挑过的、手传的会被顶掉）。
 * 两件事说的都是"已有的也不放过"，所以是同一个勾。**勾不是全部的防线**：
 * 两个动作勾上之后都照旧弹确认框，勾只是让状态按之前就看得见。
 */
const overwrite = ref(false)

const SLOTS = ['front', 'three_quarter', 'back']

const characters = computed(() => assets.value?.characters ?? [])
const locations = computed(() => assets.value?.locations ?? [])
const chapters = computed(() => story.value?.chapters ?? [])
const plan = computed(() => story.value?.plan ?? [])
const relations = computed(() => story.value?.relations ?? [])

/** 还差几张脸。三视图一人三张，缺一张算一张。 */
const charMissing = computed(() =>
  characters.value.reduce(
    (n, c) => n + SLOTS.filter((s) => !c['ref_' + s]).length,
    0,
  ),
)
const locMissing = computed(() => locations.value.filter((l) => !l.ref_empty).length)
const unwritten = computed(
  () => chapters.value.filter((c) => !(c.text ?? '').trim()).length,
)

/**
 * tab 上写什么。**数是答案，缺才是重点**——齐了就只有一个数，
 * 缺才多一截。空的格子（还没定妆、还没分集）只有名字。
 */
const TABS = computed(() => [
  {
    key: 'characters',
    label: '角色',
    n: characters.value.length,
    gap: charMissing.value ? `缺 ${charMissing.value}` : '',
  },
  {
    key: 'locations',
    label: '场景',
    n: locations.value.length,
    gap: locMissing.value ? `缺 ${locMissing.value}` : '',
  },
  {
    key: 'episodes',
    label: '分集',
    n: plan.value.length,
    gap: plan.value.length && unwritten.value ? `${unwritten.value} 章没正文` : '',
  },
])

const tab = computed(() => {
  const want = String(route.query.tab ?? '')
  return TABS.value.some((t) => t.key === want) ? want : 'characters'
})

function pick(key) {
  // replace 不是 push：切 tab 不该在浏览器的后退历史里堆一串
  router.replace({ query: { ...route.query, tab: key } })
}

/**
 * 照故事定妆。**一次把人和地方都定了**——引擎那边 post_bible 一次出
 * 整本圣经，回包里 added_characters 和 added_locations 都有。原来两格
 * 各放一个按钮，看着像两件事，其实按的是同一个接口。
 *
 * episode_id 带着是为老项目：没有故事的项目退回"从一集剧本里找"那条
 * 路，得知道拿哪一集。有故事的项目引擎不看它。
 */
async function bible() {
  const over = overwrite.value
  if (over) {
    // **把代价写成数字。** 「会冲掉参考图」听着像一句免责声明，而实际
    // 发生的是十几张图连同画它们的十几分钟一起没了，且没有撤销。
    // 2026-09-12 就这么丢过一次（15 张）。
    const lost =
      characters.value.reduce(
        (n, c) => n + SLOTS.filter((s) => c['ref_' + s]).length,
        0,
      ) + locations.value.filter((l) => l.ref_empty).length
    const cost = lost
      ? `会冲掉 ${lost} 张参考图（重画一遍约 ${Math.ceil((lost * 30) / 60)} 分钟），`
      : ''
    if (!confirm(`${cost}手改过的设定也会被顶掉，已渲染的镜头要重跑。继续？`)) {
      return
    }
  }
  const result = await run(
    () =>
      runAsyncJob(
        (extra) =>
          api.makeBible({
            project: session.projectPath,
            overwrite: over,
            ...(session.episodeId ? { episode_id: session.episodeId } : {}),
            ...extra,
          }),
        { prefix: 'bible', label: '照故事定妆' },
      ),
    { key: 'bible', refresh: true },
  )
  if (!result) return
  const c = result.added_characters ?? []
  const l = result.added_locations ?? []
  const parts = []
  if (c.length) parts.push(`${c.length} 个新角色：${c.join('、')}`)
  if (l.length) parts.push(`${l.length} 个新场景`)
  // 收掉了几条同名的要说出来——"场景从 25 变成 15"不解释的话看着像丢了东西
  if (result.merged) parts.push(`收掉 ${result.merged} 条重名的`)
  ui.ok(parts.length ? parts.join('；') : '故事里的人和地方库里都有了，没补新的')
  touch() // 三个格子和这儿的数一起重拉
}

/**
 * 把参考图一次画完。
 *
 * 不勾覆盖：**只补缺的，不重画已有的。** 已经画好的那些多半是挑过的——
 * 有的还是手传上去的真人照片。一键把它们全顶掉，等于一次点击毁掉半小时
 * 的挑选，而这种事没有撤销。
 *
 * 勾了覆盖：连已有的一起重画，问一句再动手。**改了画风之后需要它**：
 * 那时候在磁盘上的每一张都还是老提示词出的，只补缺的等于什么都没变——
 * 而"改了设置却看不出变化"是最容易让人以为功能坏了的一种。
 *
 * **一张一张来。** 显存只够一张，并发只会在引擎那边排队（现在是真排队
 * 了），而排着的看不出进度。
 */
async function genAll() {
  const force = overwrite.value
  // **这一读要包起来。** 它原来是裸的 `await api.assets(...)`：引擎打个嗝、
  // 项目被别处删了，这一下就抛出去成了没人接的 Promise 拒绝——按钮点下去
  // 一点反应都没有，也不报错。而下面每一张图那次调用都是包着的，只有
  // 开头这一读漏了。用同一个 key，读的那几百毫秒里按钮也是灰的。
  const data = await run(() => api.assets(session.projectPath), { key: 'genall' })
  if (!data) return
  const jobs = []
  for (const c of data.characters ?? []) {
    for (const slot of SLOTS) {
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
    // ref_done，那两格和这儿的数都订着它。见 useRefStream。
  }
  bulk.value = null
  if (made) ui.ok(`画好了 ${made} 张`)
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
    // 没有故事的老项目走到这儿是正常的，分集那格自己会说
    story.value = null
  }
}

async function loadAssets() {
  if (!session.projectPath) {
    assets.value = null
    return
  }
  try {
    assets.value = await api.assets(session.projectPath)
  } catch {
    // 刚建的项目还没有资产库。tab 上就只有名字，不该整页红。
    assets.value = null
  }
}

function loadAll() {
  loadStory()
  loadAssets()
}

onMounted(loadAll)
watch(() => session.projectPath, loadAll)
// 画完一张、定妆完——tab 上的数跟着走
watch(finished, loadAssets)
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
          <span v-if="t.n" class="tab__n">{{ t.n }}</span>
          <span v-if="t.gap" class="tab__gap">· {{ t.gap }}</span>
        </button>

        <span class="tabs__gap" />

        <!-- 流水线：定妆 → 出图。分集格不显示——它一张参考图都不出，
             而一个按下去要跑十几分钟、跟这一格毫无关系的按钮摆在那儿，
             只会让人以为它是「落成剧集」。
             ⚠️ **这儿不要再放"种子"。** 放过一次，用户 2026-09-12 说不用：
             种子是"这一张不满意，换一张脸"，是一张图的事；而这一行上的
             东西一按就是十几张，给它们定同一个种子既没意义也没人想要。
             要换某一张，那一格自己有「重画」。 -->
        <template v-if="tab !== 'episodes'">
          <label
            class="switch tiny"
            title="定妆：同名的用新出的顶掉，手改过的设定和参考图会丢。出图：已有的也重画，手传的会被顶掉"
          >
            <input v-model="overwrite" type="checkbox" :disabled="isBusy('bible') || !!bulk" />
            <span>覆盖已有</span>
          </label>

          <button
            class="btn btn--sm"
            :class="characters.length ? 'btn--ghost' : 'btn--ai'"
            type="button"
            :disabled="isBusy('bible') || !!bulk"
            title="让 AI 读一遍故事，把人和地方定下来"
            @click="bible"
          >
            <AppIcon v-if="!characters.length" name="sparkle" :size="13" />
            {{ isBusy('bible') ? '正在读故事…' : overwrite ? '重新定妆' : '照故事定妆' }}
          </button>

          <button
            class="btn btn--sm btn--ai"
            type="button"
            :disabled="isBusy('genall') || isBusy('bible') || !characters.length"
            :title="
              overwrite
                ? '连已经有的一起重画。改了画风之后用——已有的图还是老提示词出的'
                : '把还缺的参考图一次画完。已经有的不动——那些多半是挑过的'
            "
            @click="genAll"
          >
            <AppIcon name="sparkle" :size="13" />
            <template v-if="bulk">
              {{ bulk.at }}/{{ bulk.total }} {{ bulk.name }}
              <span v-if="bulk.pct" class="numeric">{{ bulk.pct }}%</span>
            </template>
            <template v-else>{{ overwrite ? '全部重画' : '一键出图' }}</template>
          </button>
        </template>
      </nav>

      <!-- KeepAlive：切回来时还停在原来展开的那个角色上。三格各自都有
           一堆展开状态和没保存的编辑，切一下就丢的话没人敢切。 -->
      <KeepAlive>
        <AssetCharacters v-if="tab === 'characters'" :relations="relations" />
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
/* 「缺 6」那一截。比数字再轻一档——它是提醒，不是标题 */
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
