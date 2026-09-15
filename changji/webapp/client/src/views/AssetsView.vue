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
import { stoppedByHand } from '@/composables/stopped-by-hand'
import { useRefStream } from '@/composables/useRefStream'
import { useWriter } from '@/stores/run'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const route = useRoute()
const router = useRouter()
const ui = useUi()
const { run, isBusy, error: actionError } = useAction()
const { finished, touch } = useRefStream()
const writer = useWriter()

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

/**
 * **挡着「设定」这一步的那两件事，要在 tab 上就说。**
 *
 * 侧栏那个对勾（`done.assets`）的判据是：有角色、场景都登记了、**而且这一集
 * 的镜头都真接到场景上**（引擎 flow.cpp 里那句 `scenes_ok`——靠 scene_id
 * 蒙对的不算）。参考图缺不缺它一概不问。
 *
 * 而这两件事原来只画在场景格**里面**那两条提示上，设定页默认落在角色格：
 * 侧栏说「设定还没做完」，人点进来一看角色齐、图也齐，不知道该按哪儿，
 * 非得挨个 tab 翻过去才撞见。
 *
 * 数不用另外问：`/bff/flow` 每次都在回（`counters.unlinkedShots` /
 * `counters.missingLocations`），此前整个界面只有拿不到的投递层读过它。
 * 挡着的排在「缺图」前面——缺图只是画面会差一点，这两样是这一步过不去。
 */
const unlinkedShots = computed(() => Number(session.counters?.unlinkedShots ?? 0))
const unregisteredLocs = computed(
  () => (session.counters?.missingLocations ?? []).length,
)
const locGap = computed(() => {
  if (unregisteredLocs.value) return `${unregisteredLocs.value} 个没登记`
  if (unlinkedShots.value) return `${unlinkedShots.value} 镜没接上`
  return locMissing.value ? `缺 ${locMissing.value}` : ''
})
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
    gap: locGap.value,
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
  // **确认框里报的代价是这一部剧的，那这一趟就得落在这一部上。**
  //
  // `runAsyncJob` 要等那条 socket 开（最多两秒）才发请求，而下面原来现读
  // session：这两秒里在项目库点了另一部剧，`overwrite` 那一下就带着"会冲
  // 掉 15 张参考图"的确认，落到一部**没被问过**的剧上——而那一下是没有撤
  // 销的。一键出图那条早就把项目钉死了，理由写在它旁边。
  const project = session.projectPath
  const episodeId = session.episodeId
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
            project,
            overwrite: over,
            ...(episodeId ? { episode_id: episodeId } : {}),
            ...extra,
          }),
        { prefix: 'bible', label: '照故事定妆' },
      ),
    { key: 'bible', refresh: true },
  )
  if (!result) return
  // **把 id 翻回人话。** `added_characters` 回的是 `c_lin_wan` 这种 id，
  // 直接 join 出去就是「2 个新角色：c_lin_wan、c_chen_mo」——而这一条正是
  // 定妆完人第一眼看的东西。这套系统的规矩本来就写着「分镜表里只有 id，
  // 界面负责把它翻回人话」（见 EpShots 里 charName 那段），这儿漏了。
  // 名字不用另外去问：同一个回包里的 `characters` 每条都带着 name。
  const nameOf = new Map(
    (result.characters ?? []).map((x) => [x.char_id, x.name]).filter(([, n]) => n),
  )
  const c = (result.added_characters ?? []).map((id) => nameOf.get(id) || id)
  const l = result.added_locations ?? []
  const parts = []
  if (c.length) parts.push(`${c.length} 个新角色：${c.join('、')}`)
  if (l.length) parts.push(`${l.length} 个新场景`)
  // 收掉了几条同名的要说出来——"场景从 25 变成 15"不解释的话看着像丢了东西
  if (result.merged) parts.push(`收掉 ${result.merged} 条重名的`)
  // **后面这两个数一直没人读，而引擎是特意回的。**
  //
  // 它们说的是这一下**动了已经存在的东西**，而上面那几句说的都是新增：
  //
  //   · remapped_shots：收掉重名的之后，原来指着被收那一条的镜头要改指向
  //     （引擎 planning.cpp 那句注释就写着「跟着改了几镜……界面上要说出来」）。
  //   · reset_shots：勾了覆盖时，全项目已经渲染过的镜头被退回「待重跑」、
  //     重试次数清零。按之前弹的那个确认框只说了「已渲染的镜头要重跑」，
  //     没说几个；真跑完更该报实数——场景格那条关联的提示早就是这个规矩
  //     （只提真的发生了的重置）。
  if (result.remapped_shots) parts.push(`${result.remapped_shots} 个镜头跟着改了指向`)
  if (result.reset_shots) parts.push(`${result.reset_shots} 个镜头退回重跑`)
  // 定妆要叫一趟模型，几十秒到几分钟；中途在项目库里点别的剧很自然。
  // 活儿是替钉住的 `project` 干的、结果也写在它身上，所以这句话要说清是
  // 替谁干的——照 genAll 和 AssetEpisodes 那几条现成的说法。
  const summary = parts.length
    ? parts.join('；')
    : '故事里的人和地方库里都有了，没补新的'
  if (project !== session.projectPath) {
    ui.info(`那一部剧定完妆了（${summary}），但你已经切走了——回去就能看到`)
  } else {
    ui.ok(summary)
  }
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
  // **开跑那一刻把项目钉死。**
  //
  // 这一轮要跑十几分钟，而下面每一张图原来都现读一次 `session.projectPath`。
  // 中途在项目库里点了另一部剧，接着那几张就拿**上一部**的角色 id 去新这一
  // 部出图：id 在新项目里不存在，一路 404，`run` 报一句看不懂的错然后 break
  // ——而人只是换了个项目看看。活儿是替那一部排的，就一直替那一部跑完。
  const project = session.projectPath
  // **这一读要包起来。** 它原来是裸的 `await api.assets(...)`：引擎打个嗝、
  // 项目被别处删了，这一下就抛出去成了没人接的 Promise 拒绝——按钮点下去
  // 一点反应都没有，也不报错。而下面每一张图那次调用都是包着的，只有
  // 开头这一读漏了。用同一个 key，读的那几百毫秒里按钮也是灰的。
  const data = await run(() => api.assets(project), { key: 'genall' })
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
    // **别写「在那一格点「重画」」。** 两处都对不上：
    //   · 重画的按钮在**抽屉里**，墙上那张卡只是点开抽屉的入口；
    //   · 「重画」这个字只有场景那一格有。角色那三张挤在一行里，按钮上
    //     写的是一个「画」字（「重画这一张」只在 title 里），照着找
    //     「重画」两个字会找不到。
    ui.ok('参考图都齐了。要换某一张，点开那个人或那个地方，在参考图那一格重画一张')
    return
  }
  if (
    force &&
    !confirm(
      `会把 ${jobs.length} 张参考图全部重画，手传上去的也会被顶掉。` +
        // **最大的那一项代价原来没说。** 出一张参考图引擎就调一次
        // reset_all_shots（参考图直接决定画面长什么样），也就是说这一下
        // 会把**全项目**已经渲染好的镜头退回待跑。一部已经出过片的剧，
        // 这句话的分量比"大约 8 分钟"重得多。
        `已经渲染好的镜头也会退回重跑。` +
        `一张几十秒，大约 ${Math.ceil((jobs.length * 30) / 60)} 分钟。确定？`,
    )
  ) {
    return
  }

  let made = 0
  /** 半路停在第几件上。-1 = 一件不落地跑完了。 */
  let stoppedAt = -1
  /** 停在那一件上是人按的「停下」，不是砸了。两种说法不一样。 */
  let byHand = false
  for (let i = 0; i < jobs.length; i += 1) {
    const j = jobs[i]
    bulk.value = { at: i + 1, total: jobs.length, name: j.name, pct: 0 }
    const ok = await run(
      () =>
        runAsyncJob(
          (extra) =>
            j.kind === 'char'
              ? api.generateReference({
                  project,
                  char_id: j.id,
                  slot: j.slot,
                  ...extra,
                })
              : api.generateLocationReference({
                  project,
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
      // **这一圈不自己弹红条**（quiet）：人按「停下」也会走到这儿，而
      // 「停」不是失败。引擎那头分得很清——ref_gen.cpp 里专门写着「**人按
      // 的停不是失败。** 报成「出图失败：已取消」的话，人会去找哪儿出错
      // 了」，取消回的是 400「已停下这一张」。而 run() 见到抛错一律
      // ui.error，于是主动按的停在屏幕上是一条红的，底下还跟一句橙的
      // 「还差 N 张没画」——两条都在说出事了，而什么都没出事。
      //
      // 所以这儿收住，到下面按「是停的还是砸的」分两种说法。
      { key: 'genall', quiet: true },
    )
    // 中间砸了就停：后面那些多半栽在同一件事上（模型没配、显存不够），
    // 接着画只是让人多等十几分钟再看到同一句报错。
    if (!ok) {
      stoppedAt = i
      // 引擎给取消留的是 400「已停下这一张」；别的都算真砸了。
      byHand = stoppedByHand(actionError.value)
      break
    }
    made += 1
    // 不用在这儿招呼两格重拉：引擎画完每一张都会往 refs 频道播一条
    // ref_done，那两格和这儿的数都订着它。见 useRefStream。
  }
  bulk.value = null
  // 这一轮十几分钟，中途在项目库里点别的剧很自然——活儿是替 `project`
  // 排的、也一直替它跑完（上面那段把项目钉死了）。所以这句话要说清是替谁
  // 画的，不然它落在新这一部的屏幕上，而这一部一张新图都没有。
  // 照 AssetEpisodes / EpisodeView 那几条现成的说法。
  // **半路停了就别报一句绿的。**
  //
  // 上面那句 `if (!ok) break` 是对的（后面那些多半栽在同一件事上），可
  // 报出来的一直是「画好了 2 张」——绿的、句号。`run` 那头确实先弹了一句
  // 红的说为什么，但最后落在屏幕上的是那句绿的，而它读起来像"这一轮完了"。
  // 排了 12 张只画了 2 张，剩下 10 张一个字没提。
  //
  // 说法照投递那一页现成的（「投了 N 条，M 条失败，看下面的记录」）：
  // 数字要齐，还要指一句去哪儿看原因。
  const left = stoppedAt >= 0 ? jobs.length - made : 0
  const where = project !== session.projectPath ? '那一部剧' : ''
  const tail = project !== session.projectPath ? '，但你已经切走了——回去就能看到' : ''
  if (left && byHand) {
    // **人自己按的停。** 说清停在哪儿、剩多少就够，不报错——他知道自己
    // 按了什么。同 useShots / useWriter 里那条（「自己按的停，别再红一次」）。
    ui.info(
      made
        ? `${where}停下了，已经画好 ${made} 张，还剩 ${left} 张没画${tail}`
        : `${where}停下了，一张都还没画完`,
    )
  } else if (left) {
    // 真砸了。上面那一圈是 quiet 的，报错这件事得自己来——原话照引擎给的。
    if (actionError.value) ui.error(actionError.value)
    // 一张都没画成时也要说——原来 `if (made)` 把这种整个吞了，屏幕上只有
    // `run` 那句红的，而那句话不提"这一轮一共要画几张、停在哪儿"。
    const head = made ? `${where}画好了 ${made} 张，还差 ${left} 张没画` : `${where}一张都没画成，排着的 ${left} 张都还在`
    ui.warn(`${head}——上面那句说了为什么${tail}`)
  } else if (made) {
    if (project !== session.projectPath) {
      ui.info(`那一部剧画好了 ${made} 张${tail}`)
    } else {
      ui.ok(`画好了 ${made} 张`)
    }
  }
}

async function loadStory() {
  if (!session.projectPath) {
    story.value = null
    return
  }
  // 哪一部剧的那一趟。在项目库里连着点两部，慢的那趟后落地就把上一部的
  // 章节和分集表挂在这一部的 tab 上（「分集 12」），而三个格子自己读的
  // 是新的——同一屏上两套数。
  const want = session.projectPath
  try {
    const data = await api.getStory(session.projectPath)
    if (want !== session.projectPath) return
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
  const want = session.projectPath
  try {
    const got = await api.assets(session.projectPath)
    if (want !== session.projectPath) return
    assets.value = got
  } catch {
    // 刚建的项目还没有资产库。tab 上就只有名字，不该整页红。
    assets.value = null
  }
}

/** story/assets 里那两份是哪部剧读回来的。 */
let shownFor = null

/**
 * 换了一部剧，先把上两份擦掉。
 *
 * 那两个 `want !== projectPath` 的闸管的是"回来晚了别乱写"，管不了这段
 * 空当里标签上写着什么：`story`/`assets` 不为空就一直照着算，而它们装的
 * 还是上一部的角色和分集。于是从 A 点到 B 的那一两秒里，B 的标签上明晃
 * 晃写着「角色 2 · 缺 6」「分集 10 · 8 章没正文」——而 B 可能一个角色都
 * 没有。三个格子自己读的是新的，同一屏上两套数。
 *
 * 只在 loadAll 里擦：画完一张图那条（`watch(finished, loadAssets)`）项目
 * 没变，擦了的话每出一张图标签就空一下。
 */
function loadAll() {
  const want = session.projectPath
  if (want !== shownFor) {
    shownFor = want
    story.value = null
    assets.value = null
    // **「覆盖已有」这个勾也要跟着掉。**
    //
    // 它是个**破坏性**的开关：勾上之后「照故事定妆」变「重新定妆」（同名
    // 的角色/场景用新出的顶掉旧的，手改的设定和参考图会丢）、「一键出图」
    // 变「全部重画」（挑过的、手传的会被顶掉）。
    //
    // 不清的话：在 A 上勾好准备重做，顺手去项目库点了 B——项目栏在每一页
    // 都挂着，这一下随时会发生——回到这一屏，两颗按钮已经写着「重新定妆」
    // 「全部重画」，而那是对 B 说的。人勾这一下时想的是 A。
    //
    // 同一个隐患在**一次动作内部**早就防过了（`genAll` 里那句
    // 「这两秒里在项目库点了另一部剧，`overwrite` 那一下就带着"会冲掉"的
    // 语义落到新这一部头上」，所以它先把 `over` 钉住）。防的是同一件事，
    // 只是那儿管两秒、这儿管换剧之后一直。
    //
    // 确认框仍然在（勾不是全部的防线），但那句"你确定吗"人已经在 A 上
    // 心里答过一遍了——别让它变成对 B 的答案。
    overwrite.value = false
  }
  loadStory()
  loadAssets()
}

onMounted(loadAll)
watch(() => session.projectPath, loadAll)
// 画完一张、定妆完——tab 上的数跟着走
watch(finished, loadAssets)
/**
 * 批量那条跑完，这一行标签也要跟上。
 *
 * 「分集」那一格写的是「10 · 8 章没正文」，而"没正文"这个数是从故事来的
 * ——批量展开正文（和"写"那个槽上的别的活）动的正是它。不订的话，跑了一
 * 个多小时回到这一页，那一格还是开跑之前的数，而且不会自己变。
 *
 * 只订下降沿：跑的过程中它一章章写，这一行是给"到哪一步了"看的，不是
 * 进度条。
 */
watch(
  () => writer.running,
  (now, before) => {
    if (before && !now) loadAll()
  },
)
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
            :disabled="isBusy('genall') || isBusy('bible') || (!characters.length && !locations.length)"
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
