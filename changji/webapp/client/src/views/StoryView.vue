<script setup>
/**
 * 故事。一个编辑器，照代码编辑器的骨架排。
 *
 * 用户 2026-09-11 几次定方向，最后一次是：
 *
 *   「重新理解故事页面，重点是写作窗口，其他都是辅助功能」
 *
 * 落成三栏加一行：
 *
 *   章节（左，可收） │ 正文（独占滚动） │ 对话（右，默认收起）
 *   状态栏：字数 · 存没存 · AI 在干什么
 *
 * **正文那一格是唯一的滚动容器。** 页面本身不滚（外壳按 `wide` 关掉了
 * 滚动）。之前是页面在滚、稿纸在滚、textarea 又靠 JS 撑高，三层抢同一个
 * 高度——fit() 在版面没排好时量了一次，一章 343 字被算成 8803px，右下角
 * 那排按钮在第一屏往下 8000px 的地方。现在高度由 flex 定，textarea 还是
 * 跟内容长，但只在这一格里长，而且宽度一变就重量（ResizeObserver）。
 *
 * **没有东西浮在字上面。** 对话原来是右下角弹出的 320px 面板，正好压住
 * 正文列的右半。它要装引用、来回、撤销、输入框，本来就是侧栏的体量，
 * 现在停靠在右边，打开时正文让位。选中一段才浮出来的那两个小按钮是唯一
 * 的例外，它们贴在这一格底边中间，不在字上。
 *
 * **存是自动的。** 停笔 1.5 秒、换章、失焦、离开都存，状态栏写「存着…／
 * 已存」。之前五个地方在管存（失焦、Ctrl+S、脏了冒出的按钮、存下改的 N 章、
 * AI 面板里的存下来），写东西的人不该管持久化。Ctrl+S 留着，无声。
 *
 * **AI 往这一章写字的时候锁章。** 之前人还能打字，下一个 token 一到 buf
 * 就被覆盖，人打的字直接没了。
 *
 * 保留的几条判断：
 *
 * - **编辑器用 textarea，不是 contenteditable。** selectionStart/End 直接
 *   就是偏移；contenteditable 每次输入都可能重排节点，偏移随时失效——那正是
 *   "改到一半突然替换错地方"的来源。
 * - **偏移一律换算成 Unicode 码点再送给引擎。** 浏览器给的是 UTF-16 单元，
 *   碰上代理对差一个，而差一个就切在半个字上。
 * - **请求的返回才是权威的那一份。** 流出来的是原始 token，返回那份剥过
 *   包装（``` 代码块、「修改后：」），收尾时拿它覆盖一次。
 * - **一次一章。** 十六章连着的话滚动条本身就是干扰。书的形状在左栏。
 */
import { computed, nextTick, onMounted, onUnmounted, reactive, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api, mediaUrl } from '@/api'
import { useAction } from '@/composables/useAction'
import { runAsyncJob } from '@/composables/useAsyncJob'
import { openJobSocket } from '@/composables/useJobSocket'
import { useThinking } from '@/stores/thinking'
import { readLocal, writeLocal } from '@/composables/local-storage'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'
import { useWriter } from '@/stores/run'

const session = useSession()
const ui = useUi()
const thinking = useThinking()
const writer = useWriter()
const { run, isBusy } = useAction()
// **存稿走自己的一条线。** useAction 一次只跑一件事，而改一段要跑一分钟——
// 存稿排在它后面的话，这一分钟里敲的字一个都存不下去。
const saver = useAction()

const story = ref(null)
const loading = ref(false)
/** 手里这本书是哪部剧读回来的。null = 手上没有。 */
let loadedFor = null
/** 上一趟为什么没读回来。空 = 没出事。提示条八秒就没，这一行不会。 */
const loadError = ref('')
const draft = ref(null) // AI 写完、还没采用的那一份大纲
const keywords = ref('')
const pasting = ref(false)
const pasted = ref('')
const premise = ref('')
const savedPremise = ref('')
/**
 * AI 想出来的几个选题，还没挑。空数组 = 没在挑。
 *
 * **和大纲那条路是两件事。** 「让 AI 写一份大纲」是一步到位：一分钟后
 * 回来一整本书的骨架，人只能整份采用或整份丢弃。而多数人卡住的地方在更
 * 前面——梗概那个框里一个字都没有。这一条只要十几秒，回来三个方向，
 * 挑一个填进框里，接下来是写是改都还是人说了算。
 *
 * 引擎那头 2026-09-13 就写好了（`/api/script/premise`，连顶栏那本账都
 * 记着「想梗概」），只是界面上一直没有入口：梗概框的占位符写着"没想好
 * 就空着，让 AI 来一个"，而页面上没有任何一个按钮能让 AI 来一个。
 */
const ideas = ref([])

// **体量在这一页，每集时长在「设定 · 分集」。** 两个数看着像一对，其实是
// 两件事：体量是"这个故事有多长"（创作，写大纲时就要定），每集时长是
// "把它切成多长一段"（设定，什么时候改都行）。
const SCALES = [
  { key: 'short', label: '短篇', hint: '四章左右，一口气讲完' },
  { key: 'medium', label: '中篇', hint: '八章左右' },
  { key: 'long', label: '长篇', hint: '十六章左右，主线能铺开' },
]
const scale = ref('medium')

// ---- 编辑器 ----
/** chapter_id -> 编辑中的正文。落库的那份在 story 里，这份是手里的稿子。 */
const buf = reactive({})
/** chapter_id -> <textarea> 元素，插字和调高度要用。 */
const boxes = reactive({})
/** 选中的那一段：{chapter_id, from, to, text}，偏移是码点。 */
const sel = ref(null)
/** 输入框里**此刻**是不是真有一段选着。那两个小按钮只在这时候浮出来。 */
const liveSel = ref(false)
/** 这一段上聊过的来回。换一段就清空——"再短一点"是相对上一版说的。 */
const chat = ref([])
const instruction = ref('')
/**
 * AI 刚插进去的那一段。
 *
 * 留着改之前的**整章**正文，「撤销」就是把它放回去——比记住一段区间可靠：
 * 插进去之后用户可能又手动改了两个字，按区间回退会退错地方。
 * `after` 是插完那一刻的整章，Ctrl+Z 只在稿子还是这一份时才当"撤销改写"。
 */
const pending = ref(null)
/** 正在流式写入的那一段，非空时编辑器里那几个字正一个个冒出来。 */
const streaming = ref(null)
/** 当前在看哪一章。空串表示还没挑（进来时自动挑第一章）。 */
const current = ref('')
/** 光标落在正文的第几个 UTF-16 位置。打字机模式靠它决定哪一段是"现在"。 */
const caret = ref(0)
/** 朗读出来的那段音频。 */
const audio = ref(null)
/**
 * 哪几章手里那份和落库那份不一样。setStory 只刷新不在这里面的章。
 * 存完一章、且存的时候没再敲字，才从这里拿掉。
 */
const dirtySnapshot = new Set()
/** 最后一次敲字是什么时候。批量写作"跟着翻"要看它：正在打字就别翻走。 */
let lastTyped = 0

// ---- 版面 ----
function remembered(key, fallback) {
  const v = readLocal(key)
  return v === null ? fallback : v === '1'
}
function remember(key, v) {
  writeLocal(key, v ? '1' : '0')
}
/** 左栏章节列表开没开。记在这台机器上。 */
const listOpen = ref(remembered('changji.story.list', true))
watch(listOpen, (v) => remember('changji.story.list', v))
/** 右栏对话开没开。默认收起：大多数时候是在读和写。 */
const panelOpen = ref(remembered('changji.story.panel', false))
watch(panelOpen, (v) => remember('changji.story.panel', v))
/** 章头下面那行大纲折没折。 */
const summaryOpen = ref(remembered('changji.story.summary', true))
watch(summaryOpen, (v) => remember('changji.story.summary', v))
/** 章头右边那个「…」菜单。 */
const menuOpen = ref(false)
/** 左栏顶上「这本书」点开：正文位置换成梗概、体量、重出大纲。 */
const bookOpen = ref(false)
/** 批量写作时编辑器跟不跟着翻到正在写的那一章。点了别的章就不跟了。 */
const follow = ref(true)
/** 窄屏：左栏让位，章节切换退化成章头上的下拉框；右栏变成盖上来的抽屉。 */
const narrowQuery = window.matchMedia('(max-width: 1100px)')
const narrow = ref(narrowQuery.matches)
const onNarrow = (e) => {
  narrow.value = e.matches
}
const listShown = computed(() => listOpen.value && !narrow.value && !ui.focusMode)
/** 正文那一格（滚动容器）。 */
const scroller = ref(null)
/** 右栏那个输入框。 */
const askBox = ref(null)
/** 镜像层。行号靠它量每一行折成了几行。 */
const mirror = ref(null)
/**
 * 每一个逻辑行（按 \n 分）在版面上占的高度。行号列照它排。
 *
 * **行号标的是逻辑行，不是折行。** 一段话折成三行只有一个号，和代码编辑器
 * 一样。textarea 说不出一行折成了几行，所以在它后面垫的那层镜像改成一行
 * 一块、常驻，量每块的 offsetHeight 就是答案。
 */
const lineHeights = ref([])
/** 镜像里空行放的那个零宽空格：空块没有高度，行号就会少一格。 */
const ZW = String.fromCharCode(0x200b)

const chapters = computed(() => story.value?.chapters ?? [])
const hasStory = computed(() => chapters.value.length > 0)
const premiseDirty = computed(() => premise.value.trim() !== savedPremise.value)
const writtenCount = computed(
  () => chapters.value.filter((c) => (c.text ?? '').trim()).length,
)
const unwritten = computed(() => chapters.value.length - writtenCount.value)
const totalChars = computed(() =>
  chapters.value.reduce(
    (n, c) => n + [...(buf[c.chapter_id] ?? c.text ?? '')].length,
    0,
  ),
)
/** 还没存回去的章。**页面走开之前要拦一下**，不然改的字就没了。 */
/**
 * 手里这几章和服务端那份对不上——也就是"走开会丢的"。只有 beforeUnload 用它。
 *
 * **AI 正在往里写的那一章不算。** 那一章的 buf 和 c.text 确实对不上（字是
 * 一个个流进来的，落库要等这一章写完），但它不是人改的，也不该由这一页来
 * 存——`saveChapter` 开头就明写着"AI 正往这一章写：写完那份由引擎落库，
 * 这里存的是半截"，直接 return。
 *
 * 不排掉的话，批量展开那一个多小时里刷新一下，浏览器就弹一句"你的改动可能
 * 不会被保存"——而那时候什么都不会丢（引擎每写完一章就落库，重开页面还能
 * 接着看）。拦一件根本不会丢的事，只会让人以后连真该拦的那次也照样点掉。
 */
const dirtyIds = computed(() =>
  chapters.value
    .filter((c) => c.chapter_id !== streaming.value?.chapter_id)
    .filter((c) => (buf[c.chapter_id] ?? '') !== (c.text ?? ''))
    .map((c) => c.chapter_id),
)
/** 有正文但一个人物都没提出来——粘进来的故事就是这样。 */
const needsAnalysis = computed(
  () =>
    hasStory.value &&
    !(story.value?.characters ?? []).length &&
    writtenCount.value > 0,
)
/** 老项目：有剧集、没故事。给它一条接回新流程的路。 */
const canReverse = computed(() => !hasStory.value && session.episodes.length > 0)

const chapter = computed(
  () => chapters.value.find((c) => c.chapter_id === current.value) ?? null,
)
const index = computed(() => chapters.value.findIndex((c) => c.chapter_id === current.value))
/** 当前这一章在编辑器里的那一份。 */
const body = computed(() => buf[current.value] ?? '')
const chars = computed(() => [...body.value].length)
const currentDirty = computed(
  () => chapter.value != null && body.value !== (chapter.value.text ?? ''),
)
/** 光标在第几行（逻辑行）。状态栏上那个数。 */
const caretLine = computed(() => body.value.slice(0, caret.value).split('\n').length)
/** AI 正往这一章写字。这时候输入框只读，改稿那条也不接活。 */
/**
 * 这一章此刻能不能编辑。AI 正在往里写的那一章要锁上——两个人同时往
 * 一份文本里写，光标和 to_char 都会对不上（见 saveChapter 那段账）。
 *
 * **锁要跟着写它的那件活走，活没了就得解锁。** 用户 2026-09-12 报
 * 「AI 写作的时候我无法编辑」：批量那条的锁原来只在 `writer.running`
 * 真→假的那一刻清（下面那个 watch），而那个跳变有好几条路走不到——
 * 刷新之后没人轮询时不会发生（同一天的另一个坑），任务报错或被取消时
 * 也可能错过。一旦错过，`streaming` 就永远挂在那儿，那一章**再也编不了**，
 * 只能换个项目或者重开页面。
 *
 * 所以这里按来源兜底：批量那条额外要求批量任务还在跑。写一章、改一段
 * 那两条不走 writer.running（它们有自己的 socket 和 finally），
 * 所以不能一刀切加这个条件，否则那两条的锁会当场失效。
 */
const locked = computed(() => {
  const s = streaming.value
  if (!s || s.chapter_id !== current.value) return false
  if (s.src === 'batch' && !writer.running) return false
  return true
})
/** 选中的那一段，且是这一章的。不是这一章的选区套上来会改错地方。 */
const target = computed(() => (sel.value?.chapter_id === current.value ? sel.value : null))
/** 左栏列的是哪一份：有草稿时先把草稿的章节灰着列出来。 */
const listChapters = computed(() =>
  draft.value ? (draft.value.story?.chapters ?? []) : chapters.value,
)

const saveState = computed(() => {
  if (locked.value) return 'writing'
  if (saver.isBusy('save:' + current.value)) return 'saving'
  if (currentDirty.value) return 'dirty'
  return 'saved'
})
const SAVE_LABEL = {
  writing: 'AI 写着…',
  saving: '存着…',
  dirty: '改了，马上存',
  saved: '已存',
}

/** 「第 3 章 · 雨中告别」。标题本身已经是「第三章」的话就不再套一层。 */
function chapterLabel(c, i) {
  const t = (c?.title ?? '').trim()
  if (/^第.+章/.test(t)) return t
  return `第 ${i + 1} 章${t ? ' · ' + t : ''}`
}
function countOf(id) {
  return [...(buf[id] ?? '')].length
}
function isDirty(id) {
  const c = chapters.value.find((x) => x.chapter_id === id)
  return c != null && (buf[id] ?? '') !== (c.text ?? '')
}
function failedText(id) {
  return writer.state?.episodes?.find((w) => w.chapter_id === id && w.error)?.error ?? ''
}
/** 左栏每一章后面那个小字：写着 / 砸了 / 改了 / 字数 / 还没写。按手里这份算。 */
function stateText(id) {
  if (streaming.value?.chapter_id === id) return '写着…'
  if (failedText(id)) return '砸了'
  if (isDirty(id)) return '●'
  const n = countOf(id)
  // 没写的不画「—」：九行里八个破折号等于没说。整行压暗（见 .ch.is-blank），
  // 写了的才显示字数，哪几章有字一眼分出来。
  return n ? String(n) : ''
}
/** 一个字都没有、也没在写、也没脏——左栏整行压暗。 */
function isBlank(id) {
  return !countOf(id) && !isDirty(id) && streaming.value?.chapter_id !== id && !failedText(id)
}
function stateClass(id) {
  if (streaming.value?.chapter_id === id) return 'is-live'
  if (failedText(id)) return 'is-bad'
  if (isDirty(id)) return 'is-dirty'
  return ''
}

/**
 * 把一份故事装进这一页。
 *
 * `forProject` 是"这一份是哪部剧的"。**给了就认**：那几条改完故事回一份
 * 新全文的接口（删章、加章、存梗概、直接开写、反推、采用）都是几百毫秒的
 * 来回，而这几百毫秒里在项目库点一下别的剧，回来这一装就是**上一部的整本
 * 书装进了新这一部的编辑器**——接着敲字，自动存按当前项目写回去，两部剧
 * 里都有 ch01。和这一页上那几条长活（写正文、提人物）防的是同一件事，只是
 * 窗口短得多。
 *
 * 不给就是老样子（load 和自动存那两条自己判过了）。
 */
function setStory(payload, forProject) {
  if (forProject !== undefined && forProject !== session.projectPath) return
  story.value = payload?.story ?? null
  premise.value = story.value?.premise ?? ''
  savedPremise.value = premise.value.trim()
  // **还没采用的那份大纲从服务端恢复。**
  //
  // AI 出一份大纲要三四十秒到一分多钟，而它原来只活在 draft 这个 ref 里
  // ——刷新一下、切个页面、换台机器看，那一分钟就白花了，界面上连刚才
  // 写了什么都不剩。用户 2026-09-13 报的就是这个。
  //
  // **只在自己手里没有时才认服务端那份**：正在看的那份草稿可能刚被
  // 「重出一份」顶掉，而这一次 setStory 是别的事情触发的刷新（存梗概、
  // 改体量都会走到这儿），拿旧的盖上去等于把新写的那份顶没了。
  if (!draft.value && payload?.draft) draft.value = payload.draft
  // **后台还在写一份大纲的话，重新接上那条流。**
  // 用户点下去 8 秒就刷新了：那时草稿还没落盘，这一页要是不知道后台有活
  // 在跑，就一片空白——而写完之后的结果也没人收。
  if (payload?.outline_running) attachOutline(payload.outline_running)
  // 上一轮写砸了（多半是在刷新之后砸的，那句 job_error 没人听见）。
  // 服务端一直带着这句直到下一轮或者草稿被采用/丢弃；这里记着上次弹过
  // 哪句，同一句不弹第二次——不然每次存个梗概都再弹一遍。
  if (payload?.outline_error && payload.outline_error !== shownOutlineError) {
    shownOutlineError = payload.outline_error
    ui.error('上一份大纲没写成：' + payload.outline_error)
  }
  if (story.value?.scale) scale.value = story.value.scale
  // **只刷新没改过的那几章。** 引擎重算分集表也会回一份完整故事，照单
  // 全收的话，用户正在打字的那一章会被服务端那份盖掉。
  //
  // 脏的那几章里，手里那份已经和服务端一样的才摘掉——不能整个清空：
  // 存的那一会儿又敲了字的章仍然是脏的，下一次刷新还得护着它。
  for (const c of chapters.value) {
    const id = c.chapter_id
    // **AI 正往这一章写：一个字都别动。** 手里那份是正在长出来的，
    // 而服务端那份要等它写完才落库——盖上去就是"写着写着整章空了"。
    // 批量跑着的时候中途重读（见 refreshStory）就会撞上这一条。
    if (streaming.value?.chapter_id === id) continue
    if (!dirtySnapshot.has(id)) buf[id] = c.text ?? ''
    else if ((buf[id] ?? '') === (c.text ?? '')) dirtySnapshot.delete(id)
  }
  // 进来先挑一章。**优先挑还没写正文的第一章**：那一章是接下来要干的活，
  // 而已经写好的几章翻一下就能看到。
  if (!chapters.value.some((c) => c.chapter_id === current.value)) {
    const todo = chapters.value.find((c) => !(c.text ?? '').trim())
    current.value = (todo ?? chapters.value[0])?.chapter_id ?? ''
  }
  nextTick(fitAll)
}

/**
 * 只把服务端那份重读一遍，**不清编辑器里的缓冲**。
 *
 * 和 load() 的区别就在这儿：load 会把 buf 和 dirtySnapshot 整个清掉，
 * 那在批量跑着的时候是灾难——正在长出来的那一章会当场空掉，别的章没存的
 * 改动也没了。这个只更新"服务端那份"（c.text），编辑器手里那份由
 * setStory 按脏不脏、流没流决定要不要跟。
 *
 * **读不到就算了。** 它是顺手刷新，不是关键路径；批量跑着的时候引擎正忙，
 * 为这个弹个红框只会让人以为批量挂了。
 */
/**
 * 删当前这一章。引擎顺手把分集表改对（跨着这一章的条目收缩、只在它
 * 里面的丢掉），回包里说改了几条——改过的话落成剧集要重跑，得说出来。
 */
async function deleteChapter() {
  const c = chapter.value
  if (!c) return
  const n = countOf(c.chapter_id)
  const label = chapterLabel(c, index.value)
  if (!confirm(`删掉「${label}」${n ? `（${n} 字）` : ''}，没有撤销。确定？`)) return
  // 手里没存的先放掉：删都删了，再存回去等于复活。
  // **`.t` 不能漏**：timers 里放的是 { t, project }，把整个对象交给
  // clearTimeout 是静默无效的，那次存照样会在 1.5 秒后把这一章写回去。
  clearTimeout(timers[c.chapter_id]?.t)
  delete timers[c.chapter_id]
  const project = session.projectPath
  const result = await run(
    () => api.deleteChapter({ project, chapter_id: c.chapter_id }),
    // 反方向的同一件事：删掉最后一章之后 done.story 该灭，不重拉的话
    // 导航上会给一部已经没有故事的剧一直打着勾。
    { key: 'delch', refresh: true },
  )
  if (!result) return
  const i = index.value
  delete buf[c.chapter_id]
  dirtySnapshot.delete(c.chapter_id)
  setStory(result, project)
  const rest = chapters.value
  current.value = rest.length ? rest[Math.min(i, rest.length - 1)].chapter_id : ''
  const touched = (result.plan_dropped ?? 0) + (result.plan_moved ?? 0)
  ui.ok(touched ? `删了。分集表改了 ${touched} 处，落成剧集要重跑` : '删了')
}

async function refreshStory() {
  if (!session.projectPath) return
  try {
    setStory(await api.getStory(session.projectPath))
  } catch {
    /* 下一次换章再说 */
  }
}

async function load() {
  if (!session.projectPath) {
    story.value = null
    return
  }
  // 换项目那一下会连着起两趟，回来的顺序不保证。慢的那一趟后落地就是
  // **上一部剧的正文装进了这一部的编辑器**——而接着敲字触发的自动存用的是
  // 当前这部剧的路径，等于把上一部的章节内容写进这一部。
  const want = session.projectPath
  loading.value = true
  try {
    for (const k of Object.keys(buf)) delete buf[k]
    dirtySnapshot.clear()
    const data = await api.getStory(want)
    if (want !== session.projectPath) return
    setStory(data)
    loadedFor = want
    loadError.value = ''
  } catch (err) {
    if (want !== session.projectPath) return
    // **手里这份要是上一部剧的，就得倒掉。**
    //
    // 这儿原来只弹一句错，`story` 一个字不动。而这一趟在换剧那条路上跑，
    // 砸了的话新这一部的编辑器里**摆着上一部的正文**——接着敲字，自动存
    // 用的是当前这部剧的路径加上那一章的章号，两部剧里都有 ch01，于是
    // 上一部的整章文字落进了这一部。这一页为这件事清过 draft / sel /
    // chat / streaming / pending（见换剧那个 watch），唯独 story 本身没清。
    //
    // 同一部剧自己重读失败（批量写完那条也叫 load）不倒——那时候屏幕上
    // 的就是这一部自己的字，为一次抖动清空整本书更糟。
    if (loadedFor !== session.projectPath) {
      story.value = null
      loadedFor = null
      loadError.value = err.message
    }
    ui.error(err.message)
  } finally {
    if (want === session.projectPath) loading.value = false
  }
}

function beforeUnload(e) {
  if (!dirtyIds.value.length) return
  e.preventDefault()
  e.returnValue = ''
}

/**
 * 批量展开正文时，字也一个个长进来。
 *
 * 十六章要跑一个多小时，进度条上只有"正在写 ch07"的话，那一个多小时里
 * 看不到一个字。引擎按**任务类别**广播（具体 job_id 从来不从接口暴露
 * 出去），所以这里订的是 "write"。
 *
 * 常驻一条连接是刻意的：批量可能是上一次离开页面前起的，等 writer.running
 * 变真再连的话，头几章的字已经过去了。
 */
let batchSock = null
let batchRetry = null
function watchBatch() {
  if (batchSock) return
  clearTimeout(batchRetry)
  batchRetry = null
  batchSock = openJobSocket(
    'write',
    async (msg) => {
      if (msg.type !== 'story_token' || !msg.chapter_id) return
      // **这一条是哪部剧的。**
      //
      // 这条订的是 "write" 那个**全局槽**（具体 job_id 不从接口暴露，只能
      // 按类订），而批量一跑就是一个多小时。人中途去项目库点另一部剧的话，
      // 收到的还是上一部的正文——章号是 ch01 这种、两部剧都有，光靠它分不
      // 出来，于是上一部的字一路长进这一部的编辑器，接着 current 还会跟着
      // 跳章。引擎 2026-09-15 起在这条消息里带上了 project（batch.cpp 那条
      // broadcast），比一下就知道该不该收。
      //
      // 老引擎不带这个字段：那时 `msg.project` 是 undefined，照旧全收，
      // 行为和以前一样。
      if (msg.project && msg.project !== session.projectPath) return
      // **落到它自己那一章上。** 批量是一章一章顺着写的，但消息里带着
      // chapter_id，不靠顺序猜——猜错的话字会长进隔壁那一章。
      //
      // **seq 归零就是重头来。** 批量那条写砸了会自动再要一次（实跑里
      // 第一次就用上了），而重试是从头生成的——照旧往后接的话，编辑器里
      // 会是"写砸的那半截 + 重写的全文"接在一起。跑完 load() 会把它冲掉，
      // 但那之前这一章看着就是坏的。
      // **AI 换章了：把服务端那份重读一遍。**
      //
      // 上一章这会儿刚落库，而这一页手里那份还是空的。不重读的话：
      //   - 左边那栏上一章一直显示「—」，而它明明写完了
      //   - 那一章在 dirtyIds 里挂着（buf 有字、c.text 是空）
      //   - **用户在那一章里敲一个字，存的时候就报「选中的范围不对」**
      //     ——saveChapter 拿 c.text 的长度当 to_char，而那是 0，
      //     引擎那边已经有六百字了。整段的账见 saveChapter。
      //   - 批量跑着的时候翻回上一章，编辑器里是空的，刷新页面才出来
      //     （token 是往 buf 里灌的，没听见的那几章就没有）
      //
      // 十六章一个多小时，中间十五次重读，一次就是一个 GET。
      const wrote = streaming.value?.chapter_id
      if (wrote && wrote !== msg.chapter_id) refreshStory()

      buf[msg.chapter_id] =
        msg.seq === 0 ? (msg.text ?? '') : (buf[msg.chapter_id] ?? '') + (msg.text ?? '')
      // at：AI 写到哪个字了。批量是从头往下写，所以就是当前长度。
      streaming.value = {
        chapter_id: msg.chapter_id,
        from: 0,
        at: buf[msg.chapter_id].length,
        // 标上来源：这把锁是批量那件活上的，活停了就该解开。见 locked。
        src: 'batch',
      }
      // **跟着它翻页。** 一次只看一章，不跟的话批量跑一个多小时，眼前
      // 这一章一个字都不动——"看着它写"就落空了。
      //
      // 两种情况不翻：人关了「跟着翻」（点了别的章就等于关了），或者
      // 五秒内还在敲字——正打到一半把人翻走，比看不到 AI 写字糟得多。
      if (
        current.value !== msg.chapter_id &&
        follow.value &&
        Date.now() - lastTyped > 5000
      ) {
        current.value = msg.chapter_id
      }
      await nextTick()
      fit(boxes[msg.chapter_id])
      keepEndVisible(msg.chapter_id)
    },
    () => {
      // **断了要接回来。** 这条是常驻的（批量可能是上一次离开页面前起的），
      // 而它原来断了就 null 掉、没人再开——引擎中途重启一下，正文就再也不
      // 长了，而底栏的进度、顶栏的角标各自重连之后照常走：屏幕上一半活着
      // 一半死着。别的几条流（顶栏负载表、作业角标、参考图）都是 5 秒重连，
      // 这条跟上。
      batchSock = null
      clearTimeout(batchRetry)
      batchRetry = setTimeout(watchBatch, 5000)
    },
  )
}

/** Ctrl+K：打开对话，光标进输入框。代码编辑器的肌肉记忆。 */
function onKey(e) {
  if ((e.ctrlKey || e.metaKey) && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 'k') {
    e.preventDefault()
    openDialog()
  }
}

/** 版面一变（开关左右栏、拉窗口）就把输入框高度和行高重量一次。 */
let sizer = null
watch(scroller, (el) => {
  sizer?.disconnect()
  sizer = null
  if (!el) return
  sizer = new ResizeObserver(() => {
    fitAll()
    measureLines()
  })
  sizer.observe(el)
})

/** 量镜像里每一行的高度。flush: 'post'——要等镜像按新正文排完再量。 */
function measureLines() {
  const m = mirror.value
  if (!m) {
    lineHeights.value = []
    return
  }
  // **要小数。** 一行是 28.8px，offsetHeight 取整成 29，十四行下来行号就
  // 比正文低了 3px，越往下越歪。
  lineHeights.value = [...m.children].map((el) => el.getBoundingClientRect().height)
}

/** 点行号：光标落到那一行开头。 */
function goLine(i) {
  const el = boxes[current.value]
  if (!el) return
  const lines = body.value.split('\n')
  let pos = 0
  for (let k = 0; k < i && k < lines.length; k++) pos += lines[k].length + 1
  el.focus()
  el.setSelectionRange(pos, pos)
  caret.value = pos
}

onMounted(async () => {
  load()
  // **上次离开页面时可能还在跑，那就得把轮询接着开起来。**
  //
  // 原来这儿只 poll 一次，拿到一帧就不管了。writer.start()（轮询 + socket）
  // 只在点「展开」那一下调过，所以**刷新之后没有任何人在轮询**，后果有两个，
  // 用户 2026-09-12 两个都报了：
  //   · 顶栏和侧栏那个「展开中 0/4」冻在刷新那一帧，永远不动；
  //   · AI 写完之后正文也不出现——下面那个 watch 等的是 writer.running
  //     从真变假，而没人轮询的话它根本不会变。
  await writer.poll()
  if (writer.running) writer.start()
  watchBatch()
  window.addEventListener('beforeunload', beforeUnload)
  window.addEventListener('keydown', onKey)
  narrowQuery.addEventListener('change', onNarrow)
})
onUnmounted(() => {
  writer.stop()
  batchSock?.close()
  batchSock = null
  clearTimeout(batchRetry)
  batchRetry = null
  sizer?.disconnect()
  // ⚠️ **这儿原来是 clearTimeout，那等于把刚敲的字丢掉。**
  //
  // `beforeunload` 只管关标签页和刷新，**管不到站内换页**——在故事页敲两个
  // 字、1.5 秒内点顶栏的「设定」，这一页就卸了，排着的那次存被取消，
  // 那几个字再也找不回来，而且一声不吭。文件开头写着"页面走开之前要拦
  // 一下，不然改的字就没了"，拦的只有浏览器那一半。
  //
  // 冲出去是安全的：请求已经发出，闭包还活着；组件没了只是没人去画结果，
  // 而 saveChapter 里那道 project 判断会把动界面那几句跳掉。
  flushAll()
  window.removeEventListener('beforeunload', beforeUnload)
  window.removeEventListener('keydown', onKey)
  narrowQuery.removeEventListener('change', onNarrow)
})
watch(() => session.projectPath, () => {
  // **先冲再读。** load() 会把 buf 和 dirtySnapshot 整个清掉；不先冲的话，
  // 在故事页敲两个字、1.5 秒内在项目库里点了另一部剧，那几个字就没了。
  // 每一次排队都带着自己那部剧的路径，所以冲出去落的是**原来那一部**。
  flushAll()
  // 上一部剧那条大纲流也脱钩：留着它没有意义（写完的结果不能装进这一部），
  // 而新这一部要是也有活在跑，下面 load() → setStory 会自己接上。
  detachOutline()
  // **这几样都是"上一部剧的"，得跟着走。** load() 只清 buf 和
  // dirtySnapshot，下面这些原来一直留着，而它们都认章号——两部剧里都有
  // ch01，于是全落在新这一部头上：
  //
  //   · draft：setStory 只在自己手里没有时才认服务端那份
  //     （`if (!draft.value && payload?.draft)`），所以上一部的草稿会一直
  //     挂着——**在新这一部按一下「采用」就整份写进去了**。
  //   · sel / chat：改稿的选区是字符偏移，套到另一部剧的同名章上就是改错
  //     地方；旁边那串对话也还是上一部的。
  //   · streaming / pending：上一部那章的"正在写"锁和撤销底稿。锁尤其难受
  //     ——批量还在上一部跑着（writer.running 是全局的一个槽），新这一部的
  //     同名章会被锁成不能编辑，而这一部根本没人在写它。
  //   · ideas：那三个点子是照**上一部**的梗概和各集简介避重想出来的（见
  //     suggestIdeas）。两部剧都还没有故事时，「从这儿开始」那一屏长得
  //     一模一样，三张卡就那么留在新这一部下面——点一张，pickIdea 直接
  //     `savePremise()`，上一部的选题当场存成了这一部的梗概。
  //   · audio：念出来那段音频落在上一部的目录里（URL 里钉着它的路径），
  //     换了剧还挂在状态条上，按播放放的是上一部的声音。
  draft.value = null
  sel.value = null
  chat.value = []
  streaming.value = null
  pending.value = null
  ideas.value = []
  audio.value = null
  load()
})
watch(
  () => writer.running,
  (now, before) => {
    if (before && !now) {
      // 流出来的是原始 token，落库那份解析过、过了守卫、算过钩子。
      // 整个重读一遍，以它为准。
      streaming.value = null
      load()
    }
  },
)

// ---------------------------------------------------------------------------
// 编辑器本身
// ---------------------------------------------------------------------------

/**
 * 高度跟着内容长。这一格里不该有第二根滚动条。
 *
 * **还没排出来就不量。** 宽度为 0 的时候量出来的 scrollHeight 是废数，
 * 而且会一直留着——上一版一章 343 字被算成 8803px 就是这么来的。
 */
function fit(el) {
  if (!el || !el.clientWidth) return
  el.style.height = 'auto'
  el.style.height = el.scrollHeight + 'px'
}
function fitAll() {
  for (const el of Object.values(boxes)) fit(el)
}

/**
 * 跟不跟着 AI 写的那一头走。
 *
 * **用户往上滚就松手，滚回底部就重新贴上。** 原来是每来一个 token 就
 * 无条件 `scrollTop = scrollHeight`——想往回看一眼刚写的那几段，手一松
 * 就被拽回底部，一个多小时的批量里翻不了任何东西。
 *
 * 判据是"离底部还有多远"，留 40 像素的余量：滚动位置在缩放、亚像素和
 * 输入框重排之后常常差那么一两个像素，卡死成 `=== 0` 的话，明明在底部
 * 却再也贴不回去了。
 */
const stuck = ref(true)
const kStickSlack = 40

function onScroll() {
  const el = scroller.value
  if (!el) return
  stuck.value = el.scrollHeight - el.scrollTop - el.clientHeight <= kStickSlack
}

/** AI 往眼前这一章写字时，把正在长的那一头留在视野里——**除非人滚走了**。 */
function keepEndVisible(id) {
  if (id !== current.value || !scroller.value) return
  if (!stuck.value) return
  scroller.value.scrollTop = scroller.value.scrollHeight
}

function onInput(id, event) {
  buf[id] = event.target.value
  caret.value = event.target.selectionStart
  dirtySnapshot.add(id)
  lastTyped = Date.now()
  fit(event.target)
  // 手一动就说明这一版是自己的了，AI 那条"撤销"没有意义了
  pending.value = null
  scheduleSave(id)
}

/** 输入框里的快捷键：Ctrl+S 立刻存（无声），Ctrl+Z 在稿子还是 AI 刚写的那份时撤销改写。 */
function onAreaKey(e) {
  if (!(e.ctrlKey || e.metaKey)) return
  const k = e.key.toLowerCase()
  if (k === 's') {
    e.preventDefault()
    flushSave(current.value)
  } else if (
    k === 'z' &&
    !e.shiftKey &&
    pending.value &&
    pending.value.chapter_id === current.value &&
    buf[current.value] === pending.value.after
  ) {
    e.preventDefault()
    undoRevision()
  }
}

/**
 * 点在稿子外面的空白处，光标也落到正文末尾。**整块都是纸。**
 *
 * 用户 2026-09-11：「点故事页面的下面光标为什么消失了，不是应该整个页面
 * 都是编辑区域吗」。短章的输入框只有半屏高，再往下是滚动容器的留白，点上去
 * 输入框就失焦。现在输入框本身撑到底（见 .doc--chapter），剩下的边角靠这个：
 * mousedown 拦住不让它失焦，再把光标放到末尾。按钮、下拉、章头那一行照旧。
 */
function onPaperDown(e) {
  const el = boxes[current.value]
  if (!el || !chapter.value || bookOpen.value || draft.value) return
  const t = e.target
  if (t === el) return
  // 章头那一行的空当也算纸；按钮、下拉、大纲那句（点它是折叠）、行号列不算
  if (t.closest('button, select, textarea, input, a, .doc__sum, .ed__mini, .ed__gutter')) return
  // 点的是滚动条本身
  if (scroller.value && e.offsetX >= scroller.value.clientWidth && t === scroller.value) return
  e.preventDefault()
  el.focus()
  const end = el.value.length
  el.setSelectionRange(end, end)
  caret.value = end
}

/** 浏览器给的 UTF-16 偏移换算成**码点**偏移。 */
function codePoints(text, utf16Offset) {
  return [...text.slice(0, utf16Offset)].length
}
/**
 * 把正文切成"行"，标出光标在哪一段。行号列和打字机模式的高亮都用它。
 *
 * **一行一块，空行断段。** 中文小说就是这么排的：段之间空一行。按句切的话
 * 高亮会在一段里跳来跳去，比不高亮还乱。
 *
 * 镜像里每一行是一个 block，块的折行和 textarea 里那一行的折行一样——
 * 字体、字号、行高、宽度、white-space、overflow-wrap 全部相同（见样式里
 * 那组共用声明）。差一项两边就错位，而错位之后行号和光标都对不上。
 */
function paragraphs(text, at) {
  const lines = text.split('\n')
  // 每一行在原文里的起止（含行尾那个换行符）
  const spans = []
  let pos = 0
  for (let i = 0; i < lines.length; i++) {
    const withNl = i < lines.length - 1 ? lines[i] + '\n' : lines[i]
    spans.push({ text: lines[i], from: pos, to: pos + withNl.length, blank: !lines[i].trim() })
    pos += withNl.length
  }
  // 光标在哪一行
  let hit = spans.findIndex((s) => at >= s.from && at < s.to)
  if (hit < 0) hit = spans.length - 1
  // 往上下各扩到空行为止，那一整块就是"现在这一段"
  let lo = hit
  let hi = hit
  if (!spans[hit]?.blank) {
    while (lo > 0 && !spans[lo - 1].blank) lo--
    while (hi < spans.length - 1 && !spans[hi + 1].blank) hi++
  }
  // from 带出去：AI 光标要靠它算出"落在哪一行的第几个字"
  return spans.map((s, i) => ({ text: s.text, from: s.from, now: i >= lo && i <= hi }))
}

const blocks = computed(() => paragraphs(body.value, caret.value))

/**
 * AI 正在写到哪个字。**和用户自己的光标是两回事**：
 *
 * 用户的光标是原生 caret，AI 写着的时候那一章是只读的、caret 被藏起来了；
 * 这个是画在镜像层上的另一个记号，位置由流出来的字数决定，跟用户上次点在
 * 哪儿没有关系。两个同时存在过（改一段时用户的光标还停在选区上），所以
 * 颜色和形状都得不一样，不能只画一根一样的竖线。
 *
 * 不是这一章就返回 null——别的章在写字，不该在眼前这一章上画个光标。
 */
const aiAt = computed(() =>
  streaming.value?.chapter_id === current.value &&
  typeof streaming.value.at === 'number'
    ? streaming.value.at
    : null,
)
/** AI 光标落在第几行、那一行的第几个字。找不到就是 null。 */
const aiSpot = computed(() => {
  const at = aiAt.value
  if (at === null) return null
  const bs = blocks.value
  for (let i = 0; i < bs.length; i++) {
    const end = bs[i].from + bs[i].text.length
    if (at <= end) return { line: i, col: Math.max(0, at - bs[i].from) }
  }
  return bs.length ? { line: bs.length - 1, col: bs[bs.length - 1].text.length } : null
})
// 正文一变，等镜像排完再量行高（flush: 'post'）
watch(blocks, measureLines, { flush: 'post' })

/** 码点偏移换回 UTF-16。插完字要用它把光标放回正确的位置。 */
function utf16At(text, cp) {
  return [...text].slice(0, cp).join('').length
}

function onSelectionChange(id, event) {
  const el = event.target
  const full = el.value ?? ''
  caret.value = el.selectionStart
  const from = codePoints(full, el.selectionStart)
  const to = codePoints(full, el.selectionEnd)
  liveSel.value = to - from >= 2
  if (to - from < 2) {
    // 光标只是移动了一下。**不清掉已经选好的那一段**——右栏还对着它说话。
    return
  }
  if (sel.value?.chapter_id === id && sel.value.from === from && sel.value.to === to) {
    return
  }
  sel.value = { chapter_id: id, from, to, text: [...full].slice(from, to).join('') }
  // 换了一段就从头聊：上一段的来回套在这一段上只会让它改错方向
  chat.value = []
  pending.value = null
}

/** 换一章就把改稿那摊清掉：偏移是按章算的，套到别的章上会改错地方。 */
function clearSelection() {
  sel.value = null
  liveSel.value = false
  chat.value = []
  instruction.value = ''
  pending.value = null
  audio.value = null
}
watch(current, (now, before) => {
  clearSelection()
  menuOpen.value = false
  // 走之前把上一章存了。自动存有 1.5 秒的等待，换章不该等。
  if (before) flushSave(before)
  nextTick(() => {
    fit(boxes[now])
    if (scroller.value) scroller.value.scrollTop = 0
    // 换了一章就是另一件事了：上一章滚到哪儿、跟没跟，都不带过来。
    // **要在滚到顶之后设**，不然那次 scrollTop = 0 触发的 onScroll
    // 会立刻把它判成"不在底部"。
    nextTick(() => {
      stuck.value = true
    })
  })
})

function pickChapter(id) {
  // 批量写作时人主动点了一章，就是"我要看这一章"，别再把人翻走
  if (writer.running) follow.value = false
  current.value = id
}
function toggleFollow() {
  follow.value = !follow.value
  if (follow.value && streaming.value?.chapter_id) current.value = streaming.value.chapter_id
}

// ---- 自动存 ----
//
// **每一次排队都记住这几个字属于哪部剧。** 存的时候只认 `session.projectPath`
// 的话，排队那 1.5 秒里换了项目，这一存就写进**新打开的那部剧**里去了——
// 换项目走的是 watch，组件不卸载，定时器原样留着。
const timers = {} // chapter_id -> { t, project }
/**
 * @param {string} [text] 要存的那份字。**只有"上一次还没存完"那条分支会传**
 *   ——见 saveChapter 里那段。平时不传：正常排队要的是到点那一刻 buf 里
 *   最新的字，而不是排队那一刻的。
 */
function scheduleSave(id, delay = 1500, project = session.projectPath, text) {
  clearTimeout(timers[id]?.t)
  timers[id] = { project, t: setTimeout(() => saveChapter(id, project, text), delay) }
}
/** 立刻存，不等那 1.5 秒。没排过队就按当前项目算（失焦那一下就是这样）。 */
function flushSave(id) {
  if (!id) return
  const pending = timers[id]
  clearTimeout(pending?.t)
  delete timers[id]
  return saveChapter(id, pending?.project)
}
/** 排着的全部冲出去。走开之前调——**不是 clearTimeout**，见 onUnmounted。 */
function flushAll() {
  for (const id of Object.keys(timers)) flushSave(id)
}

/**
 * 存这一章。整章当成一个选区，走的就是 AI 改稿那条写回路径。
 *
 * 存的那一会儿又敲了字的话，这一章仍算脏、再排一次；不然 setStory 会拿
 * 服务端那份（旧的）把刚敲的字盖掉。
 */
async function saveChapter(id, project = session.projectPath, text) {
  const c = chapters.value.find((x) => x.chapter_id === id)
  if (!c) return
  // `text` 只有下面那条"排在后面"的分支会带过来，理由见那儿。
  const sent = text ?? buf[id] ?? ''
  if (sent === (c.text ?? '')) return
  // AI 正往这一章写：写完那份由引擎落库，这里存的是半截
  if (streaming.value?.chapter_id === id) return
  // 引擎不收空章。清空了就先不存，敲下一个字再说
  if (!sent.trim()) return
  // 上一章还在存，排在后面
  if (saver.busy.value) {
    // 重排要带着两样东西：
    //
    //   · **原来那部剧** —— 这 400 毫秒里换了项目的话，不带就存错地方；
    //   · **手里这份字** —— 换项目那一下 `load()` 会同步把 buf 整个清空
    //     （它就在 flushAll 后面一行），400 毫秒后再读 `buf[id]` 读到的是
    //     空串，而空串这个函数开头就 return 了——那几个字**一声不吭地没了**。
    //     上面的 `sent` 是同步读到的，把它原样带过去。
    scheduleSave(id, 400, project, sent)
    return
  }
  // **存的是一个区间：[0, 服务端那份有多长)。** 所以"服务端那份有多长"
  // 必须是新的——旧了的话引擎会拒：「选中的范围不对：这一章有 601 个字，
  // 而选的是 [0, 0)」。
  //
  // 什么时候会旧：批量正一章章往下写，上一章刚落库而这一页还没重读
  // （见 watchBatch 里那段）。那一条现在在换章时就重读了，但**竞态还在**
  // ——引擎可能正好在这次存的前一刻写完这一章。所以这里再兜一层：
  // 第一次失败不弹框（quiet），重读一次拿到真正的长度，再存一次。
  const save = (len) =>
    api.applyRevision({
      project,
      chapter_id: id,
      from_char: 0,
      to_char: len,
      text: sent,
    })

  let result = await saver.run(() => save([...(c.text ?? '')].length),
                               { key: 'save:' + id, quiet: true })
  if (!result) {
    // **界面已经换走的话就别重读了。** refreshStory 拿回来的是**现在**那部剧
    // 的长度，照着它再存一次等于往新剧里写旧字。第一次没成就照实说一句。
    if (project !== session.projectPath) {
      ui.error(`「${c.title || id}」最后改的那几个字没存回去`)
      return
    }
    await refreshStory()
    const fresh = chapters.value.find((x) => x.chapter_id === id)
    result = await saver.run(() => save([...(fresh?.text ?? '')].length),
                             { key: 'save:' + id })
  }
  if (!result) return
  // 存进去了，但这一页已经在看别的剧（或者已经卸了）。下面那三句都是
  // 拿这次的结果去动界面，这时候动就是拿旧数据盖掉新打开的那部剧。
  if (project !== session.projectPath) return
  if ((buf[id] ?? '') !== sent) scheduleSave(id)
  else dirtySnapshot.delete(id)
  setStory(result)
}

// ---------------------------------------------------------------------------
// 让 AI 改：改完**直接落到编辑器里那一段的位置上**
// ---------------------------------------------------------------------------

function openDialog() {
  panelOpen.value = true
  liveSel.value = false
  nextTick(() => askBox.value?.focus())
}
function togglePanel() {
  if (panelOpen.value) panelOpen.value = false
  else openDialog()
}

/**
 * 让 AI 改。**选中了就只改那一段，没选就改整章。** 边生边写进编辑器。
 *
 * 字走 WebSocket 一个个推过来，请求本身照样在最后回完整的一份——那一份
 * 是剥过包装（``` 代码块、「修改后：」）的，所以收尾时拿它把流出来的那段
 * 覆盖一次，两边才一致。
 *
 * WebSocket 连不上就退回一次性返回：少了"看着它写"这件事，但功能还在。
 */
async function revise() {
  const want = instruction.value.trim()
  if (!chapter.value || locked.value) return
  if (!want) {
    ui.warn('说一句要改成什么样，比如「这儿太赶了，铺一下情绪」')
    return
  }
  const at = target.value
    ? { ...target.value }
    : { chapter_id: current.value, from: 0, to: chars.value, text: body.value }
  if (!at.text.trim()) {
    ui.warn('这一章还是空的。先写几句，或者点上面那个按钮让 AI 照大纲写')
    return
  }
  const id = at.chapter_id

  // 改之前那一章的整份，撤销和流式拼接都拿它当底
  const prev = buf[id] ?? ''
  const allChars = [...prev]
  const head = allChars.slice(0, at.from).join('')
  const tail = allChars.slice(at.to).join('')

  /**
   * **这一趟是替哪部剧写的。**
   *
   * 流式这三条路都要几十秒到一两分钟，而这期间人完全可能去项目库点另一部
   * 剧。换剧时 `load()` 只清了 buf 和 dirtySnapshot，**没人管在途的这条
   * 流**：它照样往 `buf[章号]` 上画，而章号是 ch01 这种、两部剧里都有，
   * 于是上一部的字长在这一部的编辑器里，还被标成"改过了"。收尾那下更狠
   * ——`scheduleSave` 默认绑的是**当前**项目，A 的稿子就存进 B 的同名章。
   *
   * 所以：画之前认一次，收尾之前再认一次。换走了就把这一份丢掉，不画不存。
   * 丢是对的：这一段只活在客户端（改稿那条接口不落库），而把它塞进另一部
   * 剧是实打实的破坏。
   */
  const owner = session.projectPath
  const mine = () => owner === session.projectPath

  const streamId = 'story-' + Math.random().toString(36).slice(2, 10)
  let acc = ''
  let sock = null
  let opened = false

  const paint = async (text) => {
    if (!mine()) return // 换剧了，别往新这一部的编辑器上画
    buf[id] = head + text + tail
    dirtySnapshot.add(id)
    await nextTick()
    fit(boxes[id])
  }

  const finish = () => {
    thinking.finish(streamId)
    sock?.close()
    sock = null
    streaming.value = null
  }

  // 那一头改完（或者改砸了）会从这条 socket 上说一声。等它，不等 HTTP。
  // 理由和写一章那条一样：请求占着引擎的 I/O 线程，一占就是十几秒，
  // 而落在同一条线程上的连接全都跟着干等。
  let settle = null
  const finished = new Promise((r) => {
    settle = r
  })

  const post = () =>
    api.reviseStory({
      // 用上面记下的 owner，不是现读：这一行发出去的时候 socket 那两下
      // 已经过去了，而 owner 才是这段字、这个章号、这个区间的主人。
      project: owner,
      chapter_id: id,
      from_char: at.from,
      to_char: at.to,
      instruction: want,
      history: chat.value,
      // 订阅没发出去就别开流：头几个字推出来时没人听，而漏掉的那几个字
      // 不会有任何提示，只是那段话缺了个开头。**也别走异步**：结果没地方
      // 送回来。
      ...(opened ? { stream: streamId, async: true } : {}),
    })

  // 先把选中那段清掉，字就从那个位置长出来——这一下就是"开始写了"
  streaming.value = { chapter_id: id, from: at.from }
  stuck.value = true   // 理由同 writeChapter
  pending.value = { chapter_id: id, prev, origin: at, after: null }
  await paint('')

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      (msg) => {
        if (msg.job_id !== streamId) return
        // 思考流。**这三步（写大纲 / 写正文 / 改一段）没走 runAsyncJob**，
        // 所以要在这儿自己接一下——而它们恰恰是思考最久的三步。
        if (msg.type === 'job_thinking') return thinking.push(streamId, msg.text ?? '')
        if (msg.type === 'story_token') {
          acc += msg.text ?? ''
          // 改一段是插在中间的，所以是选区起点加上已经流出来的长度
          streaming.value = { chapter_id: id, from: at.from, at: at.from + acc.length }
          paint(acc)
          return
        }
        // job_done / job_error 才是权威的那一份（story_done 是老名字，
        // 它带的是流完的那段字，不是整份结果）。
        if (msg.type === 'job_done') settle({ ok: true, result: msg.result })
        else if (msg.type === 'job_error') settle({ ok: false, message: msg.message })
      },
      () => {
        // 连接没了：把等的人放出来，否则这一段会永远显示"改着…"
        settle?.({ ok: false, message: '和引擎的连接断了，这一段改没改完不好说' })
        resolve()
      },
      () => {
        opened = true
        resolve()
      },
    )
    // 连不上也别卡着。两秒够本机的 WebSocket 握完手了。
    setTimeout(resolve, 2000)
  })
  // 顶栏那块「正在思考」。**开在这儿、清在 finally 里**——这三步都有
  // 好几条提前 return 的路，手动清总会漏一条，而漏掉的后果是顶栏
  // 永远显示在想。
  if (opened) thinking.start(streamId)

  let result = null
  try {
    const started = await run(post, { key: 'revise' })
    result = started
    if (started && started.started) {
      const fin = await finished
      result = fin.ok ? fin.result : null
      if (!fin.ok) ui.error(fin.message || '这一段没改成')
    }
  } finally {
    // 上面那句注释说的就是这一下。理由同 writeStory / writeChapter 那两处：
    // 它原来摆在直线上，不漏全靠 `run()` 把异常吞了。
    finish()
  }
  if (!mine()) {
    // 人已经在看别的剧了。这一段属于上一部，扔掉——留下只会写错地方。
    ui.warn('中途换了项目，刚才那一段改稿没有留下')
    return
  }
  if (!result) {
    // 改砸了，把清掉的那一段放回去
    buf[id] = prev
    pending.value = null
    await nextTick()
    fit(boxes[id])
    return
  }

  // **拿返回那一份收尾。** 流出来的是原始 token，返回那份剥过包装，
  // 两者可能差几个字符；不覆盖的话编辑器里会留下一行 ``` 。
  await paint(result.text)
  pending.value = { chapter_id: id, prev, origin: at, after: buf[id] }
  scheduleSave(id, 800)

  chat.value = [
    ...chat.value,
    { role: 'user', text: want },
    { role: 'assistant', text: result.note || '改完了' },
  ]
  instruction.value = ''

  // 选中刚写好那一段：接着说"再短一点"时，说的还是这一段
  const to = at.from + [...result.text].length
  sel.value = { chapter_id: id, from: at.from, to, text: result.text }
  await nextTick()
  const el = boxes[id]
  if (el) {
    fit(el)
    el.focus()
    const now = buf[id]
    el.setSelectionRange(utf16At(now, at.from), utf16At(now, to))
    liveSel.value = false
  }
}

/** 不要这一版。整章退回改之前——比按区间回退可靠，见 pending 上的注释。 */
function undoRevision() {
  const p = pending.value
  if (!p) return
  buf[p.chapter_id] = p.prev
  dirtySnapshot.add(p.chapter_id)
  sel.value = { ...p.origin }
  pending.value = null
  scheduleSave(p.chapter_id, 800)
  nextTick(() => fit(boxes[p.chapter_id]))
}

/**
 * 念给自己听。
 *
 * **念的是选中的那一段，没选就从光标往下念。** 整章念完要好几分钟，而人在
 * 编辑器里想听的通常是刚改过的那几句顺不顺口。引擎那边一次最多两百字
 * （模型一次合成上限约 41 秒），超了它会说。
 */
async function readAloud() {
  const el = boxes[current.value]
  const full = body.value
  if (!full.trim()) return
  let picked = ''
  if (el && el.selectionEnd > el.selectionStart) {
    picked = full.slice(el.selectionStart, el.selectionEnd)
  } else if (el) {
    picked = full.slice(el.selectionStart)
  }
  if (!picked.trim()) picked = full

  // 合成那一下也钉住项目：这一段字是这一部剧的，音频也该落在它的目录里
  // （下面那条 mediaUrl 同理）。
  const project = session.projectPath
  const result = await run(
    () =>
      runAsyncJob(
        (extra) => api.say({ project, text: picked, ...extra }),
        { prefix: 'say' },
      ),
    { key: 'say' },
  )
  if (!result) return
  if (project !== session.projectPath) return
  if (result.backend === 'estimate') {
    // 估算后端出来的是等长静音。不说的话用户会对着一段没声音的音频
    // 以为是自己音箱坏了。
    ui.warn('还没配上配音模型，念出来的是一段等长静音。去设置页看看')
  } else if (result.truncated) {
    ui.ok(`念了前 ${result.chars} 字（一次最多这么多）`)
  }
  // 加个时间戳绕开浏览器缓存：落点是固定名字，不加的话第二次点还是老那段
  audio.value = mediaUrl(project, result.rel) + '&t=' + Date.now()
  await nextTick()
  document.querySelector('audio.say')?.play?.()
}

// ---------------------------------------------------------------------------
// 从无到有的几条路，和「这本书」
// ---------------------------------------------------------------------------

async function savePremise() {
  if (!premiseDirty.value || !session.projectPath) return
  const project = session.projectPath
  const result = await run(
    () => api.saveStory({ project, premise: premise.value.trim() }),
    { key: 'premise', success: '梗概已存下' },
  )
  if (result) setStory(result, project)
}

/**
 * 让 AI 想几个选题。**不落库**——它回的是候选，人挑了才算数。
 *
 * 关键词那一格和「重出大纲」共用：往哪个方向想，这两件事要的是同一个词。
 * 有故事时那一格折在「让 AI 重出一份大纲」里，没展开就是空串，引擎收空串
 * 是合法的（自由发挥）。
 */
async function suggestIdeas() {
  if (!session.projectPath) {
    ui.warn('先选一个项目')
    return
  }
  // 想三个要跑一趟大模型（几十秒）。中途换了剧的话，这三条是照**上一部**
  // 的故事避重想出来的，摆在新这一部的框里，挑一个就存成了它的梗概。
  const project = session.projectPath
  const result = await run(
    () =>
      api.suggestPremises({
        project,
        keywords: keywords.value.trim(),
        count: 3,
      }),
    { key: 'ideas' },
  )
  if (!result) return
  if (project !== session.projectPath) {
    ui.info('那一部剧的几个点子想好了，但你已经切走了——回去再点一次')
    return
  }
  ideas.value = result.ideas ?? []
  // 引擎把已有的梗概和各集简介都算作"想过的方向"避重，所以连点两次
  // 拿回来的是新的三个。一个都没回来只可能是模型没按格式答。
  if (!ideas.value.length) ui.warn('这一轮一个都没想出来，再点一次试试')
}

/**
 * 挑中一个：填进梗概框并**立刻存下**。
 *
 * 不留在框里等失焦——挑完接着就点"让 AI 写一份大纲"的人，那一下点击不会
 * 先触发 blur（按钮在另一块里），梗概就没存进去，大纲会照着空梗概写。
 */
async function pickIdea(it) {
  premise.value = it.premise || ''
  ideas.value = []
  await savePremise()
}

/**
 * 正在长出来的那份大纲。null = 没在写。
 *
 * 那一头每隔 200 毫秒推一帧「到此为止解出来的全份」（见引擎里的
 * write_outline 和 stages/json_partial）。这块板子只是把它摆出来——
 * 出一份大纲三四十秒，不摆的话那几十秒界面上一个字都没有，
 * 而"它在想什么"正是这一步最该看见的东西。
 */
const outlineLive = ref(null)

/**
 * 这一刻已经有字的那几章。**一个字都没有的不摆。**
 *
 * 大模型写这份 JSON 的键序每次都不一样：赶上它先写 chapters 的时候，
 * 数组里会先出现一个空壳子（title 和 summary 都还没写），而模板原来那句
 * `c.title || '…'` 就把它渲成一个孤零零的「…」，一挂二三十秒。
 * 用户 2026-09-13 报的「ai 正在写的内容也不显示」就是这个——不是没在写，
 * 是写的东西还没轮到有名字的那一栏。
 */
const liveChapters = computed(() =>
  (outlineLive.value?.chapters ?? []).filter((c) => c.title || c.summary),
)

/**
 * 到此为止写出来多少字。**空窗期唯一看得见的活口。**
 *
 * 不摆这个数的话，那二三十秒里板子上只有一句「正在写…」和一个转着的点，
 * 跟卡死了长得一模一样。这个数一直在涨，是"它确实在动"的证据。
 */
const liveChars = computed(() => {
  const o = outlineLive.value
  if (!o) return 0
  // **优先用服务端那个数。** 它数的是收到的原文，不挑栏目——而下面这个
  // 合计只数得到已经解出来的那几栏，模型先写章节摘要时它一直是 0。
  if (o.raw_chars) return o.raw_chars
  const n = (s) => [...(s ?? '')].length
  return (
    n(o.logline) +
    n(o.premise) +
    n(o.genre) +
    n(o.tone) +
    (o.characters ?? []).reduce((a, c) => a + n(c.name) + n(c.identity), 0) +
    (o.chapters ?? []).reduce((a, c) => a + n(c.title) + n(c.summary) + n(c.hook), 0)
  )
})

function emptyOutlineLive() {
  return { premise: '', logline: '', genre: '', tone: '', characters: [], chapters: [] }
}

/** 重新接上的那条流（刷新之后）。null = 没接着谁。 */
let attached = null
/** 上一轮写砸的那句话，弹过的。同一句不弹第二次。 */
let shownOutlineError = ''


/**
 * 页面进来发现后台正写着一份大纲：订上那条流，把「正在写」的板子摆回来。
 *
 * 和 writeStory 走的是同一条流、同一种消息，只是发起的不是这一页。
 * 那一头推的每一帧都是"到此为止的全份"，所以中途插进来也不缺前文。
 *
 * **多一条轮询兜底**：订阅发出去那一刻活可能刚好写完，job_done 已经广播
 * 过了、没人听见——那这一页会永远显示"正在写…"。所以每五秒问一次
 * /api/story：账上没它了就收工，草稿会随那一次响应回来。
 */
function attachOutline(streamId) {
  if (outlineLive.value) return // 自己正在写，或者已经接上了
  if (attached?.id === streamId) return
  detachOutline()
  outlineLive.value = emptyOutlineLive()

  const finish = async (result) => {
    if (attached?.id !== streamId) return
    // **接的是哪部剧的那条流。** 在 A 上起了一份大纲、没写完就去看 B：
    // 这条流还挂着（B 那边没有 outline_running，没人来顶掉它），写完之后
    // 照样把草稿摆到 B 上——而那份草稿一按「采用」就写进 B 了。和预告片
    // 草稿、三条流式写作是同一个坑。
    if (attached.project !== session.projectPath) {
      detachOutline()
      return
    }
    detachOutline()
    outlineLive.value = null
    if (result) {
      draft.value = result
      bookOpen.value = false
    } else {
      // 没拿到整份结果（轮询发现它已经不在跑了）：草稿落盘了就从那儿拿
      draft.value = null
      await refreshStory()
    }
  }

  const sock = openJobSocket(
    streamId,
    (msg) => {
      if (msg.job_id !== streamId) return
      if (msg.type === 'outline_progress') outlineLive.value = msg
      else if (msg.type === 'job_done') finish(msg.result)
      else if (msg.type === 'job_error') {
        ui.error(msg.message || '这份大纲没写成')
        finish(null)
      }
    },
    () => {
      /* 断了就靠下面的轮询 */
    },
  )
  const timer = setInterval(async () => {
    if (!session.projectPath) return
    try {
      const s = await api.getStory(session.projectPath)
      if (!s.outline_running) finish(null)
    } catch {
      /* 下一轮再问 */
    }
  }, 5000)
  attached = { id: streamId, sock, timer, project: session.projectPath }
}

function detachOutline() {
  if (!attached) return
  clearInterval(attached.timer)
  attached.sock?.close()
  attached = null
}

onUnmounted(detachOutline)

/**
 * 写大纲。
 *
 * **梗概不是必填的。** 选题本来就是整条流水线上最难从零开始的一步，把它
 * 做成硬门槛等于又把人摁回空白框前面发呆。
 *
 * 已经有故事时也能重出：出来的先是草稿，采用了才换掉现在这几章。
 */
async function writeStory() {
  const streamId = 'outline-' + Math.random().toString(36).slice(2, 10)
  let sock = null
  let opened = false

  // 那一头写完（或者写砸了）从这条 socket 上说一声。理由同 writeChapter：
  // 出一份大纲三四十秒，HTTP 请求占着 Crow 的一条 I/O 线程那么久，落在
  // 同一条线程上的连接会跟着冻住。所以 POST 当场回一句"开始了"。
  let settle = null
  const finished = new Promise((r) => {
    settle = r
  })

  // 这一趟是替哪部剧写的。理由同上面两条流：换剧之后把上一部的大纲草稿
  // 摆在这一部上，点一下「采用」就写进去了——和预告片草稿那处是同一个坑。
  const owner = session.projectPath
  const mine = () => owner === session.projectPath

  // 从这一刻起就摆出那块"正在写"的板子，不等第一帧到。
  outlineLive.value = emptyOutlineLive()

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      (msg) => {
        if (msg.job_id !== streamId) return
        // 思考流。**这三步（写大纲 / 写正文 / 改一段）没走 runAsyncJob**，
        // 所以要在这儿自己接一下——而它们恰恰是思考最久的三步。
        if (msg.type === 'job_thinking') return thinking.push(streamId, msg.text ?? '')
        // 正在长出来的那份大纲。**整份换掉，不是往上累加**——那一头推的
        // 是"到此为止解出来的全份"，累加会把每一帧的前缀叠成一团。
        if (msg.type === 'outline_progress') {
          outlineLive.value = msg
          return
        }
        if (msg.type === 'job_done') {
          settle({ ok: true, result: msg.result })
          return
        }
        if (msg.type === 'job_error') {
          settle({ ok: false, message: msg.message })
        }
      },
      () => {
        // 连接没了也一定要把等的人放出来，否则这一页会一直显示"正在写…"。
        settle({ ok: false, message: '和引擎的连接断了，这份大纲写没写完不好说' })
        resolve()
      },
      () => {
        opened = true
        resolve()
      },
    )
    setTimeout(resolve, 2000)
  })
  // 顶栏那块「正在思考」。**开在这儿、清在 finally 里**——这三步都有
  // 好几条提前 return 的路，手动清总会漏一条，而漏掉的后果是顶栏
  // 永远显示在想。
  if (opened) thinking.start(streamId)

  let result = null
  try {
    const started = await run(
      () =>
        api.writeOutline({
          project: owner,
          premise: premise.value.trim(),
          scale: scale.value,
          keywords: keywords.value.trim(),
          // socket 没开就退回老路：HTTP 一直等到写完。慢，但至少拿得到结果。
          ...(opened ? { stream: streamId, async: true } : {}),
        }),
      { key: 'write' },
    )

    result = started
    if (started && started.started) {
      const fin = await finished
      result = fin.ok ? fin.result : null
      if (!fin.ok) ui.error(fin.message || '这份大纲没写成')
    }
  } finally {
    // 上面那句注释说的就是这两行。它原来**不在 finally 里**——今天不漏是
    // 因为 `run()` 把异常吞了（catch 完回 undefined），也就是说这两行的
    // 正确性挂在另一个函数的实现细节上。哪天 run 改成往外抛、或者中间加
    // 一条提前 return，顶栏就会永远显示「正在思考」，还漏一条 WebSocket。
    // 隔壁 useAsyncJob 用的就是真 finally。
    thinking.finish(streamId)
    sock?.close()
  }
  outlineLive.value = null
  if (!mine()) {
    // 上一部剧的大纲，别摆在这一部上——摆了就有人会去点「采用」。
    ui.warn('中途换了项目，刚出的那份大纲没有留下')
    return
  }
  if (result) {
    draft.value = result
    bookOpen.value = false
  }
}

/** 粘一段现成的进来。切章节不走大模型——那是机械活，而且比模型稳。 */
async function importPasted() {
  if (!pasted.value.trim()) {
    ui.warn('先把文本粘进来')
    return
  }
  // 粘进来的可能是一整本书，切一趟要几秒。换剧那一下这份草稿会被清掉
  // （见换剧那个 watch），而回包落地又会把它摆回来——摆在新这一部上，
  // 点一下「采用」就整本写进去了。
  const project = session.projectPath
  const result = await run(
    () => api.importStory({ project, text: pasted.value }),
    { key: 'import' },
  )
  if (result && project !== session.projectPath) {
    ui.warn('中途换了项目，切好的那一份没有留下')
    return
  }
  if (result) {
    draft.value = result
    pasting.value = false
    pasted.value = ''
    bookOpen.value = false
  }
}

/**
 * 直接开写：不写梗概、不挑体量、不等 AI，建一章空的就进编辑器。
 *
 * **这一条是"开始写之前不需要配置任何东西"。** 结构可以后补：写完点
 * 「提人物」，人物关系地点就出来了。先写后理，本来就是很多人写东西的顺序。
 */
async function startBlank() {
  const project = session.projectPath
  const result = await run(
    () =>
      api.adoptStory({
        project,
        story: {
          premise: premise.value.trim(),
          scale: scale.value,
          episode_duration_s: 60,
          chapters: [{ chapter_id: 'ch01', title: '第一章', summary: '', text: '' }],
        },
        overwrite: true,
      }),
    // **要 refresh。** 这一下从"没有故事"变成"有一章"，而顶栏那一步的对勾
    // 读的是 `/bff/flow` 的 done.story（判据就是章节数），不重拉的话人写完
    // 第一章、导航上还说他没写故事，下一步那个点也还钉在「故事」上。
    // 隔壁「采用这一份」（adopt）一直是带着的，这条和它是同一件事。
    { key: 'blank', refresh: true },
  )
  if (!result) return
  setStory(result, project)
  await nextTick()
  boxes[current.value]?.focus()
}

/** 加一章。空的，接着写。 */
async function addChapter() {
  const project = session.projectPath
  const next = chapters.value.map((c) => ({ ...c }))
  const id = 'ch' + String(next.length + 1).padStart(2, '0')
  next.push({ chapter_id: id, title: `第 ${next.length + 1} 章`, summary: '', text: '' })
  const result = await run(
    () =>
      api.adoptStory({
        project,
        story: { ...story.value, chapters: next },
        overwrite: true,
      }),
    { key: 'addch' },
  )
  if (!result) return
  setStory(result, project)
  current.value = id
  await nextTick()
  boxes[id]?.focus()
}

/** 老项目：把已经写好的那几集反推成故事骨架。不碰大模型，也不重新分集。 */
async function reverseFromEpisodes() {
  const project = session.projectPath
  const result = await run(
    () => api.storyFromEpisodes({ project, overwrite: true }),
    // 同 startBlank：这一下也是从无到有地长出章节，对勾要跟着亮。
    { key: 'reverse', refresh: true },
  )
  if (!result) return
  setStory(result, project)
  ui.ok(`反推出 ${result.chapters} 章。接着点左边「提人物」把人物提出来`)
}

/** 让 AI 读一遍正文，把人物关系地点提出来。**正文一个字不动。** */
async function analyzeStory() {
  // 读一遍整本书要几分钟。中途换了剧的话：请求本身会带着新那一部的路径
  // （runAsyncJob 要等 socket 开才发），而更要紧的是回包落地那一下——
  // `draft.value = result` 会把**上一部剧的骨架**摆进新这一部的草稿位，
  // 而「采用」是按当前项目写的。换剧那个 watch 清过一次 draft，但清在回
  // 包之前，挡不住。
  const project = session.projectPath
  const result = await run(
    () =>
      runAsyncJob(
        (extra) => api.analyzeStory({ project, ...extra }),
        { prefix: 'analyze' },
      ),
    { key: 'analyze' },
  )
  if (!result) return
  if (project !== session.projectPath) {
    // 这一份只在内存里（analyze 不落盘），所以要说一句，别当没发生过。
    ui.info('那一部剧的人物地点提出来了，但你已经切走了——回去再点一次')
    return
  }
  draft.value = result
}

/**
 * 丢掉这份草稿。
 *
 * **服务端那份也要删。** 草稿是落库的（story_draft.json），只清这个 ref
 * 的话刷新一下它又回来了——而用户刚刚明确说了不要。
 *
 * 先清界面再发请求：这一步没有什么可失败的，而让人对着一份"已经丢了"的
 * 草稿等一个来回没有意义。真没删掉也不致命，下次点还能再丢。
 */
async function dropDraft() {
  draft.value = null
  if (!session.projectPath) return
  try {
    await api.dropStoryDraft({ project: session.projectPath })
  } catch {
    // 删不掉就算了，不打扰。刷新之后它会再出现，那时候再点一次。
  }
}

async function adoptDraft() {
  if (!draft.value) return
  const project = session.projectPath
  // 重出的大纲会把现在这几章整份换掉。提人物那种草稿正文不变，不用问。
  const replacing =
    hasStory.value &&
    draft.value.story?.chapters?.some((c, i) => c.text !== chapters.value[i]?.text)
  if (replacing && !confirm(`采用会把现在这 ${chapters.value.length} 章整份换掉。确定？`)) return
  const result = await run(
    () =>
      api.adoptStory({
        project,
        story: draft.value.story,
        overwrite: true,
      }),
    // success 不写在这儿：下一句要按草稿的 needs_analysis 分两种说法
    { key: 'adopt', refresh: true },
  )
  if (result) {
    // **「下一步让 AI 读一遍」这句提醒，引擎是特意送上来的。**
    //
    // 出草稿那几条接口都带一个 `needs_analysis`（characters 为空就是真），
    // 而它旁边的注释写着：「前端靠这个数提醒人『下一步让 AI 读一遍』，
    // 不然采用之后会一路走到分镜才发现资产库是空的。」——这个数从来没人读，
    // 那条路也就一直是：采用 → 去设定 → 三格全空 → 不知道该按哪儿。
    //
    // 不加新按钮：「提人物」本来就在这一页的工具行上，这里只是把话说到。
    ui.ok(
      draft.value?.needs_analysis
        ? '采用了。接着点「提人物」让 AI 读一遍，人物和地点才有'
        : '采用了，写进项目了',
    )
    for (const k of Object.keys(buf)) delete buf[k]
    dirtySnapshot.clear()
    setStory(result, project)
    draft.value = null
  }
}

// ---------------------------------------------------------------------------
// 展开正文
// ---------------------------------------------------------------------------

/**
 * 展开一章。**边写边长在编辑器里**，写完直接落库。
 *
 * 一章一两分钟。攒齐了再蹦出来的话那一两分钟界面上什么都没有——而这一步
 * 是整条路上最长的一次等待。
 *
 * 引擎那边照旧要模型回 JSON（正文之外还要它标钩子，那些钩子是一集停在真
 * 悬念上的全部依据），只在 token 流上把 text 那个字段解出来推过来，
 * 所以这里收到的已经是干净的正文。
 */
async function writeChapter(chapterId, overwrite = false) {
  if (overwrite && !confirm('重写会把这一章现在的正文整份顶掉。确定？')) return
  // 这一趟是替哪部剧写的。理由见 reviseSelection 里那段（换剧之后在途的流
  // 会把上一部的字画进这一部，收尾那下还会把整份 story 换成上一部的）。
  const owner = session.projectPath
  const mine = () => owner === session.projectPath
  const streamId = 'chapter-' + Math.random().toString(36).slice(2, 10)
  let acc = ''
  let sock = null
  let opened = false

  // 那一头写完（或者写砸了）会从这条 socket 上说一声。
  //
  // **为什么不等 HTTP 那个响应了。** 引擎那边一条 I/O 线程管着一批连接，
  // 请求在它上面占多久，落在同一条线程上的连接就干等多久——而写一章是
  // 一两分钟。实测那期间别的请求会卡满二十多秒，顶栏那块表更是会直接冻住
  // （它是长连接，认准了一条线程）。所以现在 POST 当场回一句"开始了"，
  // 结果从这儿回来。
  let settle = null
  const finished = new Promise((r) => {
    settle = r
  })

  // 从这一刻起就锁章，不等第一个字到。**at 先给 0**：光标从头上开始，
  // 第一个字到之前也看得见"它准备从这儿写"。
  streaming.value = { chapter_id: chapterId, from: 0, at: 0 }
  // 新起一轮就重新跟上：上一轮里人滚上去看过，不该影响这一轮。
  stuck.value = true

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      async (msg) => {
        if (msg.job_id !== streamId) return
        // 思考流。**这三步（写大纲 / 写正文 / 改一段）没走 runAsyncJob**，
        // 所以要在这儿自己接一下——而它们恰恰是思考最久的三步。
        if (msg.type === 'job_thinking') return thinking.push(streamId, msg.text ?? '')
        // job_done / job_error 是"这件活完了"的通用信号（见 job_stream.hpp），
        // story_token 是这条路独有的、正在长出来的正文。
        //
        // **story_error 故意不在这儿接。** 引擎写砸的时候两条都会广播
        // （story_api.cpp 那两个 catch 里先播 story_error，再由 start_async
        // 播 job_error），而"把流了一半的字撤掉"这件事底下已经做了：
        // `if (!result)` 那一支把这一章放回 `chapters` 里原来那份。接一下
        // story_error 等于同一件事做两遍，还得多想一次谁先到。
        if (msg.type === 'job_done') {
          settle({ ok: true, result: msg.result })
          return
        }
        if (msg.type === 'job_error') {
          settle({ ok: false, message: msg.message })
          return
        }
        if (msg.type !== 'story_token') return
        acc += msg.text ?? ''
        if (!mine()) return // 换剧了，别往新这一部的编辑器上画
        buf[chapterId] = acc
        streaming.value = { chapter_id: chapterId, from: 0, at: acc.length }
        await nextTick()
        fit(boxes[chapterId])
        keepEndVisible(chapterId)
      },
      () => {
        // 连接没了。**一定要把等的人放出来**，否则这一章会永远显示"写着…"，
        // 而那比报个错难受得多。
        settle({ ok: false, message: '和引擎的连接断了，这一章写没写完不好说' })
        resolve()
      },
      () => {
        opened = true
        resolve()
      },
    )
    setTimeout(resolve, 2000)
  })
  // 顶栏那块「正在思考」。**开在这儿、清在 finally 里**——这三步都有
  // 好几条提前 return 的路，手动清总会漏一条，而漏掉的后果是顶栏
  // 永远显示在想。
  if (opened) thinking.start(streamId)

  let result = null
  try {
    const started = await run(
      () =>
        api.writeChapter({
          project: owner,
          chapter_id: chapterId,
          overwrite,
          // socket 没开就退回老路：让 HTTP 那个请求一直等到写完。慢，但至少
          // 拿得到结果——没有 socket 的话异步那条根本没地方把结果送回来。
          ...(opened ? { stream: streamId, async: true } : {}),
        }),
      { key: 'chapter:' + chapterId },
    )

    result = started
    if (started && started.started) {
      // 异步那条：HTTP 只说了"开始了"，真正的结果在 socket 上。
      const fin = await finished
      result = fin.ok ? fin.result : null
      if (!fin.ok) ui.error(fin.message || '这一章没写成')
    }
  } finally {
    // 理由同 writeStory 里那段：注释一直说"清在 finally 里"，而它原来不在。
    thinking.finish(streamId)
    sock?.close()
    // **锁也清在这儿。** 它原来在 finally 后面一行——只要中间有任何一条
    // 路把异常抛出去，这一章就永远锁着（`locked` 判的就是 streaming），
    // 人只能换个项目或者重开页面。而这一页为同一件事已经加过一层兜底
    // （`s.src === 'batch' && !writer.running`），那层兜底只管批量那条，
    // 写一章、改一段这两条靠的正是这一句。
    streaming.value = null
  }
  if (!mine()) {
    // 人已经在看别的剧了。**setStory 在这儿是最危险的一下**：它会把上一部
    // 的整份故事装进这一部的界面，接着任何一次自动保存都写到错的项目上。
    ui.warn('中途换了项目，这一章写完了但没有装进来。回去那部剧刷新一下就看得到')
    return
  }
  if (!result) {
    // 写砸了：把流出来那半截清掉，别在稿子里留一段没头没尾的东西。
    // **重写失败要放回原来那份**，不是清空——原来那一章是好好的。
    const was = chapters.value.find((c) => c.chapter_id === chapterId)
    buf[chapterId] = was?.text ?? ''
    await nextTick()
    fit(boxes[chapterId])
    return
  }
  // 落库那份才是权威的（解析、守卫、钩子都在那边）
  setStory(result)
  ui.ok(`${chapterId} 写了 ${result.chars} 字`)
  await nextTick()
  if (scroller.value && chapterId === current.value) scroller.value.scrollTop = 0
}

async function writeAllChapters() {
  const started = await run(
    () => api.writeChapters({ project: session.projectPath }),
    { key: 'chapters' },
  )
  if (started) {
    ui.ok(`开始展开 ${started.chapters} 章`)
    follow.value = true
    writer.start()
  }
}

async function stopWriting() {
  await run(() => api.stopSeries(), { key: 'stopWrite', success: '已停' })
  writer.poll()
}
</script>

<template>
  <div class="ed" :class="{ 'ed--focus': ui.focusMode }">
    <EmptyState
      v-if="!session.hasProject"
      class="ed__center"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="故事挂在项目上。在项目库那条栏里点一个。"
    />
    <div v-else-if="loading && !hasStory" class="ed__center tiny dim">读取中…</div>
    <!-- **读不出来的时候不能摆"开始写"那一屏。** 那一屏说的是「这部剧还
         没有故事」，而读砸了的时候有没有根本不知道——按下「直接开写」，
         要是刚才只是 story.json 一时读不出来（文件坏了引擎回 400），那
         一下就把它盖掉了。 -->
    <EmptyState
      v-else-if="loadError"
      class="ed__center"
      icon="warn"
      tone="warn"
      title="读不到这部剧的故事"
      :hint="loadError"
    />

    <template v-else>
      <!-- ================= 左：章节 ================= -->
      <!-- 书的形状要一直看得见，尤其批量跑一个多小时的时候：这一栏同时
           就是进度条。它占的是 38em 两边本来就空着的边距，不占高度。 -->
      <aside v-if="listShown && (hasStory || draft)" class="ed__list">
        <div class="list__head">
          <button
            class="book"
            :class="{ 'is-on': bookOpen }"
            type="button"
            :disabled="!!draft"
            title="梗概、体量，或者让 AI 重出一份大纲"
            @click="bookOpen = !bookOpen"
          >
            <span class="book__t truncate">
              {{ draft ? '草稿' : story?.logline || premise || '这本书' }}
            </span>
            <span class="book__s tiny dim">
              {{ listChapters.length }} 章<template v-if="!draft"> · {{ totalChars }} 字</template>
            </span>
          </button>
          <button
            class="btn btn--ghost btn--sm list__fold"
            type="button"
            title="收起章节列表"
            @click="listOpen = false"
          >
            <AppIcon name="arrowLeft" :size="14" />
          </button>
        </div>

        <div class="list__rows">
          <button
            v-for="(c, i) in listChapters"
            :key="c.chapter_id"
            class="ch"
            :class="{
              'is-on': !draft && c.chapter_id === current,
              'is-ghost': !!draft,
              'is-blank': !draft && isBlank(c.chapter_id),
            }"
            type="button"
            :disabled="!!draft"
            :title="failedText(c.chapter_id) ? '写砸了：' + failedText(c.chapter_id) : c.summary || ''"
            @click="pickChapter(c.chapter_id)"
          >
            <span class="ch__n">{{ i + 1 }}</span>
            <span class="ch__t truncate">{{ c.title || '未命名' }}</span>
            <span v-if="!draft" class="ch__s" :class="stateClass(c.chapter_id)">
              {{ stateText(c.chapter_id) }}
            </span>
          </button>
        </div>

        <!-- 页脚只留每次都要按的那一个。批量展开的进度和「停」在状态栏
             （那份多一句"正在写第几章"和「跟着翻」），这儿原来又写了一遍，
             删了；跑的时候把按钮藏起来就够。「加一章」是整本书的事，
             搬去「这本书」那一屏。 -->
        <div v-if="!draft && (unwritten || needsAnalysis) && !writer.running" class="list__foot">
          <button
            v-if="unwritten"
            class="btn btn--ai btn--sm"
            type="button"
            :disabled="isBusy('chapters')"
            @click="writeAllChapters"
          >
            <AppIcon name="sparkle" :size="13" />
            展开剩下 {{ unwritten }} 章
          </button>
          <button
            v-if="needsAnalysis"
            class="btn btn--ghost btn--sm"
            type="button"
            :disabled="isBusy('analyze')"
            title="让 AI 读一遍正文，把人物、关系、地点提出来。正文一个字不动"
            @click="analyzeStory"
          >
            {{ isBusy('analyze') ? '正在读…' : '提人物' }}
          </button>
        </div>
      </aside>

      <!-- ================= 中：正文 ================= -->
      <section class="ed__main">
        <div
          ref="scroller"
          class="ed__scroll"
          @mousedown="onPaperDown"
          @scroll.passive="onScroll"
        >
          <!-- 草稿。AI 写完先摆出来给人看，点了采用才落库 -->
          <div v-if="draft" class="doc draft">
            <div class="doc__head">
              <h1 class="doc__title">{{ draft.story?.logline || '一份新的故事' }}</h1>
            </div>
            <p class="doc__sum">
              {{ draft.chapters }} 章 ·
              {{ draft.story?.characters?.length ?? 0 }} 个人 ·
              {{ draft.story?.locations?.length ?? 0 }} 个地方。
              采用之前原来那份一个字不动。
            </p>
            <div class="draft__list">
              <div v-for="c in draft.story?.chapters ?? []" :key="c.chapter_id" class="dch">
                <b>{{ c.title }}</b>
                <span class="small dim">{{ c.summary }}</span>
              </div>
            </div>
            <div class="row">
              <button
                class="btn btn--primary"
                type="button"
                :disabled="isBusy('adopt')"
                @click="adoptDraft"
              >
                采用这一份
              </button>
              <button class="btn btn--ghost" type="button" @click="dropDraft">丢弃</button>
            </div>
          </div>

          <!-- 还没有故事：从这儿开始。有故事时点开「这本书」也是这一块。
               同一个框架，页面形状不变，只是往里长东西。 -->
          <div v-else-if="!hasStory || bookOpen" class="doc start">
            <div class="doc__head">
              <h1 class="doc__title">{{ hasStory ? '这本书' : '从这儿开始' }}</h1>
              <span class="spacer" />
              <button
                v-if="hasStory"
                class="btn btn--ghost btn--sm"
                type="button"
                @click="bookOpen = false"
              >
                回到正文
              </button>
            </div>
            <textarea
              v-model="premise"
              class="textarea start__premise"
              rows="4"
              placeholder="这部剧讲什么？想好了就写一句，比如：深夜便利店，前任推门进来，手里拿着五年前她送的那把伞。&#10;没想好就空着，让 AI 想几个给你挑。"
              @blur="savePremise"
            />
            <!-- 「想几个给我挑」。**紧贴梗概框**：它回答的就是这个框里该写
                 什么，隔一块就成了另一件事。十几秒回来三个方向，比一口气
                 出一整份大纲（一分多钟，只能整份收整份扔）轻得多。 -->
            <div class="row row--wrap">
              <button
                class="btn btn--ai btn--sm"
                type="button"
                :disabled="isBusy('ideas')"
                @click="suggestIdeas"
              >
                <AppIcon name="sparkle" :size="14" />
                {{ isBusy('ideas') ? '正在想…' : ideas.length ? '再想三个' : '想几个给我挑' }}
              </button>
              <template v-if="ideas.length">
                <span class="small dim">挑一个就填进上面那个框</span>
                <span class="spacer" />
                <button class="btn btn--ghost btn--sm" type="button" @click="ideas = []">
                  都不要
                </button>
              </template>
            </div>
            <div v-if="ideas.length" class="ideas">
              <button
                v-for="(it, i) in ideas"
                :key="i"
                class="idea"
                type="button"
                @click="pickIdea(it)"
              >
                <b class="idea__t">{{ it.title }}</b>
                <span class="idea__p">{{ it.premise }}</span>
                <span v-if="it.hook" class="idea__h">钩子 · {{ it.hook }}</span>
              </button>
            </div>
            <!-- 关键词和篇幅只在**出大纲**的时候有用。没故事时它们就是起手式，
                 摊开；有了故事之后重出是破坏性又少用的事，连按钮一起折进
                 「▸ 让 AI 重出一份大纲」里，这一屏只剩梗概和加一章。
                 「分几集在设定·分集那儿定」那句删了：在解释什么不在这儿。 -->
            <component :is="hasStory ? 'details' : 'div'" class="regen stack stack--sm">
            <summary v-if="hasStory" class="fold__t">让 AI 重出一份大纲</summary>
            <input
              v-model="keywords"
              class="input"
              placeholder="往哪个方向？热点词、题材都行，可留空（比如：重生复仇、破镜重圆）"
            />
            <div class="row row--wrap">
              <div class="scales">
                <button
                  v-for="sc in SCALES"
                  :key="sc.key"
                  class="scale"
                  :class="{ 'is-on': scale === sc.key }"
                  type="button"
                  :title="sc.hint"
                  @click="scale = sc.key"
                >
                  {{ sc.label }}
                </button>
              </div>
            </div>
            <div class="row row--wrap">
              <template v-if="!hasStory">
                <!-- **摆在最前面。** 开始写之前不需要配置任何东西；
                     想自己写的人应该一眼看见"从这儿进去"。 -->
                <button
                  class="btn btn--primary"
                  type="button"
                  :disabled="isBusy('blank')"
                  @click="startBlank"
                >
                  直接开写
                </button>
                <!-- **outlineLive 也要算在忙里。** 异步那条 run() 一拿到
                     202 就结束了，光看 isBusy('write') 的话按钮立刻变回
                     "让 AI 写一份大纲"——再点一下就是第二份在跑，而两份
                     写完会互相顶掉。 -->
                <button
                  class="btn btn--ai"
                  type="button"
                  :disabled="isBusy('write') || !!outlineLive"
                  @click="writeStory"
                >
                  <AppIcon name="sparkle" :size="15" />
                  {{ isBusy('write') || outlineLive ? '正在写…' : '让 AI 写一份大纲' }}
                </button>
                <button class="btn btn--ghost" type="button" @click="pasting = !pasting">
                  粘一份现成的
                </button>
                <button
                  v-if="canReverse"
                  class="btn btn--ghost"
                  type="button"
                  :disabled="isBusy('reverse')"
                  @click="reverseFromEpisodes"
                >
                  {{
                    isBusy('reverse')
                      ? '正在反推…'
                      : `从已有的 ${session.episodes.length} 集反推`
                  }}
                </button>
              </template>
              <template v-else>
                <button
                  class="btn btn--ai"
                  type="button"
                  :disabled="isBusy('write') || !!outlineLive"
                  @click="writeStory"
                >
                  <AppIcon name="sparkle" :size="15" />
                  {{ isBusy('write') || outlineLive ? '正在写…' : '重出' }}
                </button>
                <span class="tiny dim">出来先是草稿，采用了才会换掉现在这 {{ chapters.length }} 章</span>
              </template>
            </div>
            </component>

            <!-- 从左栏页脚搬来的。整本书的事，配在整本书这一屏 -->
            <div v-if="hasStory" class="row">
              <button
                class="btn btn--ghost btn--sm"
                type="button"
                :disabled="isBusy('addch')"
                title="加一章空的，接着写"
                @click="addChapter"
              >
                <AppIcon name="plus" :size="13" />
                加一章
              </button>
            </div>

            <!-- 正在长出来的那份大纲。**边写边看**，见 outlineLive。
                 只摆已经有字的那几项：一上来全是空框的话，看着像坏了。 -->
            <div v-if="outlineLive" class="live stack stack--sm">
              <div class="row tiny dim">
                <span class="live__dot" />
                正在写…（键序每次不一样，先出什么看它自己）
                <!-- **这个数是空窗期唯一看得见的活口。** 模型有时先写章节
                     摘要，那几十秒里上面几栏全是空的——只有这个数在涨，
                     人才知道它没卡死。 -->
                <template v-if="liveChars">· 已经写了 {{ liveChars }} 字</template>
              </div>
              <p v-if="outlineLive.logline" class="live__line">
                {{ outlineLive.logline }}
              </p>
              <p v-else-if="outlineLive.premise" class="live__line">
                {{ outlineLive.premise }}
              </p>
              <p v-if="outlineLive.genre || outlineLive.tone" class="tiny dim">
                {{ [outlineLive.genre, outlineLive.tone].filter(Boolean).join(' · ') }}
              </p>
              <p v-if="outlineLive.characters?.length" class="tiny dim">
                {{
                  outlineLive.characters
                    .filter((c) => c.name)
                    .map((c) => c.name + (c.identity ? `（${c.identity}）` : ''))
                    .join('、')
                }}
              </p>
              <!-- **空壳子不摆。** 见 liveChapters：模型先写 chapters 时
                   数组里会先冒出一个 title 和 summary 都还没写的空对象，
                   原来那句 `c.title || '…'` 把它渲成一个孤零零的「…」，
                   一挂二三十秒，看着就像坏了。 -->
              <ol v-if="liveChapters.length" class="live__chapters">
                <li v-for="(c, i) in liveChapters" :key="i">
                  <b>{{ c.title || '…' }}</b>
                  <!-- 中间那个点不能省：HTML 会把标签之间的空白折掉，
                       写成「双面人生林雨报警未果」连成一句读不出断在哪。 -->
                  <span v-if="c.summary" class="dim">&nbsp;·&nbsp;{{ c.summary }}</span>
                </li>
              </ol>
            </div>

            <div v-if="pasting" class="stack stack--sm">
              <textarea
                v-model="pasted"
                class="textarea mono"
                rows="10"
                placeholder="小说、剧本、大纲都行。认得出「第三章」「## 标题」就照它分章，认不出就按字数在段落边界上切。版权自负。"
              />
              <div class="row">
                <button
                  class="btn btn--primary"
                  type="button"
                  :disabled="isBusy('import')"
                  @click="importPasted"
                >
                  {{ isBusy('import') ? '切着…' : '切成章节' }}
                </button>
                <button class="btn btn--ghost" type="button" @click="pasting = false">
                  取消
                </button>
              </div>
            </div>
          </div>

          <!-- 一章 -->
          <div v-else-if="chapter" class="doc doc--chapter">
            <!-- 章头在正文里，随正文滚。左栏收起时它就是切换章节的地方。 -->
            <div class="doc__head">
              <!-- **窄屏不出这个按钮。** `listShown` 是
                   `listOpen && !narrow && !focusMode`，而窄屏（≤1100px）
                   下面根本没有左栏那一版——媒体查询里只把右边的对话栏挪了
                   位置，`.ed__list` 整个不渲染。所以少了 `!narrow` 的话，
                   这个按钮在窄窗口里一直亮着、点了 `listOpen = true` 却
                   什么都不会出现（还顺手把这个偏好写进了 localStorage），
                   点一百下都一样。
                   窄屏下换章走旁边那个 select，它的条件是同一个
                   `!listShown`，本来就一直在。 -->
              <button
                v-if="!listShown && !narrow && !ui.focusMode"
                class="btn btn--ghost btn--sm"
                type="button"
                title="章节列表"
                @click="listOpen = true"
              >
                <AppIcon name="menu" :size="15" />
              </button>
              <select v-if="!listShown" v-model="current" class="select doc__pick">
                <option v-for="(c, i) in chapters" :key="c.chapter_id" :value="c.chapter_id">
                  {{ chapterLabel(c, i) }} · {{ stateText(c.chapter_id) }}
                </option>
              </select>
              <h1 v-else class="doc__title">{{ chapterLabel(chapter, index) }}</h1>
              <span class="spacer" />

              <span v-if="locked" class="doc__live"><span class="dot" /> AI 正在写</span>
              <button
                v-if="locked && writer.running"
                class="btn btn--danger btn--sm"
                type="button"
                @click="stopWriting"
              >
                停
              </button>
              <!-- 空章唯一的 AI 入口就在这儿。写过的章要重写，在「…」里，
                   破坏性又少用的事不配常驻按钮。 -->
              <button
                v-else-if="!locked && !body.trim()"
                class="btn btn--ai btn--sm"
                type="button"
                :disabled="isBusy('chapter:' + current)"
                @click="writeChapter(current)"
              >
                <AppIcon name="sparkle" :size="13" />
                {{
                  isBusy('chapter:' + current)
                    ? '正在写…'
                    : chapter.summary
                      ? '照大纲写这一章'
                      : '让 AI 写这一章'
                }}
              </button>

              <div class="menu">
                <button
                  class="btn btn--ghost btn--sm menu__btn"
                  type="button"
                  title="这一章的其他事"
                  @click="menuOpen = !menuOpen"
                >
                  …
                </button>
                <template v-if="menuOpen">
                  <div class="menu__veil" @click="menuOpen = false" />
                  <div class="menu__pop">
                    <button
                      class="menu__item"
                      type="button"
                      :disabled="locked || !body.trim() || isBusy('chapter:' + current)"
                      @click="menuOpen = false; writeChapter(current, true)"
                    >
                      重写整章
                    </button>
                    <button
                      class="menu__item"
                      type="button"
                      :disabled="!body.trim() || isBusy('say')"
                      @click="menuOpen = false; readAloud()"
                    >
                      从光标处朗读
                    </button>
                    <button
                      v-if="chapter.summary"
                      class="menu__item"
                      type="button"
                      @click="menuOpen = false; summaryOpen = !summaryOpen"
                    >
                      {{ summaryOpen ? '折起大纲' : '展开大纲' }}
                    </button>
                    <!-- 破坏性又少用，配在这儿不配常驻按钮。确认框把代价写成数字。 -->
                    <button
                      class="menu__item menu__item--danger"
                      type="button"
                      :disabled="locked || isBusy('delch')"
                      @click="menuOpen = false; deleteChapter()"
                    >
                      删这一章
                    </button>
                  </div>
                </template>
              </div>
            </div>
            <!-- 大纲那一句一直在。AI 批量写正是照着它写的，写的人不该看不到。 -->
            <p
              v-if="chapter.summary && summaryOpen"
              class="doc__sum"
              title="点一下折起来"
              @click="summaryOpen = false"
            >
              {{ chapter.summary }}
            </p>
            <button
              v-else-if="chapter.summary"
              class="doc__sumfold tiny dim"
              type="button"
              @click="summaryOpen = true"
            >
              大纲 ▸
            </button>

            <!-- 打字机模式：正在写的那一段亮，其余压暗。
                 **textarea 没法给单独一段上色**，所以在它后面垫一层排版
                 一模一样的镜像，由镜像画高亮，输入框本身文字透明、只留光标。
                 这是给 textarea 做语法高亮的老办法，好处是保住了
                 "偏移即 selectionStart"——上次改错地方的根因就是偏移。 -->
            <div class="ed__stack" :class="{ 'ed__stack--dim': ui.focusMode }">
              <!-- 行号。高度照镜像量出来的走，一段折三行只占一个号。
                   点一下光标落到那一行开头。 -->
              <div class="ed__gutter" aria-hidden="true">
                <div
                  v-for="(h, i) in lineHeights"
                  :key="i"
                  class="ed__ln"
                  :class="{ 'is-now': blocks[i]?.now }"
                  :style="{ height: h + 'px' }"
                  @mousedown.prevent
                  @click="goLine(i)"
                >
                  {{ i + 1 }}
                </div>
              </div>
              <div class="ed__page">
                <!-- 镜像常驻：不在专注模式时字是透明的，只为量行高。
                     写在一行上——块之间多一个空白文本节点，pre-wrap 会把它
                     排成一行，行号就全错一格。
                     **AI 光标画在这一层**：这里的每个字和输入框里那个字
                     一一对应，所以把光标插在第几个字后面，它就正好落在
                     屏幕上那个位置——不用去算像素。那个 <i> 宽度为零，
                     不挤动一个字。 -->
                <div ref="mirror" class="ed__mirror" aria-hidden="true"><div v-for="(b, i) in blocks" :key="i" class="ed__mline" :class="{ 'is-now': b.now }"><template v-if="aiSpot && aiSpot.line === i">{{ b.text.slice(0, aiSpot.col) }}<i class="ed__ai" data-ai="AI"></i>{{ b.text.slice(aiSpot.col) }}<template v-if="!b.text">{{ ZW }}</template></template><template v-else>{{ b.text || ZW }}</template></div></div>
              <textarea
                :key="current"
                :ref="(el) => (boxes[current] = el)"
                class="ed__area"
                spellcheck="false"
                :readonly="locked"
                :placeholder="
                  chapter.summary
                    ? '从这儿开始写，或者点上面「照大纲写这一章」让 AI 先来一版。'
                    : '从这儿开始写。写完在左边点「提人物」，人物关系和地点就出来了。'
                "
                :value="body"
                @input="onInput(current, $event)"
                @select="onSelectionChange(current, $event)"
                @mouseup="onSelectionChange(current, $event)"
                @keyup="onSelectionChange(current, $event)"
                @keydown="onAreaKey"
                @blur="flushSave(current)"
              />
              </div>
            </div>
          </div>
        </div>

        <!-- 选中一段才浮出来的那两个按钮。贴在这一格底边中间，不在字上。
             mousedown 拦掉，不然一点按钮输入框就失焦、选区就散了。 -->
        <div v-if="liveSel && target && !locked && !bookOpen" class="ed__mini">
          <button
            class="btn btn--ai btn--sm"
            type="button"
            @mousedown.prevent
            @click="openDialog"
          >
            <AppIcon name="sparkle" :size="13" />
            改这 {{ target.to - target.from }} 字
          </button>
          <button
            class="btn btn--ghost btn--sm"
            type="button"
            :disabled="isBusy('say')"
            @mousedown.prevent
            @click="readAloud"
          >
            {{ isBusy('say') ? '念着…' : '朗读' }}
          </button>
        </div>

        <!-- 状态栏。一行，永远在。 -->
        <footer v-if="hasStory && !draft" class="ed__status">
          <span class="status__item">{{ chars }} 字</span>
          <span class="status__item">第 {{ caretLine }} 行</span>
          <span class="status__item" :class="'is-' + saveState">{{ SAVE_LABEL[saveState] }}</span>
          <template v-if="audio">
            <audio :src="audio" class="say" controls />
            <button class="status__btn" type="button" title="关掉" @click="audio = null">×</button>
          </template>
          <span class="spacer" />
          <template v-if="writer.running">
            <span class="status__item status__ai">
              <span class="dot" />
              AI 展开中 {{ writer.state?.done ?? 0 }}/{{ writer.state?.total ?? 0 }}
              <template v-if="writer.state?.message"> · {{ writer.state.message }}</template>
            </span>
            <button class="status__btn" type="button" @click="stopWriting">停</button>
            <button
              class="status__btn"
              :class="{ 'is-on': follow }"
              type="button"
              title="AI 写到哪一章，编辑器就翻到哪一章"
              @click="toggleFollow"
            >
              跟着翻
            </button>
          </template>
          <button
            class="status__btn"
            :class="{ 'is-on': panelOpen }"
            type="button"
            title="对着稿子说话（Ctrl+K）"
            @click="togglePanel"
          >
            对话
          </button>
          <button
            class="status__btn"
            :class="{ 'is-on': ui.focusMode }"
            type="button"
            :title="ui.focusMode ? '退出专注（Esc）' : '专注：只留稿纸'"
            @click="ui.focusMode = !ui.focusMode"
          >
            专注
          </button>
        </footer>
      </section>

      <!-- ================= 右：对话 ================= -->
      <!-- 停靠，不盖字。装四样：正在改的那段、来回、后悔的口子、输入框。 -->
      <aside v-if="panelOpen && hasStory && !draft" class="ed__side">
        <div class="side__head">
          <b>对话</b>
          <span class="spacer" />
          <button
            class="btn btn--ghost btn--sm"
            type="button"
            title="收起"
            @click="panelOpen = false"
          >
            <AppIcon name="close" :size="14" />
          </button>
        </div>
        <div class="side__body">
          <div class="side__target tiny dim">
            <template v-if="target">
              改选中的 {{ target.to - target.from }} 字，别的一个字不动。
              <button type="button" @click="sel = null">改整章</button>
            </template>
            <template v-else>
              改整章 · {{ chars }} 字
            </template>
          </div>
          <blockquote v-if="target" class="ai__quote small">{{ target.text }}</blockquote>
          <div v-for="(t, i) in chat" :key="i" class="turn">
            <span class="turn__who tiny">{{ t.role === 'user' ? '你' : 'AI' }}</span>
            <span class="small">{{ t.text }}</span>
          </div>
          <div v-if="pending" class="ai__done">
            <span class="tiny">已经落进稿子里了</span>
            <div class="row">
              <button class="btn btn--primary btn--sm" type="button" @click="pending = null">
                就这样
              </button>
              <button class="btn btn--ghost btn--sm" type="button" @click="undoRevision">
                撤销 · Ctrl+Z
              </button>
            </div>
          </div>
        </div>
        <div class="side__foot">
          <textarea
            ref="askBox"
            v-model="instruction"
            class="textarea side__ask"
            rows="3"
            :placeholder="
              chat.length
                ? '接着说，比如「再短一点」「语气冷一些」'
                : '要改成什么样？比如「这儿太赶了，铺一下情绪」'
            "
            @keydown.ctrl.enter.prevent="revise"
            @keydown.meta.enter.prevent="revise"
          />
          <div class="row">
            <button
              class="btn btn--ai btn--sm"
              type="button"
              :disabled="isBusy('revise') || locked"
              @click="revise"
            >
              <AppIcon name="sparkle" :size="14" />
              {{
                streaming
                  ? '正在写…'
                  : isBusy('revise')
                    ? '改着…'
                    : chat.length
                      ? '再改一版'
                      : '改'
              }}
            </button>
            <span class="tiny dim">Ctrl+Enter</span>
          </div>
        </div>
      </aside>
    </template>
  </div>
</template>

<style scoped>
.regen > .fold__t {
  color: var(--text-3);
  font-size: var(--fs-xs);
  cursor: pointer;
}
.regen[open] > .fold__t {
  margin-bottom: 4px;
}
/* 「正在写」那块板子。刻意做得轻：它是过程，不是结果——
   一会儿就被真正的草稿顶掉，做重了反而让人以为已经写完了。 */
.live {
  border: 1px dashed var(--line);
  border-radius: var(--r);
  padding: 0.75rem 0.9rem;
  /* 没有 --bg-soft 这个变量（tokens.css 里是 --bg-sunken）。写错的变量
     在 CSS 里不报错，只是整条声明作废——这块板子一直是透明的，而它靠底色
     和虚线边把"这是过程不是结果"说出来。 */
  background: var(--bg-sunken);
}
.live__dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  /* ⚠️ 原来这儿是 `var(--ai, #7c5cff)`——**`--ai` 这个变量全仓没有**，
     于是一直走的是那个写死的紫。和上面 --bg-soft 那条是同一种错，只是
     这条有兜底值，不作废、改成另一个颜色，所以更难看出来。

     后果：同一页上两个「AI 正在写」的呼吸点是两个颜色。下面
     `.doc__live .dot`（每一章那一行上的那个）用的是 --accent，
     而按钮那一族 `.btn--ai` 也是 --accent 起头的渐变——base.css 里那句
     注释写着「整套流程里每一步都有一个，**样子必须统一**」。这个紫是
     唯一一处例外，还是个写死的十六进制：深浅两套主题下都不跟着变。 */
  background: var(--accent);
  margin-right: 0.4rem;
  animation: live-pulse 1.1s ease-in-out infinite;
}
@keyframes live-pulse {
  0%, 100% { opacity: 0.25; }
  50% { opacity: 1; }
}
/* 动效关掉的系统上就别闪了 */
@media (prefers-reduced-motion: reduce) {
  .live__dot { animation: none; opacity: 0.7; }
}
.live__line {
  margin: 0;
  line-height: 1.6;
}
.live__chapters {
  margin: 0;
  padding-left: 1.2rem;
  line-height: 1.7;
}
.live__chapters li + li {
  margin-top: 0.2rem;
}
/* 整块就是纸。三栏之间只有细线，没有卡片、没有圆角——那一圈线本身就是
   "这是页面里的一个控件"的提示，而这一页它就是整个页面。 */
.ed {
  flex: 1;
  min-height: 0;
  display: flex;
  position: relative;
  background: var(--surface);
}
.ed__center {
  margin: auto;
}

/* ---------- 左：章节 ---------- */
.ed__list {
  flex: none;
  width: 220px;
  display: flex;
  flex-direction: column;
  min-height: 0;
  border-right: 1px solid var(--line);
  background: color-mix(in srgb, var(--surface) 70%, var(--bg));
}
.list__head {
  flex: none;
  display: flex;
  align-items: center;
  gap: var(--s1);
  padding: var(--s2) var(--s2) var(--s2) var(--s3);
  border-bottom: 1px solid var(--line);
}
.book {
  flex: 1;
  min-width: 0;
  display: grid;
  gap: 1px;
  padding: var(--s1) var(--s2);
  border: 0;
  border-radius: var(--r-sm);
  background: transparent;
  color: var(--text);
  text-align: left;
  cursor: pointer;
}
.book:hover:not(:disabled),
.book.is-on {
  background: var(--surface-2);
}
.book:disabled {
  cursor: default;
}
.book__t {
  font-size: var(--fs-sm);
  font-weight: 600;
  color: var(--text-2);
}
.list__fold {
  width: 26px;
  padding: 0;
  flex: none;
}
.list__rows {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
  padding: var(--s2);
  display: grid;
  gap: 1px;
  align-content: start;
}
.ch {
  display: flex;
  align-items: center;
  gap: var(--s2);
  width: 100%;
  padding: 5px var(--s2);
  border: 0;
  border-radius: var(--r-sm);
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  text-align: left;
  cursor: pointer;
}
.ch:hover:not(:disabled) {
  background: var(--surface-2);
  color: var(--text);
}
.ch.is-on {
  background: var(--accent-soft);
  color: var(--accent);
}
.ch.is-blank {
  color: var(--text-3);
}
.ch.is-blank.is-on {
  color: var(--text);
}
.ch.is-ghost {
  opacity: 0.55;
  cursor: default;
}
.ch__n {
  flex: none;
  width: 1.6em;
  text-align: right;
  color: var(--text-3);
  font-variant-numeric: tabular-nums;
}
.ch.is-on .ch__n {
  color: var(--accent);
}
.ch__t {
  flex: 1;
  min-width: 0;
}
.ch__s {
  flex: none;
  font-size: var(--fs-xs);
  color: var(--text-3);
  font-variant-numeric: tabular-nums;
}
.ch__s.is-live {
  color: var(--accent);
}
.ch__s.is-dirty {
  color: var(--warn);
}
.ch__s.is-bad {
  color: var(--danger);
}
.list__foot {
  flex: none;
  display: grid;
  gap: var(--s1);
  padding: var(--s2);
  border-top: 1px solid var(--line);
}
.list__foot .btn {
  justify-content: flex-start;
}
/* 这儿原来有 .list__run：章节栏底下那行「正在写…」。现在那句话画在
   每一章自己那一行上（.doc__live / .status__ai），模板里没有 list__run。 */

/* ---------- 中：正文 ---------- */
.ed__main {
  flex: 1;
  min-width: 0;
  min-height: 0;
  display: flex;
  flex-direction: column;
  position: relative;
}
/* **唯一的滚动容器。** 用户 2026-09-11：「要留白干什么玩意」——四边只剩
   12px，刚好一个标点的宽度，第一个字不压在边线上；底下不留，余量在输入框
   自己的内边距里。flex 列是为了让下面那一章把剩下的高度全占掉。 */
.ed__scroll {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
  padding: var(--s3) var(--s3) 0;
  display: flex;
  flex-direction: column;
}
/* **铺到边。** 用户 2026-09-11：「不能到边显示吗」。之前卡在一行 38 个字
   居中，两边各空出三分之一的屏幕，看着像摆设。一行长了眼睛要多扫一段，
   但那是读的事；写的人要的是眼前这块地全是自己的稿子。 */
.doc {
  width: 100%;
}
/* **整块都是纸。** 短章的输入框也撑到底：点哪儿都在写，光标不会因为点到
   正文下面的空地就没了。fit() 定的是"至少装下内容"，flex 再把它拉到底。 */
.doc--chapter {
  flex: 1;
  display: flex;
  flex-direction: column;
}
.doc__head {
  display: flex;
  align-items: center;
  gap: var(--s2);
  min-height: 32px;
}
.doc__title {
  margin: 0;
  font-size: var(--fs-xl);
  font-weight: 600;
  letter-spacing: 0.01em;
}
.doc__pick {
  width: auto;
  max-width: 22em;
}
.doc__sum {
  margin: var(--s2) 0 0;
  color: var(--text-3);
  font-size: var(--fs-base);
  line-height: 1.6;
  cursor: pointer;
}
.doc__sumfold {
  display: block;
  margin-top: var(--s1);
  padding: 0;
  border: 0;
  background: transparent;
  cursor: pointer;
}
.doc__live {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  color: var(--accent);
  font-size: var(--fs-sm);
}
.doc__live .dot,
.status__ai .dot {
  animation: pulse 0.9s infinite alternate;
}
@keyframes pulse {
  from {
    opacity: 0.25;
  }
  to {
    opacity: 1;
  }
}

.menu {
  position: relative;
}
.menu__btn {
  width: 28px;
  padding: 0;
}
.menu__veil {
  position: fixed;
  inset: 0;
  z-index: 20;
}
.menu__pop {
  position: absolute;
  right: 0;
  top: calc(100% + 4px);
  z-index: 21;
  min-width: 11em;
  display: grid;
  padding: 4px;
  background: var(--surface-2);
  border: 1px solid var(--line);
  border-radius: var(--r);
  box-shadow: var(--shadow-2);
}
.menu__item {
  padding: 6px 10px;
  border: 0;
  border-radius: var(--r-sm);
  background: transparent;
  color: var(--text);
  font-size: var(--fs-sm);
  text-align: left;
  white-space: nowrap;
  cursor: pointer;
}
.menu__item:hover:not(:disabled) {
  background: var(--surface-3);
}
.menu__item--danger:not(:disabled) {
  color: var(--danger);
}
.menu__item:disabled {
  opacity: 0.45;
  cursor: not-allowed;
}

/* 镜像和输入框叠在一起。**每一项影响排版的属性都要一模一样**——
   字体、字号、行高、字距、宽度、padding、white-space、word-break。
   差一项两层就错位，而错位之后光标和它下面的字对不上，那比没有高亮糟得多。
   所以它们共用下面这一组声明，不各写一份。 */
.ed__stack {
  position: relative;
  width: 100%;
  margin-top: var(--s3);
  flex: 1;
  display: flex;
  align-items: stretch;
}
/* 行号列。数字小一号，但每一行的行高和正文一样，不然对不齐。 */
.ed__gutter {
  flex: none;
  min-width: 2.4em;
  padding-right: var(--s3);
  text-align: right;
  color: var(--text-3);
  font-size: 13px;
  line-height: calc(16px * 1.8);
  font-variant-numeric: tabular-nums;
  user-select: none;
  cursor: pointer;
}
.ed__ln.is-now {
  color: var(--text-2);
}
.ed__page {
  flex: 1;
  min-width: 0;
  position: relative;
  display: flex;
  flex-direction: column;
}
.ed__mirror,
.ed__area {
  font: inherit;
  /* 正文字号。界面是 13、14，正文要 16——这是稿纸不是表单。 */
  font-size: 16px;
  line-height: 1.8;
  letter-spacing: inherit;
  white-space: pre-wrap;
  overflow-wrap: break-word;
  /* 底下 40vh 的余量放在输入框**自己**的内边距里：最后一行能滚到眼睛的
     高度，而且点在那片空白上光标落到末尾——它是输入框的一部分，不是页面的。 */
  padding: 0 0 40vh;
  border: 0;
  margin: 0;
  width: 100%;
}
/* 镜像常驻只为量行高，平时字是透明的；专注时它来显字，输入框的字透明。 */
.ed__mirror {
  position: absolute;
  inset: 0;
  color: transparent;
  pointer-events: none;
}
.ed__stack--dim .ed__mirror {
  color: var(--text);
}
/* 不是"现在"那一段压暗。**压暗不是变灰**——用透明度，字还在那儿，
   扫一眼看得见上下文，只是不抢眼。 */
.ed__stack--dim .ed__mline {
  opacity: 0.28;
  transition: opacity 0.18s;
}
.ed__stack--dim .ed__mline.is-now {
  opacity: 1;
}
/* 有镜像时输入框的字透明，只留光标和选中背景 */
.ed__stack--dim .ed__area {
  color: transparent;
  caret-color: var(--accent);
}
.ed__area {
  display: block;
  position: relative;
  flex: 1;
  background: transparent;
  color: inherit;
  resize: none;
  overflow: hidden;
}
.ed__area:focus {
  outline: none;
}
.ed__area::selection {
  background: var(--accent-soft);
}
.ed__area::placeholder {
  color: var(--text-3);
}
/* AI 写着的时候锁章：**用户自己的光标藏起来**，字照常显示。
   藏它是为了不和下面那个 AI 光标混在一起——两根一样的竖线杵在不同位置，
   人第一反应是"我的光标怎么跑了"。 */
.ed__area[readonly] {
  caret-color: transparent;
}

/* AI 光标。画在镜像层上，**宽度为零**，不挤动一个字。
 *
 * 和用户那根刻意长得不一样：用户的是细的、跟着主题色闪；这个是**粗一点、
 * 圆头、带一个 AI 标**，一眼能分出"这是它在写，不是我在写"。位置由流出来
 * 的字数决定，跟用户上次点在哪儿没关系——改一段时两个会同时在屏幕上。
 *
 * 镜像平时字是透明的（只为量行高），但这根竖线有自己的底色，所以在不在
 * 专注模式都看得见。 */
.ed__ai {
  position: relative;
  display: inline-block;
  width: 0;
  height: 1em;
  vertical-align: text-bottom;
}
.ed__ai::before {
  content: '';
  position: absolute;
  left: -1px;
  top: -0.12em;
  width: 3px;
  height: 1.25em;
  border-radius: 2px;
  background: var(--accent);
  animation: ed-ai-blink 1.1s steps(2, start) infinite;
}
/* 那个小标签。**绝对定位**，同样不占位；贴在竖线右上角，跟着一起走。 */
.ed__ai::after {
  content: attr(data-ai);
  position: absolute;
  left: 4px;
  top: -1.05em;
  padding: 0 4px;
  border-radius: 3px;
  background: var(--accent);
  color: var(--bg);
  font-size: 10px;
  line-height: 1.4;
  font-weight: 600;
  letter-spacing: 0.04em;
  white-space: nowrap;
}
@keyframes ed-ai-blink {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.35; }
}

/* 选中才出现的那两个按钮 */
.ed__mini {
  position: absolute;
  left: 50%;
  bottom: 44px;
  transform: translateX(-50%);
  z-index: 5;
  display: flex;
  gap: var(--s1);
  padding: 4px;
  background: var(--surface-2);
  border: 1px solid var(--line);
  border-radius: var(--r);
  box-shadow: var(--shadow-2);
}

/* 状态栏 */
.ed__status {
  flex: none;
  display: flex;
  align-items: center;
  gap: var(--s3);
  height: 28px;
  padding: 0 var(--s3);
  border-top: 1px solid var(--line);
  font-size: var(--fs-xs);
  color: var(--text-3);
  white-space: nowrap;
}
.status__item.is-dirty {
  color: var(--warn);
}
/* is-writing 那个类跟着 .list__run 一起没了——状态行上"正在写"现在是
   .status__ai 那一段自己。 */
.status__ai {
  color: var(--accent);
}
.status__ai {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
}
.status__btn {
  padding: 2px 6px;
  border: 0;
  border-radius: var(--r-sm);
  background: transparent;
  color: var(--text-3);
  font-size: var(--fs-xs);
  cursor: pointer;
}
.status__btn:hover {
  color: var(--text);
  background: var(--surface-2);
}
.status__btn.is-on {
  color: var(--accent);
}
.say {
  height: 22px;
  width: 200px;
}

/* ---------- 右：对话 ---------- */
.ed__side {
  flex: none;
  width: 340px;
  display: flex;
  flex-direction: column;
  min-height: 0;
  border-left: 1px solid var(--line);
  background: color-mix(in srgb, var(--surface) 70%, var(--bg));
}
.side__head {
  flex: none;
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s2) var(--s2) var(--s2) var(--s4);
  border-bottom: 1px solid var(--line);
  font-size: var(--fs-sm);
}
.side__body {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
  padding: var(--s3) var(--s4);
  display: grid;
  gap: var(--s3);
  align-content: start;
}
.side__target {
  line-height: 1.6;
}
.side__target button {
  padding: 0;
  border: 0;
  background: transparent;
  color: var(--accent);
  font: inherit;
  cursor: pointer;
}
.side__foot {
  flex: none;
  display: grid;
  gap: var(--s2);
  padding: var(--s3) var(--s4);
  border-top: 1px solid var(--line);
}
.side__ask {
  min-height: 72px;
}
.ai__quote {
  margin: 0;
  padding: var(--s2) var(--s3);
  border-left: 2px solid var(--accent);
  background: var(--accent-soft);
  color: var(--text-2);
  max-height: 8em;
  overflow: auto;
  white-space: pre-wrap;
}
.turn {
  display: flex;
  gap: 6px;
  align-items: baseline;
}
.turn__who {
  color: var(--text-3);
  flex: none;
  min-width: 1.6em;
}
.ai__done {
  display: grid;
  gap: var(--s2);
  border: 1px dashed var(--accent);
  border-radius: var(--r-sm);
  padding: var(--s3);
}

/* ---------- 从这儿开始 / 草稿 ---------- */
.start {
  display: grid;
  gap: var(--s3);
}
.start__premise {
  min-height: 7em;
  font-size: var(--fs-md);
}

/* AI 想的那几个选题。**一列不是一排**：梗概是两三行字，并排三列就要
   截断，而截断之后三个看起来一样长、一样模糊，没法挑。 */
.ideas {
  display: grid;
  gap: var(--s2);
}

.idea {
  display: grid;
  gap: 4px;
  padding: 10px 12px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: transparent;
  color: var(--text);
  text-align: left;
  cursor: pointer;
}

.idea:hover {
  border-color: var(--accent);
}

.idea__t {
  font-size: var(--fs-md);
}

.idea__p {
  color: var(--text-2);
  font-size: 13px;
  line-height: 1.6;
}

.idea__h {
  color: var(--text-3);
  font-size: 12px;
}
.draft {
  display: grid;
  gap: var(--s3);
}
.draft__list {
  display: grid;
  gap: var(--s2);
}
.dch {
  display: flex;
  gap: var(--s3);
  align-items: baseline;
}
.scales {
  display: inline-flex;
  border: 1px solid var(--line);
  border-radius: var(--r-sm);
  overflow: hidden;
}
.scale {
  padding: 5px 12px;
  border: 0;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.scale.is-on {
  background: var(--accent-soft);
  color: var(--accent);
}

/* ---------- 专注 ---------- */
.ed--focus .ed__list {
  display: none;
}

/* ---------- 窄屏 ---------- */
@media (max-width: 1100px) {
  /* 右栏盖上来，不再挤正文——正文已经没得挤了 */
  .ed__side {
    position: absolute;
    right: 0;
    top: 0;
    bottom: 28px;
    z-index: 10;
    box-shadow: var(--shadow-3);
  }
}
</style>
