<script setup>
/**
 * 设置。
 *
 * 所有环境和参数都收在这一页，别的地方一个配置项都不放。
 * 页面上只放控件和读数。一个控件要是非得解释才会用，解释进它的 title。
 *
 * 2026-09-14 重排。**打开这一页要回答的问题只有一个：能不能跑、不能的话
 * 卡在哪。** 答案是体检，而它原来排在 1200px 的表单后面、12 项全印
 * （包括 sd.cpp 的 `SSE3 = 1 | AVX = 1 …` 那种调试串）。现在体检在最上面，
 * 只摆没过的，全部折在「▸ 另外 N 项」里。
 *
 * 砍掉的：
 *   · 「出图出片」——两个只读数加一个去项目页的链接。画幅是项目的，
 *     换模型在项目页，步数是算出来的诊断，进体检一行。
 *   · 「外观」——顶栏那个月亮图标就是主题切换，这一节是它的第二份。
 *   · 引擎里的「权重放哪」「上次腾显存」——运行诊断，不是设置，归体检；
 *     显卡、画幅在引擎和体检里各说了一遍，留体检那份。
 *   · 装配里的「转场（秒）」——assemble.cpp 自己写着"xfade / acrossfade /
 *     fade= 都没有，转场从来没被渲染过"。能改、存得住、什么都不做，和
 *     09-13 删的「时长容差」同一类。
 * 装配和闸门合成一节：它们本来就是一个按钮一起存的。
 */
import { computed, onMounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import DirPicker from '@/components/DirPicker.vue'
import NodeMatrix from '@/components/NodeMatrix.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'
import { describeRoomDecision } from '@/composables/room-decision'
import { placementRows as buildPlacementRows } from '@/composables/placement-rows'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

const overview = ref(null)
const loading = ref(true)
/**
 * 这一趟读砸了的话，那句话。
 *
 * **没有它的时候，读砸和「还在读」长得一模一样。** 这一页上「引擎」那颗
 * 牌子判的是 `!overview`——而 overview 读砸了也是 null，于是它永远停在
 * 「检查中」。偏偏这一节的标题就是「引擎」，人来这儿正是要问一句"它到底
 * 通不通"，而「检查中」是所有答案里最容易让人干等的那一个。
 *
 * 也不能一律写「连不上」：500 是引擎活着但这一趟砸了（配置文件语法错、
 * 体检里某项抛了），和「没人应答」不是一回事。
 */
const loadError = ref('')

const node = ref({ engineBaseUrl: '', engineTimeoutMs: 600000 })
const conn = ref({})
const params = ref({})
const persist = ref(true)

// 模型往哪儿下。整台机器一套，和"挑哪个模型"（在项目页）是两件事。
const modelsDir = ref('')
/** 「浏览」那个弹窗开着没有。 */
const browsing = ref(false)
function openBrowse() {
  browsing.value = true
}
/**
 * 挑好了。**只填进输入框，不直接存**——这一节有自己的「保存」，而它上面
 * 还有一个「写回配置文件」的勾管着存到哪儿。挑完就存的话，那个勾就被绕过
 * 去了（这一节 2026-09-15 之前正好栽过这个）。
 */
function pickDir(path) {
  modelsDir.value = path
  browsing.value = false
}
const modelsSource = ref('')
const modelsSources = ref([])
const modelsTool = ref('')
const savedDir = ref('')
const savedSource = ref('')
const modelsDirty = computed(
  () => modelsDir.value !== savedDir.value || modelsSource.value !== savedSource.value,
)

async function loadModelsDir() {
  try {
    const d = await api.setupState()
    modelsDir.value = d.modelsDir || ''
    modelsSource.value = d.source || ''
    modelsSources.value = d.sources || []
    modelsTool.value = d.tool || ''
    savedDir.value = modelsDir.value
    savedSource.value = modelsSource.value
  } catch {
    // 读不到就留空。这一节不该让整页红——它装机时配一次，平时不看。
  }
}

async function saveModelsDir() {
  // **一组模型都不选**：这一节只管往哪儿下，不管下什么。引擎那边
  // `download: false` 加空 selections 就是"只存设置"（见 post_setup_download）。
  //
  // **`persist` 也要带上。** 这一页顶上那个「写回配置文件」勾原来管不到这
  // 一节：不勾着改模型目录，照样写进配置文件，还回一句「存好了」——而那个
  // 勾就在这颗按钮正上方，写着"不勾就是只对本次进程生效，重启就没了"。
  // 另外三节（引擎 / 配音 / 装配与闸门）走的 /api/settings 一直收它。
  const ok = await run(
    () =>
      api.startSetupDownload({
        selections: {},
        dir: modelsDir.value.trim() || undefined,
        source: modelsSource.value || undefined,
        download: false,
        persist: persist.value,
      }),
    {
      key: 'modelsDir',
      success: persist.value ? '存好了' : '已生效（重启后失效）',
    },
  )
  if (!ok) return
  await loadModelsDir()
}

const SECTIONS = [
  { id: 'doctor', title: '体检' },
  { id: 'look', title: '界面' },
  { id: 'engine', title: '引擎' },
  // 「大模型」那一节 2026-09-14 整个删了（用户：「加上 key，去掉设置里的
  // 大模型选择」）。服务、模型名、接口地址、密钥、温度全在项目页点模型名
  // 弹出来的那个窗口里——一件事分两页配，改完一处另一处还显示着旧的。
  // 「挑哪个模型」2026-09-14 搬去项目页了（用户的话："去掉设置页面的
  // 模型选择，改放进项目页面"）。**搬走的是"挑"，不是"往哪儿下"**——
  // 目录和下载源是整台机器共用的（"模型的路径放到设置里，这个全局统一的"），
  // 留在这儿。
  // ⚠️ **下面这三节只在引擎连得上时才渲染**（模板里那句
  // `<template v-if="engineOnline">`，理由写在那儿：读不到任何值，
  // 摆出来是一排空框）。导航这边也得跟着，否则连不上时这三个按钮照旧
  // 摆着、点下去 `scrollTo` 找不到那个 id，`?.` 一兜就是**什么都不发生**。
  // 而"引擎连不上"恰恰是最常打开这一页的时候（人就是来修连接的），
  // 那时候一半的导航是死的。
  { id: 'tts', title: '配音', needsEngine: true },
  { id: 'models', title: '模型目录和下载', needsEngine: true },
  { id: 'assembly', title: '装配与闸门', needsEngine: true },
]

const engineOnline = computed(() => Boolean(overview.value?.engine?.online))
/** 导航上摆哪几个：只摆这一刻真的在页面上的那几节。见 SECTIONS 里那段。 */
const shownSections = computed(() =>
  SECTIONS.filter((s) => !s.needsEngine || engineOnline.value),
)
/**
 * 体检里没过的那几项。
 *
 * **level 只有三个值：`ok` / `warn` / `fail`**（doctor.hpp 的 to_string
 * 写死的，那儿还专门写着"前端按这三个值上色"）。这行注释原来写的是
 * "warn / bad"、底下样式表里写的是 `check--error`——同一档东西三处三个
 * 名字，而三个里没有一个是线上真发的那个。见下面 .check--fail 那段。
 */
/**
 * 「能产什么」那一条在这一页上不显示——**同一页下面那张机器表画的就是它**。
 *
 * 体检里那条是一段文字：「写文、装配 / 配音：[models].tts 没配，或者文件
 * 不在 / 首帧：… / 出片：…」。而机器表本机那一行是同样五格，干不了的画
 * 一道短横、悬停给的是**同一句** why（引擎那边一处算的，node_json.cpp
 * 原样带过来）——还多了别的机器、还能点着开关。
 *
 * 同一件事在一屏里用两种说法讲两遍，人得先分辨"这两处说的是不是一回事"。
 * 表更全，留表。
 *
 * **不从体检里删掉**：命令行跑 doctor 的人没有那张表，那条对他们是唯一的
 * 来源。只是这一页不重复画。
 */
const kShownInNodeTable = '能产什么'
const failedChecks = computed(() =>
  (overview.value?.doctor?.checks ?? []).filter(
    (c) => c.level !== 'ok' && c.name !== kShownInNodeTable,
  ),
)
const okChecks = computed(() =>
  (overview.value?.doctor?.checks ?? []).filter(
    (c) => c.level === 'ok' && c.name !== kShownInNodeTable,
  ),
)

/**
 * 「配音」那一节要提交的那几项，和"读回来之后动过没有"。
 *
 * 摆出「未存」那个角标是有用的：这一页上每一节的保存都是独立的，
 * 改了一节去点另一节的保存按钮，什么都不会发生，而页面上原来没有任何
 * 迹象说明这件事。
 *
 * **llmPatch / savedLlm 2026-09-15 删了**：「大模型」那一节 09-14 整个
 * 搬去了项目页（点模型名弹出来那个窗口），这两个跟着成了死代码——
 * savedLlm 只被赋值、没有任何一处读它，llmPatch 只为了算 savedLlm 存在。
 */
const ttsPatch = computed(() => ({
  tts_backend: conn.value.tts_backend,
  tts_base_url: conn.value.tts_base_url ?? '',
}))
const savedTts = ref('')
const ttsDirty = computed(() => JSON.stringify(ttsPatch.value) !== savedTts.value)
/**
 * 「引擎」和「装配与闸门」那两节改过没有。
 *
 * 加它们不是为了再摆两个角标，是为了**重读的时候别把人正在改的东西抹掉**
 * ——见 load() 里那段。配音那节本来就有 `ttsDirty`（它还撑着页面上那个
 * 「未存」角标），这两节原来没有基准线，重读一下就没声不响地回去了。
 */
const savedNode = ref('')
const savedParams = ref('')
const nodeDirty = computed(() => JSON.stringify(node.value) !== savedNode.value)
const paramsDirty = computed(() => JSON.stringify(params.value) !== savedParams.value)
/** 第一次读之前，上面那三个"改过没有"都不作数（基准线还是空的）。 */
let loadedOnce = false

const nodeLocked = computed(() => node.value?.envLocked ?? {})

/**
 * 引擎是不是就是发这个页面的那个进程。
 *
 * 是的话「引擎地址」和「请求超时」两个输入框没有意义：它们是给 Node 那层
 * 转发用的，由引擎自己答时是空的，改了也没人读。显示出来只会让人以为
 * 哪里没配好——地址栏空着、旁边还挂个"已连接 0ms"。
 */
const embedded = computed(() => node.value?.embedded === true)

const hardware = computed(() => overview.value?.hardware)

/**
 * 这一轮**真正会用**的步数和画幅。
 *
 * **别拿 hardware.tiers.final 显示。** 那是档位表按显存推出来的，出片时
 * 会被两件事盖掉：画幅来自项目的 [video]，步数在挂了 Turbo 时压到 6。
 * 照着档位表显示的话，界面写"成片步数 28"而每一镜实际跑 6 步——
 * 用户看了会问"怎么没用 turbo"。真发生过（2026-09-10）。
 *
 * 引擎那边和 run.cpp 调的是同一个函数（config::effective_spec），
 * 两边不会分叉。
 */
const effective = computed(() => overview.value?.effective ?? null)

const placement = computed(() => effective.value?.placement ?? null)
const placementRows = computed(() => buildPlacementRows(placement.value))

/**
 * 最近一次「要不要腾地方」的判断，翻成人话。
 *
 * 这个判断错了的表现是"该留的时候卸了"（出片前白等几十秒重装大模型）
 * 或者"该卸的时候没卸"（显存爆掉，整个服务没了）。以前只能登上机器看
 * stderr——服务器连不上的时候这条线索就断了。摆在这儿，谁都看得见。
 */
const roomDecision = computed(() =>
  describeRoomDecision(placement.value?.lastRoomDecision),
)

/**
 * 这一趟是替哪部剧问的。
 *
 * ⚠️ **这一页 2026-09-15 才变成"跟项目走"的**（体检里「出片画布」那一项
 * 要按这部剧的画幅查），同时挂上了换项目就重拉的 watch。而
 * `/bff/settings/overview` **是全应用最慢的一条**：它里面整跑一遍体检，
 * 其中三项要发网络请求、各自 8 秒超时，最坏二十多秒。
 *
 * 也就是说这一页的两趟叠在一起不是零点几秒的窗口，是**几十秒**的窗口：
 * 在项目库里点 A 再点 B，A 那趟后落地就把 A 的体检结论、那颗
 * 「可以开工 / 还不能跑」的牌子写在 B 上，而顶栏写的是 B。
 * 这正是这个库里反复修过的那一类（session.refresh、AssetsView、EpFilm
 * 都有同一道闸），加这条 watch 时漏了。
 */
let loadSeq = 0

async function load() {
  const mine = ++loadSeq
  // **钉住路径再发**，不要在 await 之后现读：这一趟问的和下面写进去的
  // 必须是同一部剧。
  const want = session.projectPath
  loading.value = true
  try {
    // **带上顶栏选中的那部剧。** 这一份里的体检有一项是「出片画布」，
    // 而画幅是每部剧自己的——不带的话查的是全局默认，2K 的项目上这一节
    // 会说没问题、上面那颗牌子会写「可以开工」，而镜头页开跑前的体检
    // （走 /api/doctor，带了 path）会说超了。见 api.settingsOverview。
    const data = await api.settingsOverview(want)
    if (mine !== loadSeq) return
    overview.value = data
    loadError.value = ''
    // ⚠️ **改了还没存的那一节不要盖掉。**
    //
    // 这个 load 不只在进页面时跑：**体检那一节的「重新体检」按的就是它**，
    // 而那是这一页上最顺手的一个按钮。原来它三节一起整份覆盖——在「配音」
    // 里改了地址、顺手点一下重新体检，改的东西连同旁边那个「未存」角标
    // 一起没了，一句话都没有。而那个角标存在的全部理由（见 ttsPatch 上面
    // 那段）就是"这一页每节各存各的，别让人以为已经存上了"。
    //
    // 判据用各节自己的基准线；第一次读的时候基准线还是空的，所以要
    // `loadedOnce` 挡一下，否则首次进页面反而什么都装不进来。
    if (!loadedOnce || !nodeDirty.value) {
      node.value = { ...data.node }
      savedNode.value = JSON.stringify(node.value)
    }
    if (!loadedOnce || !ttsDirty.value) {
      conn.value = { ...(data.connections ?? {}) }
      // 基准线：这两句之后「未存」才说得准。放在这儿而不是保存成功那一下，
      // 是因为引擎可能没照单全收（被环境变量顶着的项就不会变）。
      savedTts.value = JSON.stringify(ttsPatch.value)
    }
    // 引擎把参数分了组，界面上摊平成一层，提交时再拆回去
    const s = data.settings
    if (s) {
      // **档位那六项不进来。** 「画质档位」那一节 2026-09-10 删了
      // （画幅搬到项目上），但字段当时还留在这儿——界面上没有控件，
      // 值却照样跟着「保存参数」写回配置文件。后果不是多存几行：
      // `[tiers].final_steps` 一旦有值，出片时的 Turbo 6 步就不再生效
      // （run.cpp 只在它是 0 时才动），每一镜悄悄变回 28 步、慢四倍，
      // 而界面上只说了一句"参数已保存到配置文件"。
      if (!loadedOnce || !paramsDirty.value) {
        params.value = {
          ...s.assembly,
          ...s.gates,
        }
        savedParams.value = JSON.stringify(params.value)
      }
    }
    loadedOnce = true
    for (const [key, message] of Object.entries(data.errors ?? {})) {
      ui.warn(`${key} 读不到：${message}`)
    }
  } catch (err) {
    // 过期那一趟的报错也不算数：上一部剧被删了回的 404 会在新这一部的
    // 页面上弹一句莫名其妙的红字。同 session.refresh 那处。
    if (mine !== loadSeq) return
    loadError.value = err.message
    ui.error(err.message)
  } finally {
    // 转圈只由最后那一趟关。被顶掉的那趟关掉的话，还在路上的那趟就没有
    // 任何"正在读"的表示了。
    if (mine === loadSeq) loading.value = false
  }
}

/** 开机自启的现状。null = 还没问到 / 这个平台不支持。 */
const autostart = ref(null)

async function loadAutostart() {
  try {
    autostart.value = await api.autostart()
  } catch {
    // 问不到就不摆那个开关：摆一个点了没反应的开关比不摆更糟。
    autostart.value = null
  }
}

async function toggleAutostart(on) {
  const r = await run(() => api.setAutostart(on), {
    key: 'autostart',
    // **失败要说原话。** 写不进去的原因（没权限、目录建不了）只有引擎
    // 知道，这儿编不出来。
  })
  if (r) {
    autostart.value = r
    ui.ok(on ? '开机会自己起来了（下次登录生效）' : '不再随系统启动')
  } else {
    // 没改成的话把开关拨回去——不拨的话它显示的是人点的那一下，
    // 而实际状态没变。
    await loadAutostart()
  }
}

/** 有没有新版。null = 还没问到。 */
const update = ref(null)

async function loadUpdate(manual) {
  const fn = () => api.checkUpdate(manual)
  const r = manual ? await run(fn, { key: 'update' }) : await fn().catch(() => null)
  if (r) update.value = r
  // **手动点的那一下要有回音**，哪怕结论是"已经是最新的"——没有回音的话
  // 人会以为按钮坏了。自动那一趟不吭声。
  if (manual && r && !r.error) {
    ui[r.newer ? 'info' : 'ok'](
      r.newer ? `有新版 ${r.latest}，手上是 ${r.current}` : '已经是最新的',
    )
  }
}

onMounted(() => {
  loadModelsDir()
  load()
  loadAutostart()
  loadUpdate(false)
})

// 换一部剧，体检里那条「出片画布」的答案就变了（画幅是每部剧自己的）。
// 不重拉的话这一节停在上一部那份上——而这一页没有任何地方写着它是给
// 哪部剧看的，停着的那份看上去就是当前这部的。
watch(() => session.projectPath, load)

/**
 * 「大模型」那条体检项上那颗一键改。
 *
 * 配着 `[llm].backend = "local"` 的机器，密钥填了、模型挑了、地址也在，
 * 写文那一格照样是灰的——而那条路 2026-09-14 就删了，只剩把 backend
 * 改成 remote 这一个正确答案。原来界面上没有任何地方能改它，体检那句
 * 「配置里把 [llm].backend 改成 remote」是叫人去手改配置文件。
 */
async function useRemoteLlm() {
  const ok = await run(() => api.useRemoteLlm(), {
    key: 'llmbackend',
    success: '改成外接了。写文这一步现在按你填的地址和密钥走',
  })
  if (ok) await load()
}

/** 这一条体检项是不是"配的是进程内跑"那一条。 */
function isDeadLlmBackend(c) {
  return c.name === '大模型' && c.detail.includes('进程内跑')
}

async function saveNode() {
  const result = await run(() => api.saveNodeConfig(node.value), {
    key: 'node',
    success: '引擎地址已保存',
  })
  if (result) {
    node.value = { ...result }
  // **存成功了就把基准线推平**，否则下面那次 load 会以为这一节
  // 还改着、跳过刷新，角标就永远挂在那儿了。见 load() 里那段。
    savedNode.value = JSON.stringify(node.value)
    await load()
  }
}
/**
 * 连接类设置。改了等于换一台干活的机器，保存完引擎会自动重新体检。
 *
 * **一节一个按钮，各存各的。** 2026-09-14 之前是一个按钮把大模型和配音
 * 一起提交，而那个按钮长在「配音」那一节里——用户在「大模型」挑完模型，
 * 整节上没有一个能点的东西，只能靠标题里那句"大模型和配音一起保存"猜。
 * 分开之后还有一个好处：改配音不会把大模型那几项也重写一遍。
 *
 * **密钥根本不经过这一页**：它和服务、模型、接口地址一起在项目页那个
 * 模型弹窗里填（ModelDialog 自己发 /api/connections）。这儿原来写的是
 * "它有自己的按钮（saveApiKey）"，而那个函数随「大模型」那一节一起
 * 09-14 删了——指着一个不存在的东西。
 */
async function saveConn(patch, key) {
  const result = await run(() => api.saveConnections({ patch, persist: persist.value }), {
    key,
  })
  if (!result) return
  ui.ok(
    result.changed?.length
      ? `改了 ${result.labels?.join('、') || result.changed.join('、')}`
      : '没有要改的项',
  )
  if (result.env_locked?.length) {
    ui.warn(`${result.env_locked.join('、')} 被环境变量顶着，重启还是环境变量那一套`)
  }
  // **存成功了就把基准线推平**，否则下面那次 load 会以为这一节
  // 还改着、跳过刷新，角标就永远挂在那儿了。见 load() 里那段。
  savedTts.value = JSON.stringify(ttsPatch.value)
  await load()
}

/** 「配音」那一节。`tts_engine` 不提交——引擎的白名单里没有这一项，
 *  而 /api/connections 也从来不回它，提交的是个 undefined。 */
const saveTts = () => saveConn(ttsPatch.value, 'conn')

async function saveParams() {
  const patch = {}
  for (const [k, v] of Object.entries(params.value)) {
    if (v === '' || v === null || v === undefined) continue
    patch[k] = typeof v === 'boolean' ? v : Number.isNaN(Number(v)) ? v : Number(v)
  }
  // 布尔项被上面的 Number 转换弄坏过一次，这里显式拨回来
  patch.gates_enabled = Boolean(params.value.enabled ?? params.value.gates_enabled)
  patch.fallback_on_exhausted = Boolean(params.value.fallback_on_exhausted)
  delete patch.enabled

  const result = await run(() => api.saveEngineSettings({ patch, persist: persist.value }), {
    key: 'params',
    success: persist.value ? '参数已保存到配置文件' : '参数已生效（重启后失效）',
  })
  if (result) {
    // **「照你填的没法做，按这个来了」要说出来。**
    //
    // 今天只有帧率会进这个数组：出片模型只出 24fps（MiniMax-H3），填 30
    // 引擎会纠回去——不纠的话整片快 25%，人走路变小跑，字幕跟着漂。引擎
    // 那句解释原来只 fprintf 到 stderr，而双击启动的人根本看不到 stderr：
    // 他看到的是填了 30、弹一句「参数已保存到配置文件」，然后那一格自己
    // 变回 24，一个字都没有。
    //
    // 用 warn 加长停留：这句话比一条绿提示长，3.2 秒读不完。
    for (const note of result.notes ?? []) ui.push('warn', note, 12000)
  // **存成功了就把基准线推平**，否则下面那次 load 会以为这一节
  // 还改着、跳过刷新，角标就永远挂在那儿了。见 load() 里那段。
    savedParams.value = JSON.stringify(params.value)
    await load()
  }
}

function scrollTo(id) {
  document.getElementById('sec-' + id)?.scrollIntoView({ behavior: 'smooth', block: 'start' })
}
</script>

<template>
  <div class="stack stack--lg">
    <div class="set">
      <!-- 小节导航 -->
      <nav class="secnav">
        <button
          v-for="s in shownSections"
          :key="s.id"
          class="secnav__item"
          type="button"
          @click="scrollTo(s.id)"
        >
          {{ s.title }}
        </button>
        <label
          class="secnav__persist"
          title="勾上：改动写回配置文件，重启还在。不勾：只对本次进程生效，重启就没了"
        >
          <input v-model="persist" type="checkbox" />
          <span>写回配置文件</span>
        </label>
      </nav>

      <div class="stack stack--lg set__body">
        <!-- 体检。**排最上面，只摆没过的。** 这一页要回答的就是"能不能跑、
             卡在哪"；过了的那十几项和引擎里那几段运行诊断全折在下面。
             大模型是黄字不是红字：出片那条路不用它。 -->
        <section id="sec-doctor" class="sec">
          <div class="sec__head">
            <h2 class="sec__t">体检</h2>
            <span
              v-if="overview?.doctor"
              class="pill"
              :class="overview.doctor.can_run ? 'pill--ok' : 'pill--danger'"
            >
              {{ overview.doctor.can_run ? '可以开工' : '还不能跑' }}
            </span>
            <!-- **没读回来之前不能说"连不上"。** `engineOnline` 是
                 `overview?.engine?.online`，而 overview 一开始是 null——
                 这一份要跑一遍体检（探 ffmpeg、扫字体、问显卡），一秒以上
                 是常事。不判 overview 的话，每次进这一页都先红一下。 -->
            <span v-else-if="overview && !engineOnline" class="pill pill--danger">引擎连不上</span>
            <span class="spacer" />
            <!-- 这一页不会自己更新（配置文件能在外面改），这是少数该有刷新的地方 -->
            <button class="btn btn--ghost btn--sm" type="button" :disabled="loading" @click="load">
              <AppIcon name="refresh" :size="14" :class="{ spin: loading }" />
              重新体检
            </button>
          </div>
          <div v-if="overview?.doctor" class="stack stack--sm">
            <div
              v-for="c in failedChecks"
              :key="c.name"
              class="check"
              :class="`check--${c.level}`"
            >
              <AppIcon name="warn" :size="14" />
              <span class="check__name nowrap">{{ c.name }}</span>
              <span class="check__detail">{{ c.detail }}</span>
              <span v-if="c.fix" class="check__fix tiny dim">{{ c.fix }}</span>
              <!-- **能一键改的就别只写一句"去改配置文件"。** 这一条只有
                   一个正确答案（backend → remote），引擎那头也一直收着
                   这个请求，缺的只是界面上这颗按钮。 -->
              <button
                v-if="isDeadLlmBackend(c)"
                class="btn btn--primary btn--sm check__act"
                type="button"
                :disabled="isBusy('llmbackend')"
                @click="useRemoteLlm"
              >
                {{ isBusy('llmbackend') ? '改着…' : '改成外接' }}
              </button>
              <!-- **缺模型那几条要给一颗按钮，不能只写「填 [models].tts」。**
                   那句话是叫人去手改配置文件，而挑模型下模型那套界面本来就
                   有。哪一组由引擎给（`c.group`），不是在这儿正则匹配那句
                   中文——那几句话一直在调，匹配挂了不会报错，只会悄悄少一颗
                   按钮。 -->
              <RouterLink
                v-else-if="c.group"
                class="btn btn--primary btn--sm check__act"
                :to="{ path: '/project', query: { model: c.group } }"
              >
                去挑模型
              </RouterLink>
            </div>
            <details class="fold">
              <summary class="fold__t">
                {{ failedChecks.length ? `另外 ${okChecks.length} 项都过了` : `全部 ${okChecks.length} 项都过了` }}
                <template v-if="embedded"> · 运行诊断</template>
              </summary>
              <div class="stack stack--sm">
                <div v-for="c in okChecks" :key="c.name" class="check check--ok">
                  <AppIcon name="check" :size="14" />
                  <span class="check__name nowrap">{{ c.name }}</span>
                  <span class="check__detail">{{ c.detail }}</span>
                </div>
                <div class="check check--ok">
                  <AppIcon name="check" :size="14" />
                  <span class="check__name nowrap">步数</span>
                  <span class="check__detail mono">
                    出片 {{ effective?.finalSteps ?? '—' }}
                    <span
                      v-if="effective?.turbo"
                      class="pill pill--ok tiny"
                      :title="`挂着 Turbo LoRA，出视频按 6 步走（档位表推的是 ${effective?.tableSteps} 步）`"
                    >Turbo</span>
                    <span v-else-if="effective?.stepsPinned" class="pill pill--neutral tiny">配置里写死</span>
                    · 首帧 {{ effective?.frameSteps ?? '—' }}
                  </span>
                </div>
                <!-- 程序给两个模型算出来的权重放置。「够就不清理」判的是实测那个数。 -->
                <div v-if="embedded && placement" class="field">
                  <span class="field__label">权重放哪</span>
                  <div class="stack stack--sm">
                    <div v-for="row in placementRows" :key="row.key" class="mono tiny">
                      {{ row.label }}：
                      <span :class="row.resident ? 'pill pill--ok tiny' : 'pill pill--warn tiny'">
                        {{ row.resident ? '常驻显存' : '放内存' }}
                      </span>
                      <template v-if="row.modelGb > 0">
                        模型 {{ row.modelGb.toFixed(1) }} GB · 估 {{ row.liveVramGb.toFixed(1) }} GB<template
                          v-if="row.measured !== null"
                        > · <strong>实测 {{ row.measured.toFixed(1) }} GB</strong><template
                          v-if="row.measuredWorkMp !== null"
                        >（{{ row.measuredWorkMp }} MP·帧）</template></template>
                      </template>
                      <template v-else>（模型没配或读不到）</template>
                    </div>
                  </div>
                </div>
                <!-- 最近一次腾地方的判断。没发生过就不显示。 -->
                <div v-if="embedded && roomDecision" class="field">
                  <span class="field__label">上次腾显存</span>
                  <div v-if="roomDecision.skipped" class="mono tiny">
                    借「{{ roomDecision.slot }}」：{{ roomDecision.verdict }}
                  </div>
                  <div v-else class="mono tiny">
                    借「{{ roomDecision.slot }}」：需要 {{ roomDecision.need }}（{{ roomDecision.needHow }}）·
                    空闲 {{ roomDecision.free }}（{{ roomDecision.how }}）
                    <span :class="roomDecision.ok ? 'pill pill--ok tiny' : 'pill pill--warn tiny'">
                      {{ roomDecision.verdict }}
                    </span>
                  </div>
                  <!-- 判「够」却是拿估的判的：这正是 CUDA OOM 的前一步。 -->
                  <span
                    v-if="!roomDecision.skipped && roomDecision.ok && !roomDecision.needTrusted"
                    class="tiny warn-text"
                  >
                    这次的「够」是估算判的，这个槽还没量过，真跑可能不够
                  </span>
                </div>
              </div>
            </details>
          </div>
        </section>

        <!-- 界面。**这一节是拖拽换边的替代品。**
             项目库那条栏原来靠拖栏头换边（约 110 行 + 一个只为
             setPointerCapture 存在的守卫函数），而靠哪边一辈子设一次，
             是「改的时候才要的」。主题不在这儿：顶栏右上那个图标就是它。 -->
        <section id="sec-look" class="sec">
          <div class="sec__head">
            <h2 class="sec__t">界面</h2>
          </div>
          <label class="field">
            <span class="field__label">项目库靠哪边</span>
            <select v-model="ui.railSide" class="select">
              <option value="right">右边</option>
              <option value="left">左边</option>
            </select>
          </label>

          <!-- **「复制提示词」那一排按钮的总开关。**
               用户 2026-09-17 要的第二句：「增加一个统一的控制开关，后期可以
               直接关闭」。关掉之后每个用大模型的地方那颗按钮一起消失。
               记在这台机器上（localStorage），和上面靠哪边一个规矩——它是
               "我这台机器上怎么用"，不是"这部剧怎么拍"。 -->
          <label class="field">
            <span class="field__label">复制提示词按钮</span>
            <span class="row row--wrap">
              <label class="switch tiny">
                <input v-model="ui.showCopyPrompt" type="checkbox" />
                <span>{{ ui.showCopyPrompt ? '开着' : '关着' }}</span>
              </label>
              <span class="tiny dim">
                {{
                  ui.showCopyPrompt
                    ? '每个用大模型的地方旁边有一颗，抄走那一步真正要发的提示词，可以拿到别处去跑。'
                    : '那几颗按钮都不显示。'
                }}
              </span>
            </span>
          </label>

          <!-- **开机自启。** 三个平台都只是往登录时系统会扫的那个目录写一个
               文件（见 cpp/src/setup/autostart.hpp）。
               ⚠️ **那句「下次登录才生效」必须写出来**：写完文件这一次并不会
               自动起，人打开开关之后去别处看不到任何变化，会以为开关没用。 -->
          <label v-if="autostart?.supported" class="field">
            <span class="field__label">随系统启动</span>
            <span class="row row--wrap">
              <label class="switch tiny">
                <input
                  type="checkbox"
                  :checked="autostart.enabled"
                  :disabled="isBusy('autostart')"
                  @change="toggleAutostart($event.target.checked)"
                />
                <span>{{ autostart.enabled ? '开着' : '关着' }}</span>
              </label>
              <span class="tiny dim">
                {{
                  autostart.enabled
                    ? '下次登录这台机器时自己起来。这一次不会自动起。'
                    : '登录后要自己开一次。'
                }}
              </span>
            </span>
          </label>
          <p v-if="autostart?.enabled" class="tiny dim" :title="autostart.path">
            开机跑的是 <code class="mono">{{ autostart.command }}</code>
          </p>

          <!-- **更新。** 只查，不换二进制——换掉正在跑的可执行文件三个平台
               三种做法，而换错了程序就起不来，那时候界面也没了。所以这儿给
               的是"有没有新的"加一条过去的路。 -->
          <label class="field">
            <span class="field__label">更新</span>
            <span class="row row--wrap">
              <span v-if="!update" class="tiny dim">正在问…</span>
              <template v-else-if="update.error">
                <span class="tiny warn">{{ update.error }}</span>
              </template>
              <template v-else-if="update.newer">
                <span class="pill pill--warn tiny nowrap">有新版</span>
                <span class="tiny">
                  <code class="mono">{{ update.latest }}</code>
                  <span class="dim">（手上是 {{ update.current }}）</span>
                </span>
                <a class="btn btn--primary btn--sm" :href="update.url" target="_blank" rel="noreferrer">
                  去下载
                </a>
              </template>
              <template v-else>
                <span class="tiny dim">
                  已经是最新的（<code class="mono">{{ update.current }}</code>）
                </span>
              </template>
              <button
                class="btn btn--ghost btn--sm"
                type="button"
                :disabled="isBusy('update')"
                @click="loadUpdate(true)"
              >
                {{ isBusy('update') ? '查着…' : '现在查一次' }}
              </button>
            </span>
          </label>
        </section>

        <!-- 引擎 -->
        <section id="sec-engine" class="sec">
          <div class="sec__head">
            <h2 class="sec__t">引擎</h2>
            <span
              class="pill"
              :class="
                loadError
                  ? 'pill--danger'
                  : !overview
                    ? 'pill--neutral'
                    : engineOnline
                      ? 'pill--ok'
                      : 'pill--danger'
              "
            >
              {{
                loadError
                  ? '读不到'
                  : !overview
                    ? '检查中'
                    : engineOnline
                      ? `已连接 ${overview.engine?.latencyMs}ms`
                      : '连不上'
              }}
            </span>
            <span class="spacer" />
            <div v-if="!embedded" class="sec__acts">
              <button
                class="btn btn--primary btn--sm"
                type="button"
                :disabled="isBusy('node')"
                @click="saveNode"
              >
                保存
              </button>
            </div>
          </div>
          <div class="stack">
            <!-- 读砸了：把引擎自己那句话原样摆出来。「连不上」那条说的是
                 引擎没应答，这一条说的是这一趟问砸了——两件事分开说。 -->
            <p v-if="loadError" class="alert alert--bad">
              <AppIcon name="warn" :size="15" />
              读不到这一页要的那份：{{ loadError }}
            </p>
            <p v-else-if="overview && !engineOnline" class="alert alert--bad">
              <AppIcon name="warn" :size="15" />
              {{ overview?.engine?.error || '引擎离线' }}
            </p>

            <div v-if="!embedded" class="grid grid--2">
              <label class="field">
                <span class="field__label">
                  引擎地址
                  <span v-if="nodeLocked.engineBaseUrl" class="pill pill--warn tiny">
                    被 {{ nodeLocked.engineBaseUrl }} 顶着
                  </span>
                </span>
                <input
                  v-model="node.engineBaseUrl"
                  class="input mono"
                  placeholder="http://127.0.0.1:8000"
                  title="changji web 跑起来之后监听的地址"
                />
              </label>
              <label class="field">
                <span class="field__label">超时（毫秒）</span>
                <input
                  v-model.number="node.engineTimeoutMs"
                  class="input numeric"
                  type="number"
                  step="1000"
                  title="出分镜要几分钟，别调太小"
                />
              </label>
            </div>
            <!-- 自带引擎时，地址和超时那两栏是空的，换成跑在什么机器上。
                 显存显示探到的那个数（effective），不是配置里顶着的。 -->
            <div v-if="embedded" class="field">
              <span class="field__label">显卡</span>
              <span class="mono">
                {{ hardware?.gpu || '未探测到显卡' }}
                <template v-if="effective?.physicalVramGb">
                  · {{ Number(effective.physicalVramGb).toFixed(1) }} GB
                </template>
              </span>
              <span v-if="effective?.vramOverride" class="tiny warn-text">
                配置里 vram_gb_override = {{ effective.vramOverride }} 顶着这张卡，把那一行删掉
              </span>
            </div>

            <p class="tiny dim mono">配置文件：{{ node.configFile }}</p>

            <!-- 「机器 × 能力」那张表。本机也是其中一行——本地和远程是
                 同一张表上的两个格子，不是两套设置。 -->
            <NodeMatrix v-if="engineOnline" />
          </div>
        </section>

        <!-- 引擎连不上时下面这几节一个都别摆：它们全是"这台引擎怎么配"，
             而那时候读不到任何值，摆出来是一排空框。 -->
        <template v-if="engineOnline">

          <!-- 配音 -->
          <section id="sec-tts" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">配音</h2>
              <span v-if="ttsDirty" class="pill pill--warn tiny">未存</span>
              <span class="spacer" />
              <div class="sec__acts">
                <!-- **只存配音这两项。** 以前这个按钮连大模型那几项一起提交，
                     而大模型那一节自己没有按钮——见 saveConn 上面那段。 -->
                <button
                  class="btn btn--primary btn--sm"
                  type="button"
                  :disabled="isBusy('conn')"
                  title="保存配音后端和地址，保存完重新体检"
                  @click="saveTts"
                >
                  {{ isBusy('conn') ? '保存中…' : '保存' }}
                </button>
              </div>
            </div>
            <div class="grid grid--2">
              <label class="field">
                <span class="field__label">后端</span>
                <select v-model="conn.tts_backend" class="select">
                  <option value="local">内置</option>
                  <option value="http">HTTP 服务</option>
                </select>
              </label>
              <!-- 选「内置」时不摆一个灰掉的空框，选 HTTP 才出现 -->
              <label v-if="conn.tts_backend === 'http'" class="field">
                <span class="field__label">服务地址</span>
                <input v-model="conn.tts_base_url" class="input mono" placeholder="必填" />
              </label>
              <!-- **这里原来还有「时长容差」和「变速安全区」两个输入框，
                   2026-09-13 删了。** 它们和 [tts].engine 一样：有校验、
                   有持久化、能改，但引擎里**没有任何一个阶段读过**，而
                   提示语写的是"超出靠尾帧冻结或变速吸收"——那个行为不存在。
                   台词装不下实际走的是按实测语速重切一次再合成
                   （stages/audio.cpp）。一个能改却什么都不做的旋钮比没有
                   更糟：人调完以为生效了。 -->
            </div>
          </section>

          <!-- 模型目录和下载。**只管往哪儿下，不管下什么**——挑模型在项目页。 -->
          <section id="sec-models" class="sec">
            <div class="sec__head">
              <h2 class="sec__t" title="整台机器共用；挑哪个模型在项目页">模型目录和下载</h2>
              <span class="spacer" />
              <button
                class="btn btn--primary btn--sm"
                type="button"
                :disabled="!modelsDirty || isBusy('modelsDir')"
                @click="saveModelsDir"
              >
                {{ isBusy('modelsDir') ? '存着…' : '保存' }}
              </button>
            </div>
            <div class="grid grid--2">
              <label class="field">
                <span class="field__label">模型目录</span>
                <!-- **敲路径这件事本来就不该靠记。** 模型动辄几十 GB，人挑的
                     是"哪块盘还装得下"，而那个路径多半在别的窗口里。旁边这
                     颗「浏览」把目录树摆出来，点进去、点「用这个」。 -->
                <div class="row">
                  <input v-model="modelsDir" class="input mono" placeholder="留空用默认位置" />
                  <button
                    class="btn btn--ghost btn--sm"
                    type="button"
                    title="翻一翻，挑个文件夹"
                    @click="openBrowse"
                  >
                    浏览
                  </button>
                </div>
              </label>
              <label class="field">
                <span class="field__label">下载源</span>
                <select v-model="modelsSource" class="select">
                  <option v-for="o in modelsSources" :key="o.id" :value="o.id">
                    {{ o.label }}<template v-if="o.note"> · {{ o.note }}</template>
                  </option>
                </select>
              </label>
            </div>
            <!-- **没有下载器才提示怎么装。** 原来这两行安装命令是无条件印在
                 页面上的，而装了 aria2 的机器上它一年也用不着。 -->
            <!-- `bad` 这个类只在 ModelDialog 自己的 scoped 里有定义，
                 scoped 跨不过组件——这一行在这儿是**不红的**，和旁边的
                 说明文字一个样。而它正是"为什么模型下不下来"的唯一答案。
                 换成 base.css 里那个全局的。 -->
            <p v-if="!modelsTool" class="tiny danger-text">
              这台机器上没找到下载器（aria2c 或 curl），下不了模型。
              Debian/Ubuntu 装：apt-get install -y aria2；Windows：winget install aria2.aria2
            </p>
          </section>

          <!-- 装配与闸门。**一个按钮一起存，就是一节。** 原来是两节，
               「保存」只在闸门那节、提示写着"装配和闸门一起保存"。 -->
          <section id="sec-assembly" class="sec">
            <div class="sec__head">
              <h2 class="sec__t">装配与闸门</h2>
              <span class="spacer" />
              <div class="sec__acts">
                <button
                  class="btn btn--primary btn--sm"
                  type="button"
                  :disabled="isBusy('params')"
                  @click="saveParams"
                >
                  {{ isBusy('params') ? '保存中…' : '保存' }}
                </button>
              </div>
            </div>
            <div class="grid grid--3">
              <label class="field">
                <span class="field__label">帧率</span>
                <input v-model.number="params.fps" class="input numeric" type="number"
                min="1" max="120" />
              </label>
              <label class="field">
                <span class="field__label">每集多长（秒）</span>
                <input
                  v-model.number="params.episode_s"
                  class="input numeric"
                  type="number"
                  min="0"
                  max="3600"
                  step="5"
                  title="填了就是章模式：一章按它自己的内容写完、拍完，最后按这个数切成几集，能切出几集是这一章内容的结果。0 = 老的一集一章：剧本按目标时长写，不够凑、超了压。"
                />
              </label>
              <label class="field">
                <span class="field__label">CRF</span>
                <input
                  v-model.number="params.crf"
                  class="input numeric"
                  type="number"
                  min="0"
                  max="51"
                  title="越小画质越好、文件越大。18 是常用值"
                />
              </label>
              <!-- 「转场（秒）」删了：assemble.cpp 写着"xfade / acrossfade /
                   fade= 都没有，转场从来没被渲染过"。配置键留着，界面上不摆
                   一个什么都不做的旋钮。 -->
              <label class="field">
                <span class="field__label">字幕字体</span>
                <input v-model="params.subtitle_font" class="input" />
              </label>
              <label class="field">
                <span class="field__label">单行字数</span>
                <input
                  v-model.number="params.subtitle_max_chars_per_line"
                  class="input numeric"
                  type="number"
                min="6" max="30"
                />
              </label>
              <label class="field">
                <span class="field__label">最多几行</span>
                <input v-model.number="params.subtitle_max_lines" class="input numeric" type="number"
                min="1" max="3" />
              </label>
            </div>
            <div class="stack">
              <label class="switch">
                <input v-model="params.enabled" type="checkbox" />
                <span>闸门开启</span>
              </label>
              <div class="grid grid--3">
                <label class="field">
                  <span class="field__label">每镜重试</span>
                  <input
                    v-model.number="params.max_attempts_per_shot"
                    class="input numeric"
                    type="number"
                    min="1"
                  />
                </label>
                <label class="field">
                  <span class="field__label">标准差下限</span>
                  <input
                    v-model.number="params.min_pixel_std"
                    class="input numeric"
                    type="number"
                min="0"
                    step="0.5"
                    title="拦纯色和噪点。调高会误杀暗场"
                  />
                </label>
                <!-- **这里原来还有「首帧相似度下限」一个输入框，2026-09-15
                     删了。** 和下面那两个（「时长容差」「变速安全区」）一模
                     一样：有校验、有持久化、能改，而**引擎里没有任何一处读过
                     `gates.min_frame_similarity`**——gates/checks.cpp 里根本
                     没有"拿某一帧和首帧比"这回事，那儿唯一沾边的是「片中亮度
                     剧烈跳变」，比的是相邻取样点，阈值还是写死的 60。
                     而那个框的提示写着「拦画面跑飞。运动大的片子要调低」，
                     人调完只会以为生效了。一个能改却什么都不做的旋钮比没有
                     更糟。字段留着（老配置里有、接口白名单还收），从接口改
                     它的话回执的 notes 里会说一句"存下来了但不生效"。 -->
                <label class="field">
                  <span class="field__label">台词偏差上限（秒）</span>
                  <input
                    v-model.number="params.max_audio_drift_s"
                    class="input numeric"
                    type="number"
                min="0.05"
                    step="0.05"
                  />
                </label>
                <label class="field">
                  <span class="field__label">响度（LUFS）</span>
                  <input
                    v-model.number="params.target_lufs"
                    class="input numeric"
                    type="number"
                    step="0.5"
                    title="短视频平台一般收 -16 到 -14"
                  />
                </label>
              </div>
              <!-- 原来这里写「重试超限降级为静帧加运镜」。**没有那回事**：
                   引擎的 fallback 只是留着最后那一版视频，全代码库一处
                   zoompan 都没有（2026-09-13 查过）。 -->
              <label
                class="switch"
                title="关掉的话，重试超限的镜头会标成「未过闸门」，整集停在那儿等人"
              >
                <input v-model="params.fallback_on_exhausted" type="checkbox" />
                <span>重试超限就留着最后那一版（没过闸门也照用）</span>
              </label>
            </div>
          </section>

        </template>

      </div>
    </div>
    <!-- 挑模型目录。**摆在根 div 里面，靠 Teleport 挂到 body 上。**

         它是个遮罩弹窗，嵌在某一节里会被那一节的 overflow 裁掉，所以原来
         和根 div **并排**摆在最外层——而那让这一页成了**多根组件**。

         ⚠️ 代价大得离谱，2026-09-17 才查出来：App.vue 里路由那一层是
         `<Transition name="fade" mode="out-in">`，而 `<Transition>` 要的是
         **单个根元素**。多根的那一页离开时那次 leave 永远不结束，`out-in`
         于是再也不放新的进来——**从设置页切到任何一页都是白的，而且之后
         每一次跳页都白，直到整页刷新**（用户 2026-09-17：「在设置页面切到
         别的页面要刷新才能看到内容」）。一个字的报错都没有。

         Teleport 两头都满足：DOM 上它仍然挂在 body 底下、不被谁的 overflow
         裁；模板这头它在根 div 里面，这一页回到单根。 -->
    <Teleport to="body">
      <DirPicker
        :open="browsing"
        :start="modelsDir"
        @close="browsing = false"
        @pick="pickDir"
      />
    </Teleport>
  </div>
</template>

<style scoped>
.fold__t {
  color: var(--text-3);
  font-size: var(--fs-xs);
  cursor: pointer;
}
.fold[open] > .fold__t {
  margin-bottom: 6px;
}
.set {
  display: grid;
  grid-template-columns: 168px minmax(0, 1fr);
  gap: var(--s6);
  align-items: start;
}

/* 小节导航：一列安静的字，不做成按钮。 */
.secnav {
  position: sticky;
  top: var(--s4);
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.secnav__item {
  padding: 3px 0;
  border: none;
  background: none;
  color: var(--text-3);
  font-size: var(--fs-sm);
  cursor: pointer;
  text-align: left;
}
.secnav__item:hover {
  color: var(--text);
}
.secnav__persist {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin-top: var(--s3);
  font-size: var(--fs-xs);
  color: var(--text-3);
  cursor: pointer;
}
.secnav__persist input {
  accent-color: var(--accent);
  width: 14px;
  height: 14px;
}

.grid--2 {
  grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
}
/* 这儿原来有 .field--wide（平台选择器独占一行）和 .field--narrow。
   那个选择器 2026-09-14 随「大模型」整节搬去了项目页那个模型弹窗，
   这一页的模板里再没有哪个元素带这两个类。 */
.grid--3 {
  grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
}

/* 这儿原来有 .linkbtn（长得像链接的小按钮，给「重新拉一次」那种动作用）。
   这一页现在只剩体检那颗「重新体检」，它是正经的 .btn--ghost。 */

.alert {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
  padding: var(--s3);
  border-radius: var(--r);
  font-size: var(--fs-base);
  line-height: 1.6;
}
.alert--bad {
  background: var(--danger-soft);
  color: var(--danger);
}

.check {
  display: flex;
  align-items: baseline;
  gap: var(--s2);
  padding: var(--s2) var(--s3);
  border-radius: var(--r-sm);
  background: var(--bg-sunken);
  font-size: var(--fs-sm);
  line-height: 1.5;
  flex-wrap: wrap;
}
.check :deep(svg) {
  align-self: center;
  color: var(--text-3);
}
.check--ok :deep(svg) {
  color: var(--ok);
}
.check--warn {
  background: var(--warn-soft);
}
.check--warn :deep(svg) {
  color: var(--warn);
}
/* ⚠️ **这一档的类名是 `fail`，不是 `error`。**
   引擎发的只有 ok / warn / fail 三个值（doctor.hpp::to_string），而这儿
   原来写的是 `.check--error`——拼出来的 `check--fail` 没有任何规则命中，
   于是**最严重的那几项反而最不显眼**：warn 有黄底黄图标，fail 落回
   .check 的灰底加一个灰图标，比警告还淡。而这一节就是拿来看"卡在哪"的，
   「还不能跑」那颗红丸子指的正是这几条。
   ProjectView 的进度条栽过同一种：拼出来的 modifier 一个都没定义。 */
.check--fail {
  background: var(--danger-soft);
}
.check--fail :deep(svg) {
  color: var(--danger);
}
.check__name {
  font-weight: 600;
  width: 7em;
}
/* 「怎么办」那一截。**引擎是按多行写的**——FFmpeg 那条给了 Windows /
   macOS / Debian 各一行，还按冒号对齐。这个类名模板里一直挂着，而规则
   一条都没有，于是换行全塌成空格，三条命令首尾相连挤成一行；README 里
   那句「缺什么它会说，并且给出怎么办」到这儿就只剩前半句。
   `pre-wrap` 不是 `pre-line`：后者会把 `macOS:   brew` 里对齐用的空格
   也并掉。整行占满一格，别和名字、detail 挤在同一行上。 */
.check__fix {
  flex-basis: 100%;
  white-space: pre-wrap;
}
/* **这一段也可能是多行的。** 「配了 5 项，其中 2 项的文件不存在：」后面
   跟的是一行一个文件名（doctor.cpp 里 `detail += "\n  " + x`），出图后端
   那条也是版本加一行编译信息。没有这一句的话它们塌成一条，那串文件名挤在
   一起反而看不出是几个。和下面 .check__fix 同一个理由、同一个取值——
   pre-wrap 而不是 pre-line：行首那两个空格是列表的缩进，要留住。 */
.check__detail {
  flex: 1;
  min-width: 12ch;
  white-space: pre-wrap;
}

/* 这儿原来有 .is-on（「外观」那三个主题按钮里选中的那个点亮）。
   「外观」那一节 2026-09-14 删了——顶栏那个月亮图标就是主题切换，
   见文件开头那份砍掉清单。 */

.spin {
  animation: spin 0.9s linear infinite;
}
@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}

@media (max-width: 900px) {
  /* ⚠️ **`minmax(0, 1fr)`，不是光一个 `1fr`。** 宽屏那条写对了，这条没有，
     而差别只在窄屏上看得见：`1fr` 的自动最小值是 min-content，于是这一列
     被里面**最不肯换行的那个东西**撑开——375px 上实测是那张「机器 × 能力」
     的表（六列中文表头加一颗按钮，min-content 357.5px），把整张设置页顶到
     374 宽，页面横着能推、屏幕上什么都不说，而「重新体检」「保存」这些都
     被推到侧边那条栏底下。摘掉那张表再量就是 282，正好放得下。
     （那张表自己也要能横滚，见 NodeMatrix 的 .matrix__scroll——两边缺一个
     都不行：这条只让列能缩，表还是会溢出来。） */
  .set {
    grid-template-columns: minmax(0, 1fr);
  }
  /* **换行，不是横着滚。** 这一排最后挂着「写回配置文件」那个勾，而它
     管着下面每一颗保存（勾掉就是"只对本次进程生效"）。原来这儿是
     `nowrap` 加一条不画滚动条的横滚——375px 上实测那个勾整个落在容器外
     8px 处，屏幕上看不见，也没有任何东西说这一排还能滑。
     上面那几个是小节跳转，换到第二行照样点得到；那个勾必须点得到。 */
  .secnav {
    position: static;
    flex-direction: row;
    flex-wrap: wrap;
    gap: var(--s3);
    padding-bottom: var(--s2);
  }
  .secnav__item {
    white-space: nowrap;
  }
  .secnav__persist {
    margin-top: 0;
    white-space: nowrap;
  }
}
/* 配置里有 vram_gb_override 顶着真实显存、估算判「够」那类提醒 */
</style>
