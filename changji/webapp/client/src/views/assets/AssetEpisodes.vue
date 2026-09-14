<script setup>
/**
 * 分集。设定三格里的第三格。
 *
 * 角色是「谁」，场景是「哪儿」，这一格是「怎么切」——同一个故事，每集
 * 30 秒切出来十几集，每集 3 分钟切出来三四集，讲的是同一件事。
 *
 * **这一页不写东西。** 正文在「故事」那一页，这里只看它被切成什么样：
 * 章节多长、线画在哪、每一集停在什么悬念上。往这里加编辑框的话，同一段
 * 正文就有两个地方能改，而两个地方迟早对不上。
 *
 * 分集是**章节之间那条线**，不是另一张表。单独列一张「第 3 集覆盖第 5~6
 * 章」的表，人看不见线画在哪，还得回去翻第 5 章是什么。
 */
import { computed, onActivated, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { runAsyncJob } from '@/composables/useAsyncJob'
import { useAction } from '@/composables/useAction'
import { useRefStream } from '@/composables/useRefStream'
import { useWriter } from '@/stores/run'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()
/**
 * 资产库变了就重拉——**这一格也要订**。
 *
 * 分集线上那一排人脸和空景图是从资产库来的（`facesOf` / `scenesOf`），
 * 而资产库是隔壁两格在改：照故事定妆会加人加地方、一键出图会把脸画出来。
 * 角色格和场景格都订着 `finished`，这一格原来没订——三格被 KeepAlive
 * 冻着来回切，于是在角色格定完妆切回来，分集线上新角色那一格还是空的，
 * 非得整个离开「设定」再回来才对。
 *
 * （原来这儿挂着 `defineExpose({ load })`，看着像是留给父组件招呼用的，
 * 但 AssetsView 用的是 `<AssetEpisodes v-else />`——没有 ref，那个口子
 * 从来没接上过。改成订同一条线，和另外两格一致。）
 */
const { finished, bustOf } = useRefStream()
const writer = useWriter()

const story = ref(null)
const assets = ref(null)
const loading = ref(false)
const openChapter = ref('')

// **体量不在这一格。** 它是"这个故事有多长"，写大纲时就要定，属于创作，
// 所以留在「故事」那一页。这一格只管"把它切成多长一段"。
const DURATIONS = [30, 60, 90, 120, 180]

const chapters = computed(() => story.value?.chapters ?? [])
const plan = computed(() => story.value?.plan ?? [])
const durationS = computed(() => story.value?.episode_duration_s ?? 60)
const hasStory = computed(() => chapters.value.length > 0)
const writtenCount = computed(
  () => chapters.value.filter((c) => (c.text ?? '').trim()).length,
)
/** 停在有说法的钩子上的集数。剩下的收在段落边界——到点了，不是悬念。 */
const hooked = computed(() => plan.value.filter((p) => p.hook).length)

/**
 * 回车/空格也能展开这一章。
 *
 * 那一行是个 div，键盘那条路得自己补——这一页上"看这一章讲什么"只有这一个
 * 入口（摘要只画在展开之后），Tab 走不到就等于键盘用户看不到。
 *
 * ⚠️ **只认落在行本身上的那一下。** 行里还有一个「去展开正文」的链接，
 * 不判的话在它上面按回车会既跳去故事页、又把这一行展开。项目库那条
 * （ProjectRail 的 onKey）栽过同一个坑，判据照抄它。
 */
function onChapKey(id, event) {
  if (event.target !== event.currentTarget) return
  if (event.key === 'Enter' || event.key === ' ') {
    event.preventDefault()
    openChapter.value = openChapter.value === id ? '' : id
  }
}

/** 这一章之后要画的那几条分集线（在这一章结束的集）。 */
function cutsAfter(chapterId) {
  return plan.value.filter((p) => p.to_chapter === chapterId)
}

// ---------------------------------------------------------------------------
// 每一集用到的人和地方
// ---------------------------------------------------------------------------
//
// 用户 2026-09-12：「分集里每集用到的角色和场景图应该显示出来，容易区分」。
//
// **分集表最难的就是分不清。** 十来行「第 N 集 · 60s · 某某悬念」，字都
// 差不多长、颜色都一样，要找"陈默在医院那一集"只能一行行读过去。而这几集
// 之间真正的差别是**谁在场、在哪儿**——那正好是图。
//
// 名单从章节来（`chapter.characters` / `chapter.locations`，是「读故事」
// 那一步照着正文读出来的），图从资产库来。两边靠**名字**对上：章节里存的
// 就是名字，不是 id。

/** 名字 → 角色。资产库里存的是 char_id，而章节里记的是名字。 */
const charByName = computed(() => {
  const m = new Map()
  for (const c of assets.value?.characters ?? []) m.set(c.name, c)
  return m
})
const locByName = computed(() => {
  const m = new Map()
  for (const l of assets.value?.locations ?? []) m.set(l.name, l)
  return m
})

/** 这一集覆盖的那几章。分集表里存的是首尾章号，中间的按顺序取。 */
function chaptersOf(ep) {
  const from = chapters.value.findIndex((c) => c.chapter_id === ep.from_chapter)
  const to = chapters.value.findIndex((c) => c.chapter_id === ep.to_chapter)
  if (from < 0 || to < 0) return []
  return chapters.value.slice(from, to + 1)
}

/**
 * 这一集自己那段正文。
 *
 * **一集常常只是一章的一截**：一章三千字切成三集，三集共用一份章级名单的
 * 话，那三行看着一模一样——而这一栏存在的全部理由就是让人分得清。
 *
 * `from_char` / `to_char` 是**各自那一章里**的偏移（首章从 from_char 到尾，
 * 末章从头到 to_char，中间整章）。它们是码点偏移，而 JS 的 slice 按 UTF-16
 * 数——中文都在基本平面上，两者一致；真混进 emoji 也只是偏几个字，
 * 对"名字在不在这一段里"没有影响。
 */
function sliceOf(ep) {
  const list = chaptersOf(ep)
  if (!list.length) return ''
  let out = ''
  list.forEach((c, i) => {
    const text = c.text ?? ''
    const a = i === 0 ? (ep.from_char ?? 0) : 0
    const b = i === list.length - 1 ? (ep.to_char ?? text.length) : text.length
    out += text.slice(a, b)
  })
  return out
}

/**
 * 这一集有谁、在哪儿。
 *
 * 两步：**名单从章节来，在不在场看这一集自己那段正文**。
 *
 *   * 候选名单是 `chapter.characters` / `chapter.locations`——「读故事」那一步
 *     照着正文读出来的，是权威的那一份。
 *   * **老项目里这两项是空的**：2026-09-12 之前 schema 没把它们写进
 *     required，14B 就一个都不给（见 story_outline.cpp 里那段）。那时候候选
 *     退成资产库里登记过的全部名字。
 *   * 然后拿这一集的正文过一遍：出现过的才算在场。人名在正文里是实打实
 *     写出来的，扫得准；地名多半扫不到（正文里很少原样写"高架桥下的咖啡
 *     馆"），扫不到就空着，不猜。
 *
 * 正文还没写的章走不到第二步（没得扫），那就直接用章级名单——那时候它是
 * 计划，显示计划是对的。
 */
function castOf(ep, listKey, lookup, refKey) {
  const covered = chaptersOf(ep)
  const listed = []
  for (const c of covered) {
    for (const n of c[listKey] ?? []) if (!listed.includes(n)) listed.push(n)
  }
  const candidates = listed.length ? listed : [...lookup.keys()]

  const text = sliceOf(ep)
  let names = text ? candidates.filter((n) => n && text.includes(n)) : []
  // 扫不出来（正文没写，或者名字确实没在这一段里出现）就退回章级名单。
  // **空着比错着好，但全空就等于这一栏不存在**——所以只在完全扫不到时退。
  if (!names.length) names = listed

  return names.map((name) => {
    const hit = lookup.get(name)
    // 地址后面挂一个换代号：参考图重画是原地覆盖、路径不变，不挂的话这一排
    // 脸会一直是缓存里的老样子（理由见 useRefStream 的 bustOf）。
    // target 和引擎那条 refs 频道上用的一样：`<id>_<slot>`——refKey 是
    // `ref_front` / `ref_empty`，去掉 `ref_` 正好是 slot 那一段。
    const id = hit?.char_id ?? hit?.location_id ?? ''
    const target = id ? `${id}_${refKey.slice(4)}` : ''
    return {
      name,
      // 没有图就只给名字，界面上退成一个字的小牌子——**比不显示强**：
      // 这一栏存在的理由就是让人一眼分清哪一集是哪一集，而名字也分得清。
      url: hit?.[refKey]
        ? mediaUrl(session.projectPath, hit[refKey]) + '&_=' + bustOf(target)
        : '',
    }
  })
}

const facesOf = (ep) =>
  castOf(ep, 'characters', charByName.value, 'ref_front')
const scenesOf = (ep) =>
  castOf(ep, 'locations', locByName.value, 'ref_empty')

async function load() {
  if (!session.projectPath) {
    story.value = null
    assets.value = null
    // 早返回也要把转圈关掉，理由同镜头墙那处：被顶掉的那趟不会清它，
    // 而这一页转圈亮着的时候章节表和空状态都不画，一片空白。
    loading.value = false
    return
  }
  // 这一趟是给哪部剧读的。在项目库里连着点三部，回来的顺序不保证——
  // 慢的那趟后落地，画的就是别的剧的章节和分集线。
  const want = session.projectPath
  const mine = () => want === session.projectPath
  loading.value = true
  try {
    // 两份一起拉：分集线上要显示的人脸和空景图在资产库里，
    // 而一集是哪几个人在哪几个地方，在故事里。
    const [got, lib] = await Promise.all([
      api.getStory(want),
      // 资产库拉不动不该把整页挡住——那时候分集线退成只有名字。
      api.assets(want).catch(() => null),
    ])
    if (!mine()) return
    story.value = got.story ?? null
    assets.value = lib
  } catch (err) {
    if (!mine()) return
    // **读不到就得空着。** 没有故事的项目是 200 加一份空故事（story.json
    // 不存在时引擎回空的那份），走到这儿的是真出事了：项目被挪走、
    // story.json 坏了、引擎连不上。这时候原来什么都不清——上一部剧的章节
    // 和分集线就**原样留在这一部的页面上**，而下面那排按钮（改时长就是
    // 重新分集、落成剧集）按的是现在这一部。
    story.value = null
    assets.value = null
    ui.error(err.message)
  } finally {
    if (mine()) loading.value = false
  }
}
/**
 * 剪好还没采用的那一条预告片。**声明必须留在这儿**，不能跟着它那一族
 * （extras / trailerDurationS / TRAILER_ID）放到下面的折叠区那一节去。
 *
 * ⚠️ 下面那个换剧的 watch 带着 `immediate: true`——Vue 会在 `watch()`
 * 这一句上**同步**跑一次回调，而回调第一件事就是 `trailerDraft.value = null`。
 * 声明在它后面的话，那一下落在 const 的暂时性死区里，当场
 * `ReferenceError: Cannot access 'trailerDraft' before initialization`，
 * setup 抛出去、这一格整个渲染不出来——「分集」那一 tab 点开就是错误页。
 *
 * 0e65fcf 把那个 watch 从 `watch(…, load, { immediate: true })` 改成带
 * 回调的写法时就是这么栽的（AssetCharacters 那三个 ref 2026-09-15 早些
 * 时候刚因为同一件事提过一次）。
 */
const trailerDraft = ref(null)

watch(
  () => session.projectPath,
  () => {
    // **换剧要把预告片草稿扔掉。** 它是上一部剧的字，而 adoptTrailer 写的
    // 是 `session.projectPath`——也就是**现在这一部**。剪完一条不采用、去
    // 项目库点了另一部剧，草稿那一格还原样挂着（load 只换 story 和资产），
    // 这时候按「存成 trailer 这一集」，上一部剧的预告片就落进这一部了。
    //
    // 只在换剧这一条路上清。load 还挂在 `finished` 上（资产库一变就重拉），
    // 在那儿清的话，隔壁格子出完图会顺手把人正看着的草稿收走。
    trailerDraft.value = null
    load()
  },
  { immediate: true },
)
watch(finished, load)
// 批量展开正文跑完，这一页的章节长度、分集线、「几章没正文」都变了。
// 理由同 AssetsView 那条：不订的话跑完一个多小时回来，看到的还是开跑
// 之前那份，而且不会自己变。
watch(
  () => writer.running,
  (now, before) => {
    if (before && !now) load()
  },
)

async function pickDuration(event) {
  const seconds = Number(event.target.value)
  const project = session.projectPath
  // **改时长就是重新分集。** 存一个数然后等人再按一次「重算」，那一下
  // 之间界面上写的集数是旧的，而用户以为已经改了。
  const result = await run(
    () =>
      api.planEpisodes({ project, duration_s: seconds }),
    { key: 'duration' },
  )
  if (!result) return
  // 换剧了就别把这一份分集表装进新那一部（它是上一部算出来的）。
  if (project !== session.projectPath) return
  story.value = result.story ?? story.value
  ui.ok(`每集 ${seconds} 秒 → ${plan.value.length} 集`)
}

async function makeEpisodes() {
  const result = await run(
    () => api.makeEpisodes({ project: session.projectPath }),
    { key: 'episodes', refresh: true },
  )
  if (!result) return
  const created = result.created?.length ?? 0
  const updated = result.updated?.length ?? 0
  ui.ok(created ? `建了 ${created} 集` : `${updated} 集已经在了，只更新了信息`)
  await load()
}

// ---------------------------------------------------------------------------
// 支线。整部剧只用一两次的东西，收在折叠区里
// ---------------------------------------------------------------------------

const extras = ref(false)
const trailerDurationS = ref(20)
/** 预告片挂在固定集号上，只有一条，重剪覆盖上一条 */
const TRAILER_ID = 'trailer'

/**
 * 剪一条预告片。
 *
 * 对流水线来说预告片就是特别短的一集：采用之后照样走镜头、成片、发布。
 * 区别只在写的时候——要的是钩子不是完整故事，所以它**不占集号**，也不参与
 * 「接着前几集写」的上下文。
 */
async function writeTrailer() {
  // 开工那一刻把项目钉死：`runAsyncJob` 要等那条 socket 开（最多两秒）
  // 才把请求发出去，这中间在项目库里点了别的剧的话，下面这个 `project`
  // 读到的就是新那一部。同文件里一键出图那条的理由。
  const project = session.projectPath
  const result = await run(
    () =>
      runAsyncJob(
        (extra) =>
          api.writeTrailer({
            project,
            duration_s: trailerDurationS.value,
            ...extra,
          }),
        { prefix: 'trailer', label: '剪预告片' },
      ),
    { key: 'trailer' },
  )
  if (!result) return
  // **人已经走了就别把它摆在这一部上。**
  //
  // 剪一条要一两分钟。换剧那个 watch 会把 `trailerDraft` 清掉（注释写着
  // 理由：草稿是上一部剧的字，而 adoptTrailer 写的是现在这一部），但清空
  // 发生在**回包之前**——落地这一下会把它又摆回来，摆在新这一部的折叠区
  // 里，按一下「存成 trailer 这一集」就写进去了。和剧本页那个草稿是同一
  // 个坑。
  //
  // 这一份不落盘（引擎不存预告片草稿），所以要说一句，别当没发生过。
  if (project !== session.projectPath) {
    ui.info('那一部剧的预告片剪好了，但你已经切走了——回去再剪一次')
    return
  }
  trailerDraft.value = result
}

async function adoptTrailer() {
  if (!trailerDraft.value) return
  // 三趟请求（建集 / 改名 / 存剧本）串着发，中间隔两个来回。项目路径现读
  // 的话，这中间在项目库点一下别的剧，后面那两趟就落到新那一部上——建集
  // 建在这一部、剧本存到了另一部。整件事钉在开工那一刻这一部上。
  const project = session.projectPath
  await run(
    async () => {
      const exists = session.episodes.some((e) => e.episode_id === TRAILER_ID)
      if (!exists) {
        await api.newEpisode({
          project,
          episode_id: TRAILER_ID,
          title: trailerDraft.value.title,
          target_duration_s: trailerDurationS.value,
        })
      } else if (trailerDraft.value.title) {
        // 重剪一条覆盖上一条。标题只在建集那一下写过，不补这一句的话，
        // 顶栏的集号选择器上挂的还是**上一条**预告片的名字，而底下的
        // 剧本已经换了——存剧本那一趟只写 script / 时长 / 梗概。
        await api.episodeAction({
          project,
          episode_id: TRAILER_ID,
          action: 'rename',
          new_title: trailerDraft.value.title,
        })
      }
      await api.saveScript({
        project,
        episode_id: TRAILER_ID,
        script: trailerDraft.value.script,
        duration_s: trailerDurationS.value,
        synopsis: trailerDraft.value.logline,
      })
      trailerDraft.value = null
    },
    { key: 'adoptTrailer', success: '预告片存下了', refresh: true },
  )
}

/**
 * 刷新之前拦一下：预告片那份草稿**只活在内存里**。
 *
 * 剪一条要跑几分钟，而回包里那份稿子这一页没有任何落盘的地方（故事页那份
 * 大纲不一样，引擎把它存了，见 save_story_draft）。刷新、关标签页、点错一
 * 个链接，那几分钟就得重来一遍，而屏幕上什么都不会说。
 *
 * 正在剪的时候同理：活儿在引擎那头照样跑完，但结果只从那条 socket 送回来
 * 一次，没人接就没了。
 */
function beforeUnload(e) {
  if (!trailerDraft.value && !isBusy('trailer')) return
  e.preventDefault()
  e.returnValue = ''
}
/**
 * 让「写」那个槽的状态**真的有人在看**。
 *
 * ⚠️ **`writer` 这个 store 原来只有故事页在驱动。** `writer.start()` /
 * `writer.poll()` 全仓只出现在 StoryView 里，而读 `writer.running` 的有
 * 五处：这一格、AssetsView、EpisodeView、ProjectRail、useShots。
 * 没人轮询的时候 `writer.state` 一直是 null，`running` 恒为假——
 * 那五处挂在它上面的 watch **一次都不会触发**。
 *
 * 落到这一格上就是：在这儿按「批量补分镜」（它跑在"写"那个槽上），
 * 从头到尾没有任何一处轮询过那个槽，于是下面那条「跑完了重拉」的 watch
 * 是死的——跑完一个多小时回来，章节长度、分集线、「几章没正文」还是开跑
 * 之前那份，而且不会自己变。那条 watch 的注释写的正是这件事，它只是
 * 没等到人。AssetsView 的标签数、EpisodeView 的四个标签同理。
 *
 * 故事页早就为同一件事补过（它 onMounted 里那两句，注释写着「刷新之后
 * 没有任何人在轮询」），这儿照它办：进来先问一次，还在跑就把轮询接上。
 * `poll()` 自己会在"没在跑"时 stop()，所以不在跑的时候这两句只多一次请求。
 *
 * **onActivated 也要**：这一格被 AssetsView 的 `<KeepAlive>` 冻着，
 * 切到角色格再切回来 onMounted 不会再跑，而那中间别处完全可能起一轮。
 */
async function watchWriter() {
  await writer.poll()
  if (writer.running) writer.start()
}

onMounted(() => {
  window.addEventListener('beforeunload', beforeUnload)
  watchWriter()
})
onActivated(watchWriter)
onUnmounted(() => window.removeEventListener('beforeunload', beforeUnload))

/** 手动加一集。没走故事那条路的老项目还得有这个口子。 */
async function addEpisode() {
  // **钉住项目。** 建一集要落盘、回来还跟着 refresh 一趟，中间隔两个来回，
  // 而这中间在项目库里点一下别的剧是随时会发生的。路径现读的话后果落在
  // 最后那句 `selectEpisode` 上：**上一部**新建的那个集号被写进了新这一部
  // （还顺手写进 localStorage），于是新这一部的每一页都拿着一个它根本没有
  // 的集号去问引擎——界面上长得像"引擎抽风"，而且刷新也不会自己好。
  // 同文件里剪预告片、采用预告片那两条的理由。
  const project = session.projectPath
  const created = await run(
    () =>
      api.newEpisode({
        project,
        target_duration_s: durationS.value,
      }),
    { key: 'addEp', success: '新建了一集' },
  )
  if (!created) return
  if (project !== session.projectPath) {
    ui.info(`那一部剧加了 ${created.episode_id}，但你已经切走了——集没丢，回去就在`)
    return
  }
  await session.refresh()
  if (project !== session.projectPath) return
  session.selectEpisode(created.episode_id)
}

/** 给全项目还没分镜的集补分镜。从「这一集」的镜头格搬来的，理由见模板。 */
async function planAll() {
  const result = await run(
    () => api.planAll({ project: session.projectPath, overwrite: false }),
    { key: 'planAll' },
  )
  // **别写「去『这一集』能看进度」。** 批量补分镜跑在"写"那个槽上
  // （和写整季同一个），而「这一集」那一页盯的是"出片"那个槽——它那儿
  // 一动不动。真正一直看得见的是顶栏那块「AI 作业中」。
  if (result) {
    // **把轮询接上。** 不接的话没有任何一处在看"写"那个槽，下面那条
    // 「跑完了重拉」的 watch 永远等不到 running 从真变假——见 watchWriter
    // 上面那段。故事页点「展开」那一下也是这么做的（writeAllChapters）。
    writer.start()
    ui.info(
      `正在给 ${result.episodes.join('、')} 补分镜，顶栏那块「AI 作业中」里看进度`,
    )
  }
}

</script>

<template>
  <div class="eps">
    <!-- 同一个故事，每集多长决定切成几集。集数是算出来的。改时长就是重新分集。 -->
    <div class="toolbar">
      <label class="dur">
        <span class="tiny dim">每集</span>
        <select
          class="select dur__pick"
          :value="durationS"
          :disabled="isBusy('duration')"
          title="改时长就是重新分集"
          @change="pickDuration"
        >
          <option v-for="d in DURATIONS" :key="d" :value="d">{{ d }} 秒</option>
        </select>
      </label>
      <span v-if="hasStory" class="tiny dim nowrap">
        {{ chapters.length }} 章 → {{ plan.length }} 集<template v-if="plan.length">
          · {{ hooked }} 集停在悬念上</template>
        <template v-if="writtenCount < chapters.length">
          · {{ chapters.length - writtenCount }} 章还没正文</template>
      </span>
      <span class="spacer" />
      <!-- 给全项目还没分镜的集都出一遍。原来在「这一集」的镜头格上——
           一个管**全项目**的按钮摆在**一集**的页面上。它属于这儿：
           这一格就是所有集摆在一起的地方。 -->
      <button
        v-if="hasStory && plan.length"
        class="btn btn--ghost btn--sm"
        type="button"
        :disabled="isBusy('planAll')"
        title="把还没有分镜的集一次出完，有分镜的不动"
        @click="planAll"
      >
        {{ isBusy('planAll') ? '排着…' : '批量补分镜' }}
      </button>
      <button
        v-if="hasStory"
        class="btn btn--primary btn--sm"
        type="button"
        :disabled="isBusy('episodes')"
        title="分集表是计划，落成剧集之后后面几步才有东西可对"
        @click="makeEpisodes"
      >
        {{ isBusy('episodes') ? '正在建…' : '落成剧集' }}
      </button>
    </div>

    <div v-if="loading" class="tiny dim">读取中…</div>

    <!-- 图标名要在 AppIcon 的 PATHS 里有。`book` 没有，拼不到就落回 ⓘ——
         一个说「还没有故事」的空状态顶着信息图标。`script` 是故事那一步在
         侧边栏用的同一个，指过去的也正是那一页。 -->
    <EmptyState v-else-if="!hasStory" icon="script" title="还没有故事">
      <RouterLink to="/story" class="btn btn--sm">去写故事</RouterLink>
    </EmptyState>

    <!-- 章节一行，分集线画在两行之间。线在哪一眼就看得见。 -->
    <section v-else class="stack stack--sm">
      <template v-for="(c, i) in chapters" :key="c.chapter_id">
        <div
          class="chap"
          :class="{ 'is-open': openChapter === c.chapter_id }"
          role="button"
          tabindex="0"
          :aria-expanded="openChapter === c.chapter_id"
          @click="openChapter = openChapter === c.chapter_id ? '' : c.chapter_id"
          @keydown="onChapKey(c.chapter_id, $event)"
        >
          <span class="chap__no numeric">{{ i + 1 }}</span>
          <div class="chap__text">
            <div class="chap__title">{{ c.title }}</div>
            <p v-if="openChapter === c.chapter_id && c.summary" class="chap__sum">
              {{ c.summary }}
            </p>
          </div>
          <span v-if="c.text" class="tiny dim numeric nowrap">
            {{ [...c.text].length }} 字
          </span>
          <RouterLink v-else to="/story" class="btn btn--sm btn--ghost nowrap" @click.stop>
            去展开正文
          </RouterLink>
        </div>

        <div v-for="ep in cutsAfter(c.chapter_id)" :key="ep.episode_id" class="cut">
          <span class="cut__id numeric">{{ ep.episode_id }}</span>
          <span class="cut__dur numeric">{{ ep.target_duration_s }}s</span>

          <!-- 这一集里谁在场、在哪儿。**人是圆的，地方是方的**——形状不一样，
               扫一眼就分得开，不用去读底下那行字。 -->
          <span class="cast">
            <span
              v-for="f in facesOf(ep)"
              :key="'c' + f.name"
              class="cast__one cast__one--who"
              :title="f.name"
            >
              <img v-if="f.url" :src="f.url" :alt="f.name" loading="lazy" />
              <i v-else>{{ [...f.name][0] }}</i>
            </span>
            <span
              v-for="l in scenesOf(ep)"
              :key="'l' + l.name"
              class="cast__one cast__one--where"
              :title="l.name"
            >
              <img v-if="l.url" :src="l.url" :alt="l.name" loading="lazy" />
              <i v-else>{{ [...l.name][0] }}</i>
            </span>
          </span>

          <!-- 钩子是"这一集停在哪儿"的全部说明，分集线上一行放不下。 -->
          <span v-if="ep.hook" class="cut__hook truncate" :title="ep.hook">{{ ep.hook }}</span>
          <span v-else class="cut__hook dim">章尾</span>
        </div>
      </template>
    </section>

    <!-- 支线。整部剧只用一两次的东西，收在折叠区里 -->
    <section class="stack stack--sm">
      <button class="fold" type="button" @click="extras = !extras">
        <AppIcon :name="extras ? 'arrowLeft' : 'arrowRight'" :size="14" />
        <span>预告片 · 手动加一集</span>
      </button>

      <div v-if="extras" class="stack stack--sm">
        <section v-if="trailerDraft" class="draft">
          <div class="sec__head">
            <h2 class="sec__t">{{ trailerDraft.title }}</h2>
            <span class="tiny dim truncate">{{ trailerDraft.logline }}</span>
            <span class="spacer" />
            <button
              class="btn btn--primary btn--sm"
              type="button"
              :disabled="isBusy('adoptTrailer')"
              @click="adoptTrailer"
            >
              存成 trailer 这一集
            </button>
            <button
              class="btn btn--ghost btn--sm"
              type="button"
              @click="trailerDraft = null"
            >
              丢弃
            </button>
          </div>
          <pre class="mono small trailer__script">{{ trailerDraft.script }}</pre>
        </section>

        <div class="row row--wrap">
          <label class="dur">
            <span class="tiny dim">预告片</span>
            <select v-model.number="trailerDurationS" class="select dur__pick">
              <option :value="15">15 秒</option>
              <option :value="20">20 秒</option>
              <option :value="30">30 秒</option>
            </select>
          </label>
          <button
            class="btn btn--ai btn--sm"
            type="button"
            :disabled="!hasStory || isBusy('trailer')"
            @click="writeTrailer"
          >
            {{ isBusy('trailer') ? '剪着…' : '剪一条' }}
          </button>
          <span class="spacer" />
          <button
            class="btn btn--ghost btn--sm"
            type="button"
            :disabled="isBusy('addEp')"
            title="加出来的那集不在分集表里，走老路径"
            @click="addEpisode"
          >
            手动加一集
          </button>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.eps {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
}
.dur {
  display: inline-flex;
  align-items: center;
  gap: var(--s2);
}
.dur__pick {
  width: auto;
  height: 27px;
  padding: 0 var(--s2);
}
.draft {
  border: 1px dashed var(--accent-line);
  border-radius: var(--r);
  padding: var(--s3);
}

/* 章节一行，分集线画在两行之间——线在哪一眼就看得见，
   这正是不把分集做成另一张表的理由 */
.chap {
  display: flex;
  align-items: baseline;
  gap: 10px;
  padding: 8px 12px;
  border: 1px solid var(--line);
  border-radius: var(--r-sm);
  background: var(--surface);
  cursor: pointer;
}
.chap.is-open {
  border-color: var(--accent);
}
.chap__no {
  color: var(--text-3);
  font-size: var(--fs-sm);
  min-width: 1.5em;
}
.chap__text {
  flex: 1;
  min-width: 0;
}
.chap__title {
  font-weight: 500;
}
.chap__sum {
  margin: 4px 0 0;
  font-size: var(--fs-sm);
  color: var(--text-2);
}

.cut {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 2px 12px;
  font-size: var(--fs-xs);
  color: var(--accent);
  border-top: 1px dashed var(--accent);
  margin: 2px 0;
}
.cut__id {
  font-weight: 600;
}
.cut__dur {
  color: var(--text-3);
}
.cut__hook {
  flex: 1;
  min-width: 0;
}

/* 这一集用到的人和地方。**挤在分集线上，不另起一行**——它是用来区分
   相邻几集的，离开那条线就失去了参照。 */
.cast {
  display: flex;
  align-items: center;
  gap: 3px;
  flex: none;
}
.cast__one {
  display: grid;
  place-items: center;
  width: 22px;
  height: 22px;
  overflow: hidden;
  background: var(--surface-3);
  color: var(--text-3);
  font-size: 10px;
  font-style: normal;
  line-height: 1;
}
.cast__one img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
/* 人是圆的。头像裁成圆的时候脸在正中间，而参考图是正面全身——
   所以取上面那一截。 */
.cast__one--who {
  border-radius: 50%;
}
.cast__one--who img {
  object-position: top center;
}
/* 地方是方的（带一点圆角），而且宽一些：空景图是 16:9，裁成正方形
   基本只剩中间一堵墙。 */
.cast__one--where {
  width: 34px;
  border-radius: var(--r-sm);
}

.fold {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: 6px 0;
  border: 0;
  background: transparent;
  color: var(--text-2);
  cursor: pointer;
}
.trailer__script {
  white-space: pre-wrap;
  max-height: 20em;
  overflow: auto;
  margin: 0;
}
</style>
