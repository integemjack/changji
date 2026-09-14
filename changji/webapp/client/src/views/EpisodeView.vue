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
import { computed, nextTick, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import EmptyState from '@/components/EmptyState.vue'
import EpFilm from '@/views/episode/EpFilm.vue'
import EpPublish from '@/views/episode/EpPublish.vue'
import EpScript from '@/views/episode/EpScript.vue'
import EpShots from '@/views/episode/EpShots.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useRun } from '@/stores/run'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const runner = useRun()
const ui = useUi()
const route = useRoute()
const router = useRouter()
const { run, isBusy } = useAction()

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

/**
 * 第几趟。**回来晚了的那趟不许写。**
 *
 * 这一层四个请求一起发，而叫它的地方有四个：换项目、换集、切 tab、跑完
 * 一轮。顶上连着点两集（`session.refresh()` 之后集号会跳），两趟都在路上
 * ——outputs 那一趟扫的是整个成片目录，比另外三趟慢得多，慢的那趟后落地
 * 就把**上一集**的字数、镜头数、缺几张首帧写在这一集的标签上。而这行数正
 * 是"站在剧本格也知道镜头到哪一步"的全部依据，错了没有别处对得出来。
 *
 * 清空那一支也要占一个号：不占的话，删完集清空之后，在路上的那趟回来又把
 * 数写回去了。
 */
let loadSeq = 0
/** 现在标签上这几个数是哪一集的。换集要先擦，别让上一集的数挂着。 */
let shownFor = null

async function load() {
  const mine = ++loadSeq
  const proj = session.projectPath
  const epId = session.episodeId
  // **换了一集，先擦。** 趟号只挡住"回来晚了别写"，挡不住这一两秒里标签
  // 上印着什么——不擦的话 ep02 的标签上写着 ep01 的「剧本 1232 字」「镜头
  // 16 · 差 16 首帧」。擦成 0 之后标签只剩名字（这页本来就有这个分支：没
  // 剧本、没分镜时只显示名字），那是此刻唯一说得准的。
  // 切 tab、跑完一轮那两条叫进来时集号没变，不擦，也就不闪。
  const key = `${proj}\u0000${epId}`
  if (key !== shownFor) {
    shownFor = key
    scriptChars.value = 0
    shots.value = []
    outputs.value = 0
    loaded.value = false
  }
  if (!proj || !epId) {
    scriptChars.value = 0
    shots.value = []
    outputs.value = 0
    loaded.value = false
    return
  }
  const [sc, sh, out, ps] = await Promise.allSettled([
    api.getScript(proj, epId),
    api.shots(proj, epId),
    api.outputs(proj),
    api.platforms(),
  ])
  if (mine !== loadSeq) return
  scriptChars.value =
    sc.status === 'fulfilled' ? [...String(sc.value?.script ?? '').trim()].length : 0
  shots.value = sh.status === 'fulfilled' ? (sh.value?.shots ?? []) : []
  const files = out.status === 'fulfilled' ? (out.value?.files ?? []) : []
  outputs.value = files.filter((f) => String(f.name ?? '').includes(epId)).length
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

// ---------------------------------------------------------------------------
// 这一集自己的那几件事：改名、复制一份、删掉
// ---------------------------------------------------------------------------
//
// 引擎那头一直有（`/api/episode/action` 收 delete / duplicate / rename），
// 界面上却只有「手动加一集」——**能建不能删**。手滑多建一集、试拍一版想
// 留个副本、把「第 3 集」改叫「第 3 集 · 天台」，三件事以前都只能去改
// project.json。
//
// **放在这一页而不是分集那一格**：分集那一格画的是故事切在哪儿（计划），
// 这三件事动的是剧集本身（已经落下来的那份），而"哪一集"正是这一页的主语。

const menuOpen = ref(false)
const renaming = ref(false)
const newTitle = ref('')
const removing = ref(false)
const titleBox = ref(null)

/** 顶栏挑中的那一集。改名和删除的确认文案都要它。 */
const ep = computed(() => session.episode)
const epName = computed(() => ep.value?.title || session.episodeId)

function closeMenu() {
  menuOpen.value = false
}

async function startRename() {
  menuOpen.value = false
  removing.value = false
  renaming.value = true
  newTitle.value = ep.value?.title ?? ''
  await nextTick()
  titleBox.value?.focus()
  titleBox.value?.select()
}

async function commitRename() {
  const title = newTitle.value.trim()
  if (!renaming.value) return
  renaming.value = false
  if (title === (ep.value?.title ?? '')) return
  const done = await run(
    () =>
      api.episodeAction({
        project: session.projectPath,
        episode_id: session.episodeId,
        action: 'rename',
        new_title: title,
      }),
    { key: 'epRename', success: '改好了' },
  )
  if (done) await session.refresh()
}

/**
 * 复制一份。**产出物和状态不跟过去**（引擎那边就是这么做的）——复制出来的
 * 一集是要重跑的，带着状态过去会显示成已完成而点播放是黑的。
 */
async function duplicate() {
  menuOpen.value = false
  const made = await run(
    () =>
      api.episodeAction({
        project: session.projectPath,
        episode_id: session.episodeId,
        action: 'duplicate',
      }),
    { key: 'epDup' },
  )
  if (!made) return
  await session.refresh()
  session.selectEpisode(made.episode_id)
  ui.ok(`复制成 ${made.episode_id}，${made.shots} 个镜头都要重跑`)
}

function askRemove() {
  menuOpen.value = false
  renaming.value = false
  removing.value = true
}

/**
 * 删掉这一集。
 *
 * **删完必须先把集号清空再 refresh。** `/bff/flow` 只在没给 episode_id 时
 * 才回落到第一集；给了一个已经不存在的，它照样把这个 id 回给前端，而
 * `session.refresh()` 看见 id 没变就不换——页面会停在一个空壳上，tab 上
 * 全是 0，人以为东西都没了。
 */
async function remove() {
  const gone = session.episodeId
  const done = await run(
    () =>
      api.episodeAction({
        project: session.projectPath,
        episode_id: gone,
        action: 'delete',
      }),
    { key: 'epDelete', success: `${gone} 删掉了` },
  )
  if (!done) return
  removing.value = false
  session.selectEpisode('')
  await session.refresh()
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

/**
 * 跑完一轮也要重拉。
 *
 * tab 上那几个数（「差 3 首帧」「差 12 视频」「成片 2」）用的是**这一层
 * 自己拉的那份镜头表**——`useShots` 的状态是每次调用新建的，镜头页里那份
 * 和这儿这份是两个。而这儿原来只在换项目、换集、切 tab 时重拉。
 *
 * 于是出片那十几二十分钟里，底下的墙一格一格亮起来，上面那行始终写着
 * 「差 12 视频」；跑完了也不动，非得切一下 tab 才对。同一屏上两个数说
 * 同一件事，而其中一个是二十分钟前的。
 *
 * 只订"跑完那一下"，不跟着每一镜刷——这一层一次要发四个请求，而跑的过程
 * 中真正要看的是墙，不是标签。出片写盘也正好发生在这一刻，「成片」那个数
 * 跟着一起对上。写整季那条（writer）不订：它动的是故事页那边的东西。
 */
watch(
  () => runner.running,
  (now, before) => {
    if (before && !now) load()
  },
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
          <span v-if="v.n" class="tab__n">{{ v.n }}</span>
          <span v-if="v.gap" class="tab__gap">· {{ v.gap }}</span>
        </button>

        <span class="spacer" />

        <!-- 这一集自己的那几件事。收在 ⋯ 里：三件都是偶尔才做一次的，
             摆成三个按钮会和左边四个 tab 抢同一条横线上的注意力。 -->
        <input
          v-if="renaming"
          ref="titleBox"
          v-model="newTitle"
          class="input ep__rename"
          :placeholder="session.episodeId"
          @keydown.stop.enter="commitRename"
          @keydown.stop.esc="renaming = false"
          @blur="commitRename"
        />
        <div v-else class="ep__more">
          <button class="tab tab--more" type="button" title="改名、复制、删掉" @click="menuOpen = !menuOpen">
            ⋯
          </button>
          <template v-if="menuOpen">
            <div class="menu__veil" @click="closeMenu" />
            <div class="menu__pop">
              <button class="menu__item" type="button" @click="startRename">改名</button>
              <button
                class="menu__item"
                type="button"
                :disabled="isBusy('epDup')"
                @click="duplicate"
              >
                {{ isBusy('epDup') ? '复制中…' : '复制一份' }}
              </button>
              <button
                class="menu__item menu__item--danger"
                type="button"
                :disabled="session.episodes.length <= 1"
                :title="session.episodes.length <= 1 ? '至少要留一集' : ''"
                @click="askRemove"
              >
                删掉
              </button>
            </div>
          </template>
        </div>
      </nav>

      <!-- 就地确认。**把要丢的东西按数说出来**——这一页正好已经拿着这三个
           数（tab 上写着的就是它们），比"确定要删除吗"有用得多。 -->
      <div v-if="removing" class="ep__confirm">
        <span class="small">
          删掉「{{ epName }}」？
          <template v-if="scriptChars || shots.length">
            <b>{{ scriptChars }} 字剧本</b>、<b>{{ shots.length }} 个镜头</b>一起没了，
          </template>
          已经出好的成片文件留在磁盘上，不会被删。
        </span>
        <span class="spacer" />
        <button
          class="btn btn--danger btn--sm"
          type="button"
          :disabled="isBusy('epDelete')"
          @click="remove"
        >
          永久删除
        </button>
        <button class="btn btn--ghost btn--sm" type="button" @click="removing = false">
          算了
        </button>
      </div>

      <!-- keep-alive：切回镜头那一屏时轮询、选中和抽屉都还在。
           `script-chars` 在读完之前给 null，不是 0——镜头页拿它分辨"这一集
           还没有剧本"和"还不知道"，理由见那边那个空状态。 -->
      <KeepAlive>
        <component
          :is="current.comp"
          :key="current.key"
          :can-publish="publishOk"
          :script-chars="loaded ? scriptChars : null"
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

/* ⋯ 那一颗和它的浮层 */
.ep__more {
  position: relative;
}

.tab--more {
  letter-spacing: 2px;
}

.ep__rename {
  max-width: 14rem;
}

.menu__veil {
  position: fixed;
  inset: 0;
  z-index: 40;
}

.menu__pop {
  position: absolute;
  top: 100%;
  right: 0;
  z-index: 41;
  display: flex;
  flex-direction: column;
  min-width: 7rem;
  margin-top: 4px;
  padding: 4px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: var(--surface);
  box-shadow: 0 6px 20px rgb(0 0 0 / 28%);
}

.menu__item {
  padding: 6px 10px;
  border: 0;
  border-radius: 6px;
  background: transparent;
  color: var(--text);
  font-size: 13px;
  text-align: left;
  cursor: pointer;
}

.menu__item:hover:not(:disabled) {
  background: var(--surface-2);
}

.menu__item:disabled {
  color: var(--text-3);
  cursor: not-allowed;
}

.menu__item--danger {
  color: var(--danger);
}

/* 就地确认那一条。跨整行，别挤在 tab 那条线上 */
.ep__confirm {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--s2);
  padding: 8px 10px;
  border: 1px solid var(--danger);
  border-radius: 8px;
}
</style>
