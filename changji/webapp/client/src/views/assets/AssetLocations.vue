<script setup>
/**
 * 第四步：场景。分集阶段的第一步。
 *
 * 场景库是全剧共用的一份——分镜表里只存 location_id，空间和光线的描述
 * 由程序从库里拼接，逐字节相同。每集各存一份拷贝的话，同一个安保室在
 * 第一集和第五集会长得不一样，而这正是这套系统要防的事。
 *
 * 所以这一页不是「这一集自己的场景表」，而是把镜头对准库里属于这一集的
 * 那几个：出分镜之前，AI 按这一集的剧本补新场景；出了分镜之后，按镜头
 * 实际引用的 id 分成「本集用到」和「其他集的」两组。
 */
import { computed, onActivated, onDeactivated, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { useAction } from '@/composables/useAction'
import { runAsyncJob } from '@/composables/useAsyncJob'
import { useRefStream } from '@/composables/useRefStream'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

/** 进度、预览、在不在跑——都从那条固定频道来。见 useRefStream。 */
const { pct: genPct, preview, live, finished, touch } = useRefStream()

/** 引擎那边怎么叫这一格。改这里就得改 ref_gen.cpp 里拼 stem 那一行。 */
const targetOf = (id) => `${id}_empty`

/** 抽屉里改的是哪一处。墙用来挑，抽屉用来改——和角色那边同一套。 */
const openId = ref('')
/**
 * 抽屉开关时的焦点。
 *
 * 这个抽屉是 `position: fixed; inset: 0` 的全屏遮罩，和那三个弹窗一个性质：
 * 用键盘按开它之后，焦点还留在**遮罩后面**那颗按钮上，按 Tab 是在看不见的
 * 页面里走；关掉之后焦点落到 `<body>`，下一次 Tab 得从整页开头重走。
 *
 * 开：焦点放到面板本身（tabindex="-1"），不猜第一个控件。盯的是面板出现
 * 那一刻——模板 ref 是响应式的，元素挂上来就聚焦。
 * 关：还给把它叫起来的那颗按钮。**只在从"没开"到"开"时记一次**，换一条
 * （不关抽屉直接点另一个）时不重记，否则记下的会是抽屉里面的元素。
 */
const panel = ref(null)
watch(panel, (el) => el?.focus())
let opener = null
watch(openId, (now, before) => {
  if (now && !before) {
    opener = document.activeElement
    return
  }
  if (!now) {
    const back = opener
    opener = null
    back?.focus?.()
  }
})

const openLoc = computed(
  () => mine.value.find((l) => l.location_id === openId.value) ?? null,
)

function toggle(id) {
  // 改了描述直接收起来，改动就没了。先问一句。
  //
  // **角色格那边早就是这个规矩**（「这个角色有改动还没保存，收起就没了」），
  // 而这两页是同一副骨架：同样的 edits、同样的 changed()、同样的「未保存」
  // 角标、同样的放弃按钮——独独收抽屉这一下少了拦截。点开一个场景把空间、
  // 光线、色彩三段重写一遍，再点一下那一行（或者点开别的场景）就全没了。
  if (openId.value && changed(openId.value)) {
    if (!confirm('这个场景有改动还没保存，收起就没了。确定？')) return
    // 放弃的那一份要还原，否则「未保存」的角标会一直挂在列表上
    const was = locations.value.find((l) => l.location_id === openId.value)
    if (was) edits.value[openId.value] = { ...was }
  }
  openId.value = openId.value === id ? '' : id
}

function onEsc(e) {
  // 抽屉盖着半个屏幕，而鼠标多半正停在里面——Esc 是唯一不用先瞄准的出口。
  if (e.key === 'Escape' && openId.value) openId.value = ''
}


const assets = ref(null)
const shots = ref([])
const loading = ref(false)

/**
 * 筛这一格。
 *
 * **25 个场景、名字前缀还高度雷同**（「临川大学旧礼堂观众席第一排 /
 * 最后一排 / 后台 / 舞台中央…」），翻着找很难受；而真正要找的多半是
 * "哪些还没画"。所以两样：一个搜索框，一个「只看缺图」。
 */
const q = ref('')

function match(l) {
  const k = q.value.trim().toLowerCase()
  if (k && !String(l.name || '').toLowerCase().includes(k)) return false
  return true
}

/** 名字里大家共用的那一截前缀。扫这一格时要认的是尾巴。 */
const prefix = computed(() => {
  const names = locations.value.map((l) => String(l.name || ''))
  if (names.length < 3) return ''
  let p = names[0]
  for (const n of names.slice(1)) {
    let i = 0
    while (i < p.length && i < n.length && p[i] === n[i]) i += 1
    p = p.slice(0, i)
    if (!p) break
  }
  // 太短的共用前缀不值得折（「临」「临川」这种），四个字起
  return p.length >= 4 ? p : ''
})

/** 这张牌要不要缩成一半高：没图、而且这会儿也没在画。 */
function flat(l) {
  if (l.ref_empty) return false
  const t = targetOf(l.location_id)
  return !live[t] && !preview[t] && !isBusy(`gen:${l.location_id}`)
}

function head(l) {
  const n = String(l.name || '')
  return prefix.value && n.startsWith(prefix.value) ? n.slice(prefix.value.length) : n
}
const edits = ref({})
const showOthers = ref(false)

const FIELDS = [
  { key: 'space', label: '空间', hint: '房间大小、家具布置、镜头能看到什么', rows: 3 },
  { key: 'lighting', label: '光线', hint: '冷调顶光、暖调侧逆光', rows: 2 },
  { key: 'palette', label: '色彩', hint: '主色和点缀色，可留空', rows: 2 },
]

const locations = computed(() => assets.value?.locations ?? [])

const knownIds = computed(() => new Set(locations.value.map((l) => l.location_id)))

/**
 * 这一集的镜头引用了哪几个场景，各用了几镜。
 *
 * 两个字段都认：schema 里 scene_id 和 location_id 是分开的，模型十次有
 * 八次只填了 scene_id。那种镜头渲染时拿不到场景描述——不报错，只是画面
 * 里的房间每镜都不一样。下面的 unlinked 就是用来把这件事说出来的。
 */
const usage = computed(() => {
  const map = new Map()
  for (const s of shots.value) {
    const id = s.location_id || (knownIds.value.has(s.scene_id) ? s.scene_id : '')
    if (!id) continue
    map.set(id, (map.get(id) ?? 0) + 1)
  }
  return map
})

const unlinked = computed(
  () =>
    shots.value.filter((s) => !s.location_id && knownIds.value.has(s.scene_id)).length,
)

async function linkLocations() {
  const result = await run(
    () =>
      api.linkLocations({
        project: session.projectPath,
        episode_id: session.episodeId,
      }),
    { key: 'link', refresh: true },
  )
  if (!result) return
  ui.ok(
    `${result.linked} 个镜头接回了场景` +
      (result.reset_shots ? `，${result.reset_shots} 个镜头退回重跑` : ''),
  )
  await load()
}

// 还没出分镜的时候无从知道这一集用哪几个，全都算本集的
const hasShots = computed(() => shots.value.length > 0)
const mine = computed(() =>
  (hasShots.value
    ? locations.value.filter((l) => usage.value.has(l.location_id))
    : locations.value
  ).filter(match),
)
const others = computed(() =>
  (hasShots.value
    ? locations.value.filter((l) => !usage.value.has(l.location_id))
    : []
  ).filter(match),
)

/** 分镜引用了、但库里没有的场景。跑起来会直接报「场景未注册」。 */
const missing = computed(() => {
  const known = new Set(locations.value.map((l) => l.location_id))
  return [...usage.value.keys()].filter((id) => !known.has(id))
})

async function load() {
  if (!session.projectPath) return
  // 换剧时两趟会叠在一起，慢的那趟后落地就把上一部的场景摆在这一部下面
  const want = session.projectPath
  loading.value = true
  try {
    const before = edits.value
    const got = await api.assets(session.projectPath)
    if (want !== session.projectPath) return
    assets.value = got
    // ⚠️ **改了还没存的那一条不能被盖掉。**
    //
    // 这个 load 不只在换项目时跑：`watch(finished, load)` 让它在**每画完
    // 一张参考图**的时候也跑一遍。而一键出图是一张接一张、每张几十秒——
    // 抽屉里正在改外观描述的人，每隔几十秒手底下的字就被服务端那份原样
    // 盖回去一次，一声不吭。单独给某个角色出一张图也一样。
    //
    // 照故事页那条既有规矩办（`setStory` 只刷新不在 dirtySnapshot 里的章）：
    // **只刷新没改过的那几条**。先把新的资产库装上，`changed()` 比的就是
    // 「手里这份」和「刚拿回来这份」，正好是要的判据。
    //
    // 空景图缩略图不受影响：它们绑的是 `l.ref_empty`，从 assets
    // 来，不从 edits 来——所以留着草稿不会把刚画完的图冻在旧的上。
    edits.value = Object.fromEntries(
      (assets.value.locations ?? []).map((l) => [
        l.location_id,
        before[l.location_id] && changed(l.location_id) ? before[l.location_id] : { ...l },
      ]),
    )
  } catch (err) {
    if (want === session.projectPath) ui.error(err.message)
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
  try {
    const data = await api.shots(session.projectPath, session.episodeId)
    if (want !== `${session.projectPath}::${session.episodeId}`) return
    shots.value = data.shots ?? []
  } catch {
    if (want === `${session.projectPath}::${session.episodeId}`) shots.value = []
  }
}

onMounted(() => document.addEventListener('keydown', onEsc))
onUnmounted(() => document.removeEventListener('keydown', onEsc))

/**
 * **切到别的格子就把 Esc 让出去。**
 *
 * 设定那三格被 AssetsView 的 `<KeepAlive>` 冻着——切走是**停用**不是卸载，
 * `onUnmounted` 不跑，这个 keydown 照样挂在 document 上。而 `onEsc` 只看
 * 自己的 `openId` 非空，切格子并不会关抽屉：在场景格按一下 Esc，隔壁角色
 * 格那个看不见的抽屉跟着一起关了，回去才发现它自己合上了。
 *
 * 镜头页早有这道（见 EpShots 里 onActivated 那段）。重复 add 同一个函数
 * 引用是安全的（DOM 去重），首次挂载时两个钩子都跑一遍没关系。
 */
onActivated(() => document.addEventListener('keydown', onEsc))
onDeactivated(() => document.removeEventListener('keydown', onEsc))

/**
 * **换项目要先把手里那份编辑清掉。**
 *
 * `load()` 里那段「只刷新没改过的」是为 `watch(finished, load)` 写的——
 * 一键出图一张接一张，每张几十秒就重拉一次，抽屉里正在改字的人不能被
 * 服务端那份一遍遍盖回去。
 *
 * 但**换项目走的是同一个 load**，而 edits 是按 id 索引的：两部剧里出现
 * 同一个 id 不是稀奇事（id 是照名字生成的，续集、复制出来的项目、同名
 * 角色都会撞），撞上的那一条会被当成"这条人家改过、别刷新"，于是上一部
 * 的外观描述留在这一部的抽屉里，一存就写进去了。
 *
 * 所以换项目这条路先清空再读：那时候"手里改了还没存的"本来就属于上一部，
 * 冲不冲得掉是另一回事（这一页没有自动保存，走开就是丢），但绝不能跟着
 * 到下一部去。抽屉也一并收起来——它开着的是上一部那一条。
 */
watch(
  () => [session.projectPath, session.episodeId],
  ([project], old) => {
    // 只在**换项目**时清。换集不清：场景库是全剧共用的一份，换一集只是
    // 换了"这一集用到哪几个"的分组，手里改着的那条还在同一部剧里。
    if (old && project !== old[0]) {
      edits.value = {}
      openId.value = ''
    }
    load()
  },
  { immediate: true },
)
watch(finished, load)   // 画完一张就重拉，刷新过页面的人只剩这条路

function changed(id) {
  const now = edits.value[id]
  const was = locations.value.find((l) => l.location_id === id)
  if (!now || !was) return false
  return ['name', ...FIELDS.map((f) => f.key)].some(
    (k) => (now[k] ?? '') !== (was[k] ?? ''),
  )
}

/**
 * 照故事给这一集的地方定妆。
 *
 * **这一步不创作新的地方**——地方是故事里定的，这里只是把故事里那份名单
 * 翻成"长什么样"：空间结构、光线基调、色彩方案。
 *
 * 默认只补还没定过妆的：库里已有的同名场景保住，手改过的描述和传过的
 * 空景图都还在。勾了覆盖才让新出的顶掉，那会把全剧镜头退回重跑。
 */

async function save(id) {
  const draft = edits.value[id]
  const patch = {}
  for (const key of ['name', ...FIELDS.map((f) => f.key)]) {
    if (draft[key] !== undefined && draft[key] !== null) patch[key] = draft[key]
  }
  const result = await run(
    () => api.saveLocation({ project: session.projectPath, location_id: id, patch }),
    { key: 'save:' + id, refresh: true },
  )
  if (!result) return
  ui.ok(result.reset_shots ? `已保存，${result.reset_shots} 个镜头退回重跑` : '已保存')
  await load()
}

async function uploadEmpty(locationId, event) {
  const file = event.target.files?.[0]
  if (!file) return
  const form = new FormData()
  form.append('project', session.projectPath)
  form.append('location_id', locationId)
  form.append('file', file)
  const result = await run(() => api.uploadLocationReference(form), {
    key: 'up:' + locationId,
  })
  event.target.value = ''
  if (!result) return
  // 同角色参考图那处：没退就别提退，0 在这一页是常态。
  ui.ok(
    `空景图已存（${result.size_kb} KB）` +
      (result.reset_shots ? `，${result.reset_shots} 个镜头退回重跑` : ''),
  )
  // **招呼一声就够。** 传图和撤图都不走 `ref_done`（那条只有出图才发），
  // 而这一格和设定页 tab 上那个「缺 N」都订着 finished——touch 一下两边
  // 一起重拉。自己再 `await load()` 的话，同一个 /api/assets 打两次。
  // `bible()` 那儿用的就是这个写法。touch 的注释里说的「资产库变了但不是
  // 画完一张」，就是这两下。
  touch()
}

/**
 * 照着这个场景那段"拼出来的提示词"画一张空景图。
 *
 * **这是这一页上唯一真正在用 AI 的地方**：地方是故事里定的，这一页只把它
 * 翻成"长什么样"，再照着画出来。
 *
 * 画出来的**里面没有人**——空景图是场景一致性的锚点，后面每一镜都拿它当
 * 底子。里面站个人的话，那个人会被当成这个地方的一部分一路复制下去，而
 * 那是个不属于任何角色、也没法改的人。
 */
async function genEmpty(locationId) {
  const result = await run(
    () =>
      runAsyncJob(
        (extra) =>
          api.generateLocationReference({
            project: session.projectPath,
            location_id: locationId,
            ...extra,
          }),
        // 进度和预览都从固定频道来（useRefStream），这儿不用再接一遍。
        { prefix: 'ref' },
      ),
    { key: 'gen:' + locationId },
  )
  if (!result) return
  // 同角色格：出图也会退镜头，回包里有数。
  ui.ok(
    `空景图画好了（${Math.round(result.seconds)} 秒）` +
      (result.reset_shots ? `，${result.reset_shots} 个镜头退回重跑` : ''),
  )
  await load()
}

async function clearEmpty(locationId) {
  const result = await run(
    () =>
      api.clearLocationReference({
        project: session.projectPath,
        location_id: locationId,
      }),
    { key: 'clr:' + locationId },
  )
  if (result) {
    // 同角色格那处：撤图会把已渲染的镜头退回待跑，得说出来。
    ui.ok(
      result.cleared === false
        ? '这个场景本来就没有空景图'
        : '已撤掉空景图' +
            (result.reset_shots ? `，${result.reset_shots} 个镜头退回重跑` : ''),
    )
    touch() // 同 upload：撤图不发 ref_done，这一格和「缺 N」都靠它重拉
  }
}
</script>

<template>
  <div class="locs">
    <!-- 场景库是全剧共用的一份，分镜表里只存 id。这一页按「本集用到的」分组。
         工具行：刷新、重定、照故事定。 -->
    <!-- 只剩一个搜索框，理由见 AssetCharacters 里同一段。出了分镜之后
         墙上只有本集用到的，那时候得说一句——不然 25 个场景只见 6 个，
         像丢了。 -->
    <div class="toolbar">
      <span v-if="hasShots" class="tiny dim">本集用到 {{ mine.length }} 个</span>
      <span class="spacer" />
      <input v-model="q" class="input find" placeholder="找场景" />
    </div>

    <!-- **不要在这儿套一个光秃秃的 <template>**：Vue 只把带
         v-if/v-for/v-slot 的 template 当片段，没有指令的会当成真的
         HTML template 元素渲染出去——浏览器默认 display:none，
         内容全在 DOM 里、一个字不报错，就是看不见。栽过一次。 -->
      <div v-if="unlinked" class="alert alert--warn">
        <AppIcon name="warn" :size="14" />
        <span title="只填了 scene_id 没接到场景的镜头，渲染时拿不到空间和光线">
          <b class="numeric">{{ unlinked }}</b> 个镜头没接到场景
        </span>
        <button
          class="btn btn--sm"
          type="button"
          :disabled="isBusy('link')"
          @click="linkLocations"
        >
          {{ isBusy('link') ? '接上中…' : '一键接上' }}
        </button>
      </div>

      <p v-if="missing.length" class="alert alert--bad">
        <AppIcon name="warn" :size="14" />
        <span>分镜引用了 {{ missing.join('、') }}，场景库里没有</span>
      </p>

      <!-- 同角色格：故事已经有了的时候，差的那一步在这一页右上角，
           不该再把人支回故事页。判据用侧栏那个对勾同一份（done.story）。 -->
      <EmptyState
        v-if="!loading && !locations.length && !session.done.story"
        icon="scene"
        title="场景库还是空的"
        hint="先写故事，再点右上角「照故事定妆」"
      >
        <RouterLink to="/story" class="btn btn--sm">去写故事</RouterLink>
      </EmptyState>

      <EmptyState
        v-else-if="!loading && !locations.length"
        icon="scene"
        title="场景库还是空的"
        hint="故事写好了，点右上角「照故事定妆」，让 AI 读一遍把人和地方定下来"
      />

      <EmptyState
        v-else-if="!loading && !mine.length && !others.length"
        icon="search"
        title="没有对得上的"
        hint="换个词试试"
      />

      <template v-else>
        <!-- 本集场景 -->
        <section>
          <!-- 一处一张牌，和角色那面墙、和「这一集」那面镜头墙一个样子。
               以前每张牌里都摊着名字输入框和三段描述——十来个场景就是三十
               多个输入框铺在一屏上，而这一页十次有九次只是来看一眼空景图
               对不对。改的时候点开抽屉。 -->
          <div class="wall">
            <article
              v-for="l in mine"
              :key="l.location_id"
              class="cell"
              :class="{
                'cell--live': isBusy('gen:' + l.location_id) || live[targetOf(l.location_id)],
                'cell--open': openId === l.location_id,
                'cell--flat': flat(l),
              }"
            >
              <div class="cell__frame" @click="toggle(l.location_id)">
                <img
                  v-if="l.ref_empty"
                  :src="mediaUrl(session.projectPath, l.ref_empty)"
                  :alt="`${l.name} 空景图`"
                  loading="lazy"
                />
                <div v-else class="loc__blank">
                  <AppIcon name="image" :size="20" />
                  <span class="tiny">没有空景图</span>
                </div>

                <!-- 采样中途那张小图。见 AssetCharacters 里同一段。 -->
                <img
                  v-if="preview[targetOf(l.location_id)]"
                  class="loc__preview"
                  :src="preview[targetOf(l.location_id)]"
                  alt=""
                />
                <span v-if="genPct[targetOf(l.location_id)]" class="cell__pct numeric">
                  {{ genPct[targetOf(l.location_id)] }}%
                </span>
                <span v-if="usage.get(l.location_id)" class="loc__count pill pill--neutral">
                  本集 {{ usage.get(l.location_id) }} 镜
                </span>
              </div>

              <div class="cell__bottom">
                <span
                  v-if="isBusy('gen:' + l.location_id) || live[targetOf(l.location_id)]"
                  class="cell__fill"
                  :class="{ 'cell__fill--idle': !genPct[targetOf(l.location_id)] }"
                  :style="
                    genPct[targetOf(l.location_id)]
                      ? { width: genPct[targetOf(l.location_id)] + '%' }
                      : null
                  "
                />
                <button
                  class="cell__name truncate"
                  type="button"
                  :title="l.name"
                  @click="toggle(l.location_id)"
                >
                  <span v-if="prefix" class="cell__pre">{{ prefix }}</span>{{ head(l) }}
                </button>
                <span class="spacer" />
                <!-- 「有空景图/缺图」删了：图在不在，看框里就知道 -->
                <span v-if="changed(l.location_id)" class="pill pill--warn tiny">未保存</span>
              </div>
            </article>
          </div>
        </section>

        <!-- 点开一处场景，从右边滑出来改。和角色那边同一套。 -->
        <div v-if="openLoc" class="drawer" @click.self="openId = ''">
          <aside ref="panel" class="drawer__panel" tabindex="-1">
            <header class="drawer__head">
              <b>{{ openLoc.name }}</b>
              <span class="tiny dim mono">{{ openLoc.location_id }}</span>
              <span v-if="changed(openLoc.location_id)" class="pill pill--warn tiny">未保存</span>
              <span class="spacer" />
              <button class="iconbtn" type="button" title="收起（Esc）" @click="openId = ''">✕</button>
            </header>

            <div class="drawer__body stack">
              <!-- 空景图排在最上面：这一页的活是"看看这地方对不对"。 -->
              <div class="loc__shot">
                <img
                  v-if="openLoc.ref_empty"
                  :src="mediaUrl(session.projectPath, openLoc.ref_empty)"
                  :alt="`${openLoc.name} 空景图`"
                />
                <div v-else class="loc__blank">
                  <AppIcon name="image" :size="20" />
                  <span class="tiny">没有空景图</span>
                </div>
                <img
                  v-if="preview[targetOf(openLoc.location_id)]"
                  class="loc__preview"
                  :src="preview[targetOf(openLoc.location_id)]"
                  alt=""
                />
                <div class="loc__overlay">
                  <button
                    class="btn btn--sm btn--ai"
                    type="button"
                    :disabled="
                      isBusy('gen:' + openLoc.location_id) ||
                      live[targetOf(openLoc.location_id)]
                    "
                    title="照下面那段拼出来的提示词画一张空景，里面不会有人"
                    @click="genEmpty(openLoc.location_id)"
                  >
                    <AppIcon name="sparkle" :size="13" />
                    {{
                      isBusy('gen:' + openLoc.location_id) ||
                      live[targetOf(openLoc.location_id)]
                        ? genPct[targetOf(openLoc.location_id)]
                          ? genPct[targetOf(openLoc.location_id)] + '%'
                          : '画着…'
                        : openLoc.ref_empty
                          ? '重画'
                          : '画一张'
                    }}
                  </button>
                  <label class="btn btn--sm">
                    <AppIcon name="image" :size="13" />
                    {{ openLoc.ref_empty ? '换一张' : '传空景图' }}
                    <input
                      type="file"
                      accept="image/png,image/jpeg,image/webp"
                      hidden
                      @change="uploadEmpty(openLoc.location_id, $event)"
                    />
                  </label>
                  <button
                    v-if="openLoc.ref_empty"
                    class="btn btn--sm btn--ghost"
                    type="button"
                    @click="clearEmpty(openLoc.location_id)"
                  >
                    撤掉
                  </button>
                </div>
              </div>

              <label class="field">
                <span class="field__label">名字</span>
                <input v-model="edits[openLoc.location_id].name" class="input" />
              </label>

              <label v-for="f in FIELDS" :key="f.key" class="field" :title="f.hint">
                <span class="field__label">{{ f.label }}</span>
                <textarea
                  v-model="edits[openLoc.location_id][f.key]"
                  class="textarea textarea--tight"
                  :rows="f.rows"
                  :placeholder="f.hint"
                />
              </label>

              <!-- **折起来。** 这是排障用的——出来的图不对时，翻开看一眼
                   真正发给画图模型的那一串。平时它是一整段灰字，白占抽屉里
                   三分之一的高度，而抽屉是用来改描述的。 -->
              <details class="fold" title="每个镜头拿到的都是这一串">
                <summary class="fold__t">拼出来的提示词</summary>
                <p class="rendered mono">{{ openLoc.rendered }}</p>
              </details>
            </div>

            <div class="drawer__foot">
              <button
                class="btn btn--primary btn--sm"
                type="button"
                :disabled="!changed(openLoc.location_id) || isBusy('save:' + openLoc.location_id)"
                title="改了描述，已渲染的镜头会退回重跑"
                @click="save(openLoc.location_id)"
              >
                保存
              </button>
              <button
                class="btn btn--ghost btn--sm"
                type="button"
                :disabled="!changed(openLoc.location_id)"
                @click="edits[openLoc.location_id] = { ...openLoc }"
              >
                撤销
              </button>
            </div>
          </aside>
        </div>


        <!-- 其他集的场景 -->
        <section v-if="others.length" class="stack stack--sm">
          <button
            class="fold"
            type="button"
            title="同一个库，这一集没用到"
            @click="showOthers = !showOthers"
          >
            <AppIcon :name="showOthers ? 'arrowLeft' : 'arrowRight'" :size="14" />
            <span>其他集的场景</span>
            <span class="tab__n">{{ others.length }}</span>
          </button>

          <div v-if="showOthers" class="grid grid--locs">
            <article v-for="l in others" :key="l.location_id" class="loc loc--dim">
              <div class="loc__shot loc__shot--slim">
                <img
                  v-if="l.ref_empty"
                  :src="mediaUrl(session.projectPath, l.ref_empty)"
                  :alt="l.name"
                  loading="lazy"
                />
                <div v-else class="loc__blank"><AppIcon name="image" :size="16" /></div>
              </div>
              <div class="loc__head">
                <span class="strong truncate">{{ l.name }}</span>
                <span class="tiny dim mono nowrap">{{ l.location_id }}</span>
              </div>
              <div class="loc__body">
                <p class="rendered mono">{{ l.rendered }}</p>
              </div>
            </article>
          </div>
        </section>
      </template>
  </div>
</template>

<style scoped>
.locs {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
}

/* 出了问题才有的一行。 */
.alert {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin: 0;
  font-size: var(--fs-sm);
}
.alert--bad {
  color: var(--danger);
}
.alert--warn {
  color: var(--warn);
}
.alert--warn b {
  font-weight: 700;
}

.grid--locs {
  grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
  align-items: start;
}

/* 一处一张牌。和角色那面墙、和「这一集」那面镜头墙同一套——三处的尺寸
   和类名刻意长一样，改一处就该想到另外两处。 */
.wall {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(260px, 1fr));
  gap: var(--s3);
  align-items: start;
}
.cell {
  border: 1px solid var(--line);
  border-radius: var(--r);
  overflow: hidden;
  background: var(--surface);
}
.cell--live { border-color: var(--accent); }
.cell--open { outline: 2px solid var(--accent); }
/* **一个占位符不需要 16:9 的框。** 25 个没画的场景按原尺寸排是 2258px，
   缩成一半之后约 700px——而这一页真正要扫的就是"哪些还没画"。 */
.cell--flat .cell__frame {
  aspect-ratio: auto;
  height: 54px;
}
.cell--flat .loc__blank .tiny { display: none; }
/* 空景图给人看的是空间，宽一点看得清家具位置——所以是 16:9，
   不跟角色那边的 9:16。成片是竖屏，但这一格不是成片。 */
.cell__frame {
  position: relative;
  display: block;
  width: 100%;
  aspect-ratio: 16 / 9;
  background: var(--bg-sunken);
  color: var(--text-3);
  cursor: pointer;
}
.cell__frame img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.cell__pct {
  position: absolute;
  left: 50%;
  top: 50%;
  transform: translate(-50%, -50%);
  padding: 1px 6px;
  border-radius: 999px;
  background: color-mix(in srgb, var(--bg) 75%, transparent);
  color: var(--accent);
  font-size: var(--fs-xs);
  pointer-events: none;
}
.cell__bottom {
  position: relative;
  display: flex;
  align-items: center;
  gap: 4px;
  padding: 4px 6px;
  border-top: 1px solid var(--line);
}
.cell__fill {
  position: absolute;
  inset: 0 auto 0 0;
  background: var(--accent-soft);
  pointer-events: none;
}
.cell__fill--idle {
  right: 0;
  animation: cell-sweep 1.4s ease-in-out infinite;
}
@keyframes cell-sweep {
  0%, 100% { opacity: 0.25; }
  50% { opacity: 0.7; }
}
.cell__name {
  position: relative;
  border: 0;
  background: transparent;
  color: var(--text-1);
  font-size: var(--fs-sm);
  font-weight: 600;
  cursor: pointer;
  padding: 0;
  min-width: 0;
}
.cell__bottom .pill { position: relative; }
/* 共用的那一截前缀退成小字灰字，大字留给区别的那半截 */
.cell__pre {
  color: var(--text-3);
  font-weight: 400;
  font-size: var(--fs-xs);
}

/* 搜索框不该和按钮抢地方：够打四五个字就行 */
.find {
  width: 8rem;
  padding: 2px 8px;
  font-size: var(--fs-xs);
}

/* 抽屉。和角色那边同一套尺寸。 */
.drawer {
  position: fixed;
  inset: 0;
  z-index: 40;
  background: color-mix(in srgb, black 45%, transparent);
  display: flex;
  justify-content: flex-end;
}
.drawer__panel {
  display: flex;
  flex-direction: column;
  width: min(96vw, 720px);
  height: 100%;
  background: var(--surface);
  border-left: 1px solid var(--line);
}
.drawer__head,
.drawer__foot {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3);
}
.drawer__head { border-bottom: 1px solid var(--line); }
.drawer__foot { border-top: 1px solid var(--line); }
.drawer__body {
  flex: 1;
  overflow-y: auto;
  padding: var(--s3);
}
/* **flex: none，否则那张图会被压成一条。** drawer__body 是 flex 竖排，
   而 `aspect-ratio` 不给 flex 定基准尺寸——默认 `flex-shrink: 1` 一收，
   16:9 的空景图在这儿实测只剩 40px 高（该是 390）。而这一格恰恰是
   打开抽屉最先要看的东西。 */
.drawer .loc__shot { flex: none; }

/* 一格一个场景。图在上，格子要有边，不然图和图连成一片。 */
.loc {
  overflow: hidden;
  border: 1px solid var(--line);
  border-radius: var(--r);
}
.loc--dim {
  opacity: 0.72;
}

/* 采样中途那张预览，盖在空景图上。**pointer-events 关掉**：底下那排
   按钮（重画、换一张、撤掉）要点得着。 */
.loc__preview {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  object-fit: cover;
  pointer-events: none;
}

/* 空景图。16:9 是勘景照的常见比例，成片是竖屏，但这里给人看空间，
   宽一点看得清家具位置。 */
.loc__shot {
  position: relative;
  aspect-ratio: 16 / 9;
  background: var(--bg-sunken);
  border-bottom: 1px solid var(--line);
  overflow: hidden;
}
.loc__shot--slim {
  aspect-ratio: 21 / 9;
}
.loc__shot img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.loc__blank {
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 4px;
  color: var(--text-3);
}
.loc__overlay {
  position: absolute;
  inset: auto 0 0 0;
  display: flex;
  gap: var(--s2);
  padding: var(--s2);
  background: linear-gradient(transparent, rgba(0, 0, 0, 0.65));
  opacity: 0;
  transition: opacity 0.15s var(--ease);
}
.loc:hover .loc__overlay,
.loc:focus-within .loc__overlay {
  opacity: 1;
}
/* **抽屉里那一排一直显示。** 它是靠 `.loc:hover` 露出来的，而抽屉里的图
   外面没有 `.loc` 这层壳——不写这一条的话「画一张 / 换一张 / 撤掉」永远
   是透明的，点得着但看不见，等于没有。
   而且抽屉本来就是"进来改东西"的地方，不用再藏一道。 */
.drawer .loc__overlay { opacity: 1; }
.loc__count {
  position: absolute;
  top: var(--s2);
  left: var(--s2);
  background: rgba(0, 0, 0, 0.6);
  color: #fff;
  border-color: transparent;
}

.loc__head {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3) var(--s4);
  border-bottom: 1px solid var(--line);
}
.loc__body {
  padding: var(--s4);
}

/* 折起来的排障块。summary 默认是 list-item，带个三角；留着——那个三角
   正是"这里还有东西"的唯一提示。 */
.fold__t {
  color: var(--text-3);
  font-size: var(--fs-xs);
  cursor: pointer;
}
.fold[open] .fold__t {
  margin-bottom: 4px;
}
.rendered {
  margin: 0;
  padding: var(--s2) var(--s3);
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
  color: var(--accent);
  line-height: 1.7;
  word-break: break-word;
}

.fold {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: 6px 0;
  border: 0;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
  text-align: left;
}
.fold:hover {
  color: var(--text);
}
</style>
