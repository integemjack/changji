<script setup>
/**
 * 故事。一个编辑器，照代码编辑器的骨架排。
 *
 * 用户 2026-09-11 几次定方向，最后一次是：
 *
 *   「重新理解故事页面，重点是写作窗口，其他都是辅助功能」
 *
 * 落成两栏加两行：
 *
 *   章节（左，可收） │ 正文（独占滚动）
 *   AI 栏（底，Ctrl+K 或右下角那颗；一个框，说什么做什么）
 *   状态栏：字数 · 存没存
 *
 * **进来就是稿纸。** 用户 2026-09-17：「根本不需要这 3 个页面，应该直接进入
 * 编辑框，右下角有个 ai 按钮……通过对话形式让 ai 写内容和修改内容，思考实时
 * 显示在输入框上面固定一行显示，就像 vscode 中修改程序一样」；同一天又说
 * 「所有让 ai 做的都只是这一个章的内容」。之前是三屏（起手屏：梗概框、关键
 * 词框、体量、四条路；草稿屏：采用 / 丢弃；「这本书」屏）加七颗散着的 AI
 * 按钮，还有写大纲 / 想方向 / 写这一章 / 改一段这些各自的路。现在一章都没
 * 有就自动建一章空的，AI 只有一条路：对着眼前这一章说一句——选中了就改选
 * 中的，没选就是整章，章还是空的就是从头写。
 *
 * **正文那一格是唯一的滚动容器。** 页面本身不滚（外壳按 `wide` 关掉了
 * 滚动）。之前是页面在滚、稿纸在滚、textarea 又靠 JS 撑高，三层抢同一个
 * 高度——fit() 在版面没排好时量了一次，一章 343 字被算成 8803px，右下角
 * 那排按钮在第一屏往下 8000px 的地方。现在高度由 flex 定，textarea 还是
 * 跟内容长，但只在这一格里长，而且宽度一变就重量（ResizeObserver）。
 *
 * **没有东西浮在字上面。** AI 栏停靠在稿纸底下，打开时稿纸让位。右下角
 * 那颗按钮和选中一段才浮出来的那两个小按钮是仅有的例外，它们贴在这一格
 * 的边上，不在字上。
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
import { openJobFeed } from '@/composables/useJobFeed'
import { useRetryWhenBack } from '@/composables/useSystemFeed'
import { stoppedByHand } from '@/composables/stopped-by-hand'
import { openJobSocket } from '@/composables/useJobSocket'
import { useThinking } from '@/stores/thinking'
import { readLocal, writeLocal } from '@/composables/local-storage'
import { pickProjectHint } from '@/composables/pick-project-hint'
import { needsFlowReread, storyIsDone } from '@/composables/story-done'
import { useProjects } from '@/stores/projects'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'
import { useWriter } from '@/stores/run'

const session = useSession()
/** 只为那一屏「还没选项目」的提示：一个都没有时该说的是「建一个」。 */
const projects = useProjects()
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
/** 梗概。只读——它是出大纲那一趟说的那句话，落在 story.json 里。 */
const premise = computed(() => story.value?.premise ?? '')

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
/** 章头下面那行大纲折没折。 */
const summaryOpen = ref(remembered('changji.story.summary', true))
watch(summaryOpen, (v) => remember('changji.story.summary', v))
/** 章头右边那个「…」菜单。 */
const menuOpen = ref(false)
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

/**
 * 故事从"空"变成"有字"（或者反过来）→ 重读一趟流程。
 *
 * **不重读的后果**：顶栏那条导航里「设定」是按 `session.done.story` 显示的
 * （App.vue 的 visibleSteps），而 `session.refresh()` 只在挂载时、换项目 /
 * 换集时、以及带 `refresh: true` 的动作之后才跑。新建一部剧写完第一章，
 * 故事页自己是对的——可 done.story 还停在上一次那个 false，于是**「设定」
 * 那一格不出现，非得刷新一次页面才看得见**。用户 2026-09-18 报的就是它。
 *
 * **为什么挂在这儿，而不是各条路上各补一句。** 这一页改故事的路有六条：
 * 敲字（防抖存稿）、AI 写眼前这一章、批量展开、删章、直接开写、反推。
 * 其中删章 / 直接开写 / 反推三条早就带着 `refresh: true`，剩下三条漏了
 * ——漏掉的恰恰是最常走的那几条。一条一条补的话，下次再加一条写故事的
 * 路还会漏。`setStory` 是服务端那份故事进这一页的**唯一**入口，六条路
 * 最后都落在它上面，所以只盯它算出来的这一个值。
 *
 * 判据和"要不要问"都在 story-done.js 里，那儿有测试盯着。
 */
const storyDone = computed(() => storyIsDone(chapters.value))

watch(storyDone, (now) => {
  if (needsFlowReread(now, session.done.story)) session.refresh()
})

const writtenCount = computed(
  () => chapters.value.filter((c) => (c.text ?? '').trim()).length,
)
/**
 * 能「展开」的：有大纲、还没正文的那几章。**不是"没正文的"**——刚建的
 * 那一章空的连大纲都没有，照大纲写等于照空气写，那颗按钮不该亮。
 */
const expandable = computed(
  () => chapters.value.filter((c) => !(c.text ?? '').trim() && (c.summary ?? '').trim()).length,
)
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
/**
 * 老项目：有老流程写好的剧集（带简介、不是从章节同步出来的），故事还没字。
 * 给它一条接回新流程的路。刚建的那一章同步出来的那一集不算——它有
 * chapter_refs。
 */
const canReverse = computed(
  () =>
    writtenCount.value === 0 &&
    session.episodes.some((e) => !(e.chapter_refs?.length) && (e.synopsis ?? '').trim()),
)

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
  // **带上归属再交给 setStory。**
  //
  // 这一趟的三个调用方都在调之前确认过"还在这部剧上"，但确认完之后还有
  // 一个 GET 的来回——换剧就发生在那个来回里。而 setStory 的归属判断
  // （`forProject !== session.projectPath` 就直接返回）只在**传了**第二个
  // 参数时才生效，这儿原来没传，于是全 setStory 里唯一一条不设防的路。
  //
  // 落地就是 load() 上面那段写的那件事：上一部剧的正文装进这一部的编辑器，
  // 接着敲字触发的自动存用的是**这一部**的路径，等于把上一部的章节内容
  // 写进这一部。最长的那条路是批量写——一跑一个多小时，中间十五次重读，
  // 每一次都是一扇窗。
  const want = session.projectPath
  if (!want) return
  try {
    setStory(await api.getStory(want), want)
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
    // 一章都没有就建一章空的：进来就是稿纸。后台正写着大纲的话别建——
    // 写完那份会把它顶掉，而且这时候页面锁着，建了也写不了。
    if (!chapters.value.length) await ensureChapter()
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


// 引擎重启之后自己回来：这一页停在「读不到…」上时，那份表一回来就重读一趟。
// 见 useRetryWhenBack。
useRetryWhenBack(() => loadError.value, load)

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
    openBar()
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

// **挂载那一趟 await 期间人可能已经走了。** 卸载钩子跑在前面的话，
// 后面才挂上的监听和轮询没人收——beforeunload 在别的页上继续拦、Ctrl+K
// 继续翻面板、socket 和 5 秒重试永远活着。所以监听先挂，await 回来再看
// 这一面还在不在。
let alive = true
onMounted(async () => {
  window.addEventListener('beforeunload', beforeUnload)
  window.addEventListener('keydown', onKey)
  narrowQuery.addEventListener('change', onNarrow)
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
  if (!alive) return
  if (writer.running) writer.start()
  watchBatch()
})
onUnmounted(() => {
  alive = false
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
  // **这几样都是"上一部剧的"，得跟着走。** load() 只清 buf 和
  // dirtySnapshot，下面这些原来一直留着，而它们都认章号——两部剧里都有
  // ch01，于是全落在新这一部头上：
  //
  //   · sel / chat：改稿的选区是字符偏移，套到另一部剧的同名章上就是改错
  //     地方；旁边那串对话也还是上一部的。
  //   · streaming / pending：上一部那章的"正在写"锁和撤销底稿。锁尤其难受
  //     ——批量还在上一部跑着（writer.running 是全局的一个槽），新这一部的
  //     同名章会被锁成不能编辑，而这一部根本没人在写它。
  //   · instruction：输入框里那半句话说的是上一部的事。
  //   · audio：念出来那段音频落在上一部的目录里（URL 里钉着它的路径），
  //     换了剧还挂在状态条上，按播放放的是上一部的声音。
  sel.value = null
  chat.value = []
  streaming.value = null
  pending.value = null
  instruction.value = ''
  audio.value = null
  load()
})
watch(
  () => writer.running,
  (now, before) => {
    if (before && !now) {
      // 流出来的是原始 token，落库那份解析过、过了守卫、算过钩子。
      // 整个重读一遍，以它为准。
      //
      // **先把排着的存冲出去。** 批量跑着的时候只锁正在流的那一章，别的章
      // 人照样能改；load() 一上来就把 buf 整个清掉，1.5 秒那次自动存接着
      // 读到的是空串、直接跳过，那几个字就没了。flushAll 同步读走 buf 再
      // 发请求，赶在清空前面。
      streaming.value = null
      flushAll()
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
  if (!el || !chapter.value) return
  const t = e.target
  if (t === el) return
  // 章头那一行的空当也算纸；按钮、下拉、大纲那句（点它是折叠）、行号列不算
  if (t.closest('button, select, textarea, input, a, .doc__sum, .ed__mini, .ed__bar, .ed__fab, .ed__gutter')) return
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

function openBar() {
  barOpen.value = true
  liveSel.value = false
  nextTick(() => askBox.value?.focus())
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
    ui.warn('说一句要它干什么，比如「这儿太赶了，铺一下情绪」')
    return
  }
  // 选中了就改那一段；没选就是整章；章还是空的就是 [0, 0) ——引擎认这个
  // 空区间，走"写"那套提示词（见 stages/story_revise 的 rules_insert）。
  const at = target.value
    ? { ...target.value }
    : { chapter_id: current.value, from: 0, to: chars.value, text: body.value }
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
  activeStream.value = streamId
  let acc = ''
  let feed = null
  let live = false

  const paint = async (text) => {
    if (!mine()) return // 换剧了，别往新这一部的编辑器上画
    buf[id] = head + text + tail
    dirtySnapshot.add(id)
    await nextTick()
    fit(boxes[id])
  }

  const finish = () => {
    thinking.finish(streamId)
    if (activeStream.value === streamId) activeStream.value = null
    feed?.close()
    feed = null
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
      // 回传通道没接上就别开流：头几个字推出来时没人听，而漏掉的那几个字
      // 不会有任何提示，只是那段话缺了个开头。**也别走异步**：结果没地方
      // 送回来。socket 连不上还有信箱轮询那条，见 useJobFeed。
      ...(live ? { stream: streamId, async: true } : {}),
    })

  // 先把选中那段清掉，字就从那个位置长出来——这一下就是"开始写了"
  streaming.value = { chapter_id: id, from: at.from }
  stuck.value = true   // 理由同 writeChapter
  pending.value = { chapter_id: id, prev, origin: at, after: null }
  await paint('')

  /**
   * 这一趟是**连接断了**，不是改砸了。
   *
   * ⚠️ **声明必须在 openJobFeed 之前**：那个 onLost 可能在 await 里就叫，
   * 摆在后面踩的是 TDZ，而且只在"刚好那一刻断了"才现。
   *
   * `openJobFeed` 的 onLost 只在"我们跟丢了"时叫：socket 掉了、信箱取不到、
   * 引擎说完了而收尾那条没看见。这几种的共同点是**那头干没干完不知道**，
   * 和"引擎明说改砸了"是两回事，而原来它俩走的是同一条收尾。
   */
  let lostLink = false

  feed = await openJobFeed(
    streamId,
    (msg) => {
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
    // 这条路断了：把等的人放出来，否则这一段会永远显示"改着…"
    // **记一笔它是"断了"而不是"改砸了"**——底下那两条收尾完全不一样。
    (message) => {
      lostLink = true
      settle?.({ ok: false, message })
    },
    { lost: '和引擎的连接断了，这一段改没改完不好说' },
  )
  live = feed.mode !== 'none'
  // 顶栏那块「正在思考」。**开在这儿、清在 finally 里**——这三步都有
  // 好几条提前 return 的路，手动清总会漏一条，而漏掉的后果是顶栏
  // 永远显示在想。**轮询那条也要开**：没有它就没有「停下」可按。
  if (live) thinking.start(streamId, '改这一段')

  let result = null
  /** 这一趟是被人按停的，不是砸了。 */
  let byHand = false
  try {
    const started = await run(post, { key: 'revise' })
    result = started
    if (started && started.started) {
      const fin = await finished
      result = fin.ok ? fin.result : null
      // 自己按的停别再红一次，理由同 writeChapter 那处。
      byHand = !fin.ok && stoppedByHand(fin.message)
      // 断线那条也别在这儿红一次：底下 `lostLink` 那一支说得更清楚，
      // 还带着"该怎么办"。两条一起弹的话，人先看到的是红的那句。
      if (!fin.ok && !byHand && !lostLink) ui.error(fin.message || '这一段没改成')
    }
  } finally {
    // 上面那句注释说的就是这一下。理由同隔壁 useAsyncJob：
    // 它原来摆在直线上，不漏全靠 `run()` 把异常吞了。
    finish()
  }
  if (!mine()) {
    // 人已经在看别的剧了。这一段属于上一部，扔掉——留下只会写错地方。
    ui.warn('中途换了项目，刚才那一段改稿没有留下')
    return
  }
  if (!result) {
    if (byHand && acc.trim()) {
      // **人按停之前改出来的那一段要留着**，理由同 writeChapter 那处。
      // 这一条比那边省事：改稿本来就有「撤销」（pending / Ctrl+Z），
      // 摆上底稿就是成功那条路的收尾，只是少了剥包装那一下。
      pending.value = { chapter_id: id, prev, origin: at, after: buf[id] }
      scheduleSave(id, 800)
      ui.info(`停下了，改出来的 ${[...acc].length} 字留着（不要就按撤销）`)
      return
    }
    if (lostLink && acc.trim()) {
      // **断线不是改砸了。** 上面那句话自己都说"改没改完不好说"，而原来
      // 接着就把改出来的那一段清掉了——人盯着它改了半天，网抖一下全没。
      //
      // 留在编辑器里，但**不存**：那头可能已经改完并落库了，这会儿把半截
      // 存回去等于拿它盖掉完整那份。按停那条敢存，是因为停是确定的
      // ——引擎收到停就不会再写了。
      pending.value = { chapter_id: id, prev, origin: at, after: buf[id] }
      await nextTick()
      fit(boxes[id])
      ui.push(
        'warn',
        `连接断了。改出来的 ${[...acc].length} 字留在这儿了，但没存下去` +
          `——那头可能已经改完了，刷新这一页看引擎那份；要丢就按撤销`,
        12000,
      )
      return
    }
    if (lostLink) {
      // 断了，而且一个字都没流出来。放回原样，但话要说清是"断了"不是
      // "改砸了"——两者该做的事不一样（前者刷新看看，后者重来一次）。
      buf[id] = prev
      pending.value = null
      await nextTick()
      fit(boxes[id])
      ui.warn('连接断了，这一段改没改完不好说。刷新这一页看引擎那份')
      return
    }
    // 改砸了，把清掉的那一段放回去
    buf[id] = prev
    pending.value = null
    await nextTick()
    fit(boxes[id])
    if (byHand) ui.info('停下了，这一段还没改出东西来')
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
  // 写正文那条按停之后也摆底稿，它没有"改的是哪一段"——那时候别动选区。
  if (p.origin) sel.value = { ...p.origin }
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
// 从无到有的几条路
// ---------------------------------------------------------------------------

/**
 * 一章都没有就建一章空的。**进来就是稿纸**，不问梗概、不挑体量、不等 AI。
 * 结构后补：大纲、提人物都从底下那条 AI 栏上说。
 *
 * 建不出来（引擎没起、目录没了）就停在「还没有第一章」那一行，上面有颗
 * 按钮再试——别自动重试，那会在引擎真出事时每次进页面都刷一条红。
 */
async function ensureChapter() {
  if (hasStory.value) return
  const project = session.projectPath
  const result = await run(
    () =>
      api.adoptStory({
        project,
        story: {
          premise: premise.value,
          scale: 'medium',
          episode_duration_s: 60,
          chapters: [{ chapter_id: 'ch01', title: '第一章', summary: '', text: '' }],
        },
        overwrite: true,
      }),
    // **要 refresh。** 这一下从"没有故事"变成"有一章"，而顶栏那一步的对勾
    // 读的是 `/bff/flow` 的 done.story（判据就是章节数），不重拉的话导航上
    // 还说他没写故事。
    { key: 'blank', refresh: true },
  )
  if (!result) return
  setStory(result, project)
}

/** 加一章。空的，接着写。 */
async function addChapter() {
  const project = session.projectPath
  const next = chapters.value.map((c) => ({ ...c }))
  // **编号取"最大的那个 + 1"，不是"有几章 + 1"。** 删掉中间一章之后
  // 数量比最大编号小，按数量算出来的 id 已经有人占着，引擎回 400
  // 「章节 id 重复」，之后每一次「加一章」都撞在同一个 id 上。
  const top = next.reduce((m, c) => {
    const n = Number(/^ch(\d+)$/.exec(c.chapter_id ?? '')?.[1] ?? 0)
    return Number.isFinite(n) && n > m ? n : m
  }, 0)
  const id = 'ch' + String(top + 1).padStart(2, '0')
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
    // 同 ensureChapter：这一下也是从无到有地长出章节，对勾要跟着亮。
    { key: 'reverse', refresh: true },
  )
  if (!result) return
  setStory(result, project)
  ui.ok(`反推出 ${result.chapters} 章。接着点左边「提人物」把人物提出来`)
}

// ---------------------------------------------------------------------------
// 展开正文
// ---------------------------------------------------------------------------

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
  // 连点两下的话第二下什么都不做。**不能靠 run() 自己那道去重闸**：它挡住
  // 之后回的也是 undefined，和"请求砸了"分不出来，而下面正要拿这个回值判。
  if (isBusy('stopWrite')) return
  // 先打招呼再发请求：引擎把「已手动停止」写进 job 级 error，而下一拍的
  // announceFatal 看见 error 就会弹红字——自己按的停不该再红一次。
  writer.markStopped()
  const ok = await run(() => api.stopSeries(), { key: 'stopWrite', success: '已停' })
  // **没停成就把那面旗收回来。** 留着的话，接下来那趟真的炸了的时候
  // （盘满了、引擎半路重启）announceFatal 会拿它当"自己按的停"吃掉，
  // 屏幕上一个字都没有——而这一刻活儿压根没停，还在写。
  if (!ok) writer.markStopped(false)
  writer.poll()
}

// ---------------------------------------------------------------------------
// 底部那条 AI 栏：一个框，对着眼前这一章说话
// ---------------------------------------------------------------------------
//
// 用户 2026-09-17：「右下角有个 ai 按钮，点击按钮再底部悬浮一个输入框和一个
// ↩︎图标，通过对话形式让 ai 写内容和修改内容，思考实时显示在输入框上面固定
// 一行显示，就像 vscode 中修改程序一样」；同一天又说「所有让 ai 做的都只是
// 这一个章的内容」。
//
// 所以这儿没有"这一下会做什么"的标签，也没有写大纲 / 想方向 / 切章节：
// 那些不是这一章的内容。**选中了就改选中的，没选就是整章，章还是空的就是
// 从头写**——三种情况走的是引擎同一条改稿接口，只差区间。

/** 栏开没开。记在这台机器上；默认收着，右下角那颗按钮或 Ctrl+K 打开。 */
const barOpen = ref(remembered('changji.story.bar', false))
watch(barOpen, (v) => remember('changji.story.bar', v))
/** 这一页自己起的、还在跑的那条流。底栏那个「停」按它。 */
const activeStream = ref(null)

/** 栏上正忙：一件没完别接第二件。 */
const busyBar = computed(() => !!streaming.value || isBusy('revise'))

/** 框里那句灰字就说明这一下动哪块字：选中的 / 整章 / 空章从头写。 */
const askPlaceholder = computed(() => {
  if (target.value) return `改选中的 ${target.value.to - target.value.from} 字：要改成什么样？`
  if (!body.value.trim()) return '这一章写什么？说一句，它从头写'
  return chat.value.length ? '接着说，比如「再短一点」' : `改整章（${chars.value} 字）：要改成什么样？只改一段就先选中它`
})

/** 上一件的回话（「改完了」那句）。有新的活在跑就不摆。 */
const lastNote = computed(() => {
  const last = chat.value[chat.value.length - 1]
  return last?.role === 'assistant' ? last.text : ''
})

/** ↩︎。 */
async function send() {
  if (busyBar.value) return
  await revise()
}

/**
 * 右下角那颗：从网上找热点，写眼前这一章。
 *
 * 用户 2026-09-18：「不需要点击右下角的 ai 图标弹出输入框了，而是点击直接
 * 让大语言模型使用 tools 从网上获取热门内容改写成一个完整的故事」，接着定
 * 「这次写的就只是这一章内容」。一条带工具的对话在引擎里跑
 * （stages/story_from_web），写完只换这一章的正文，这一页那个 writer.running
 * 的 watch 会重读。输入框还在——Ctrl+K 或者选中一段浮出来的那颗——那是改
 * 这一章里某一段用的。
 */
/* ------------------------------------------------------------------ *
 * 一键处理：写这一章 → 理解故事 → 拆分镜头
 * ------------------------------------------------------------------ */

/** 右下角那颗角标展开的菜单开着没有。（章头那个「…」菜单是 menuOpen。） */
const fabMenu = ref(false)
/** 一键处理跑到第几步（0 = 没在跑）。菜单和按钮上的字都读它。 */
const chainStep = ref(0)
/** 按了「停」。三步之间靠它断开——引擎只知道停手里这一件。 */
let chainAbort = false

const CHAIN = ['写这一章', '理解故事', '拆分镜头']

/**
 * 等「写」那个槽闲下来。
 *
 * **不是定时器猜**：`seriesStatus` 就是那个槽的实况。抄的是 EpShots 里
 * 那条一条龙（waitWrite），连"引擎打嗝那几拍不算结束"也一样——当成结束
 * 的话下一步会在上一步还跑着的时候发出去，两件事抢同一个槽，后来那件
 * 直接 409。
 */
async function waitSlot(project) {
  for (;;) {
    if (chainAbort || session.projectPath !== project) return false
    let st = null
    try {
      st = await api.seriesStatus()
    } catch {
      await new Promise((r) => setTimeout(r, 2000))
      continue
    }
    if (!st?.running) return true
    await new Promise((r) => setTimeout(r, 1500))
  }
}

/**
 * 三件活串起来跑。**都是大模型的活，而且抢同一个槽**，所以只能一件一件来。
 *
 * 用户 2026-09-18：「一键处理内容是写这一个章节，理解故事，拆分镜头这几个
 * 和大语言模型有关的内容」。
 *
 * **开跑那一刻把项目钉死。** 这一轮十几分钟起，中途在项目库里点了别的剧，
 * 活儿还是替按下去那一部排的——每一步之间都对一次，不对就停手（story 页
 * 别处那几条长活也是这么防的）。
 */
async function oneClick() {
  fabMenu.value = false
  const id = current.value
  const project = session.projectPath
  if (!id || !project) return
  if (
    !confirm(
      `会依次跑：${CHAIN.join(' → ')}。\n` +
        `第一步会换掉这一章现在的 ${chars.value} 字，理解故事会顶掉手改过的设定和每章剧本。\n` +
        `十几分钟起。确定？`,
    )
  ) {
    return
  }
  chainAbort = false
  // 排着的那次自动存要取消：写完落盘的是新正文，那一存会把老的写回去
  clearTimeout(timers[id]?.t)
  delete timers[id]

  const steps = [
    () => api.storyFromWeb({ project, chapter_id: id, overwrite: true }),
    () => api.understandStory({ project, overwrite: true }),
    () => api.planAll({ project, overwrite: false }),
  ]
  try {
    for (let i = 0; i < steps.length; i++) {
      if (chainAbort || session.projectPath !== project) break
      chainStep.value = i + 1
      const started = await run(steps[i], { key: 'chain' })
      // 那一步没发出去（409、参数不对、断线）就到此为止。**不往下走**：
      // 后面两步吃的是前一步的产出，硬跑只会连着报一串看不懂的错。
      if (!started) {
        ui.error(`「${CHAIN[i]}」没能开始，一键处理停在这儿了`)
        break
      }
      writer.start()
      if (!(await waitSlot(project))) break
      // 每一步都会改故事 / 分集 / 分镜，顶栏那几个判据跟着变
      await refreshStory()
      session.refresh()
    }
    if (!chainAbort && session.projectPath === project) {
      ui.ok('一键处理跑完了：' + CHAIN.join(' → '))
    }
  } finally {
    chainStep.value = 0
    chainAbort = false
  }
}

/**
 * 停。**链子跑着的时候必须走这条**：`stopBar` 只让引擎停手里这一件，
 * 而一键处理下一步会照常发出去——人按了停，屏幕上那件确实停了，两秒后
 * 又自己开始跑下一件，看着像按钮坏了。
 */
async function stopNow() {
  if (chainStep.value > 0) chainAbort = true
  await stopBar()
}

async function writeFromWeb() {
  const id = current.value
  if (!id) return
  if (body.value.trim() && !confirm(`会换掉这一章现在的 ${chars.value} 字。确定？`)) return
  // 排着的那次自动存要取消：写完落盘的是新正文，那一存会把老的写回去
  clearTimeout(timers[id]?.t)
  delete timers[id]
  const started = await run(
    () => api.storyFromWeb({ project: session.projectPath, chapter_id: id, overwrite: true }),
    { key: 'fromweb' },
  )
  if (!started) return
  ui.ok('去网上看热点了。写好会直接落进这一章')
  writer.start()
}

/** 停手里这一件。批量那条走它自己的停。 */
async function stopBar() {
  if (writer.running && !activeStream.value) return stopWriting()
  const id = activeStream.value
  if (!id) return
  try {
    await api.cancelJob(id)
  } catch (e) {
    ui.error(e.message || '没停下来')
  }
}

function onAskKey(e) {
  if (e.key === 'Escape') {
    e.preventDefault()
    barOpen.value = false
    boxes[current.value]?.focus()
    return
  }
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault()
    send()
  }
}
/** 输入框跟着内容长，最多五行。 */
function fitAsk(e) {
  const el = e.target
  el.style.height = 'auto'
  el.style.height = Math.min(el.scrollHeight, 180) + 'px'
}

/**
 * 输入框上面固定那一行：AI 此刻在干什么。
 *
 * 只认这一页自己起的那条流（activeStream），外加批量展开那本账；别的页面
 * 起的活归顶栏那个徽标管。
 *
 * **报的是秒数，不是字数。** 思考正文在 store 里只留最后 4000 字（浮层要
 * 看的是"现在在想什么"），拿它的长度当"想了多少"的话 42 秒就封顶不动——
 * 2026-09-17 实测一份大纲想了八分钟，那个数从第 42 秒起一动不动，而它本来
 * 是"没卡死"的唯一证据。秒数永远在涨。
 */
const now = ref(Date.now())
let clock = null
onMounted(() => {
  clock = setInterval(() => {
    now.value = Date.now()
  }, 1000)
})
onUnmounted(() => clearInterval(clock))

function fmtSecs(s) {
  return s < 60 ? `${s} 秒` : `${Math.floor(s / 60)} 分 ${String(s % 60).padStart(2, '0')} 秒`
}

const thinkLine = computed(() => {
  const mine = activeStream.value
    ? (thinking.items.find((it) => it.id === activeStream.value) ?? null)
    : null
  const secs = mine ? Math.max(0, Math.round((now.value - mine.at) / 1000)) : null
  const tail = (mine?.text ?? '').replace(/\s+/g, ' ').trim().slice(-200)
  const st = streaming.value
  if (st && st.src !== 'batch') {
    // 落笔了：看写到第几个字
    const wrote = typeof st.at === 'number' ? st.at - (st.from ?? 0) : 0
    if (wrote > 0) return { label: '写着', secs, tail: `已经写了 ${wrote} 字`, stop: true }
    return { label: '在想', secs, tail, stop: true }
  }
  if (mine) return { label: '在想', secs, tail, stop: true }
  if (writer.running) {
    // 批量那个槽上跑着的活（展开正文、从网上写故事）：账上那句话 + 思考尾巴
    const w = writer.state ?? {}
    const n = w.total > 1 ? ` ${w.done ?? 0}/${w.total}` : ''
    const board = thinking.items[0]?.text ?? ''
    return {
      label: `${w.message || '在写'}${n}`,
      secs: null,
      tail: board.replace(/\s+/g, ' ').trim().slice(-200),
      stop: true,
    }
  }
  return null
})
</script>

<template>
  <div class="ed" :class="{ 'ed--focus': ui.focusMode }">
    <EmptyState
      v-if="!session.hasProject"
      class="ed__center"
      icon="folder"
      tone="warn"
      title="还没选项目"
      :hint="`故事挂在项目上。${pickProjectHint(projects)}。`"
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
           就是进度条。 -->
      <aside v-if="listShown && hasStory" class="ed__list">
        <div class="list__head">
          <div class="list__book">
            <span class="book__t truncate" :title="story?.logline || premise">
              {{ story?.logline || premise || '这本书' }}
            </span>
            <span class="book__s tiny dim">
              {{ chapters.length }} 章 · {{ totalChars }} 字
            </span>
          </div>
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
            v-for="(c, i) in chapters"
            :key="c.chapter_id"
            class="ch"
            :class="{
              'is-on': c.chapter_id === current,
              'is-blank': isBlank(c.chapter_id),
            }"
            type="button"
            :title="failedText(c.chapter_id) ? '写砸了：' + failedText(c.chapter_id) : c.summary || ''"
            @click="pickChapter(c.chapter_id)"
          >
            <span class="ch__n">{{ i + 1 }}</span>
            <span class="ch__t truncate">{{ c.title || '未命名' }}</span>
            <span class="ch__s" :class="stateClass(c.chapter_id)">
              {{ stateText(c.chapter_id) }}
            </span>
          </button>
        </div>

        <!-- 整本书的事：加一章、展开剩下的（老项目带大纲才有）、反推。眼前
             这一章的字不在这儿——那是底下那个输入框的事。跑的时候藏起来。 -->
        <div v-if="!writer.running" class="list__foot">
          <button
            v-if="expandable"
            class="btn btn--ai btn--sm"
            type="button"
            :disabled="isBusy('chapters')"
            @click="writeAllChapters"
          >
            <AppIcon name="sparkle" :size="13" />
            展开剩下 {{ expandable }} 章
          </button>
          <button
            v-if="canReverse"
            class="btn btn--ghost btn--sm"
            type="button"
            :disabled="isBusy('reverse')"
            @click="reverseFromEpisodes"
          >
            {{ isBusy('reverse') ? '正在反推…' : `从已有的 ${session.episodes.length} 集反推` }}
          </button>
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
      </aside>

      <!-- ================= 中：正文 ================= -->
      <section class="ed__main">
        <div
          ref="scroller"
          class="ed__scroll"
          :class="{ 'ed__scroll--bar': barOpen }"
          @mousedown="onPaperDown"
          @scroll.passive="onScroll"
        >
          <!-- 一章。进来就是它：一章都没有时 load() 会先建一章空的。 -->
          <div v-if="chapter" class="doc doc--chapter">
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
              <!-- 走 pickChapter，不直接绑 current：批量写作时人主动挑一章就是
                   "我要看这一章"，得把「跟着翻」关掉，否则下一个 token 又翻走。 -->
              <select
                v-if="!listShown"
                :value="current"
                class="select doc__pick"
                @change="pickChapter($event.target.value)"
              >
                <option v-for="(c, i) in chapters" :key="c.chapter_id" :value="c.chapter_id">
                  {{ chapterLabel(c, i) }} · {{ stateText(c.chapter_id) }}
                </option>
              </select>
              <h1 v-else class="doc__title">{{ chapterLabel(chapter, index) }}</h1>
              <span class="spacer" />

              <!-- **整本书那两件事，窄屏下没有别的入口。**
                   它们长在左栏底下（`.list__foot`），而窄屏（≤1100px）
                   `.ed__list` 整个不渲染——上面那条注释记的是同一件事，
                   当时只把那颗点了没反应的「章节列表」按钮关掉了，这两件
                   事却没给窝。后果是整条流水线走不下去：**「提人物」够不着
                   的话，人物和地点永远是空的**，设定页空着、拆分镜没人可
                   引用，而「反推」「采用大纲」之后那两句提示还在说「接着点
                   左边「提人物」」——左边什么都没有。
                   条件和左栏底下那一组逐字一样，只是多一个 `!listShown`：
                   两边同时出现就是同一件事摆了两遍。 -->
              <template v-if="!listShown && !writer.running">
                <button
                  v-if="expandable"
                  class="btn btn--ai btn--sm"
                  type="button"
                  :disabled="isBusy('chapters')"
                  @click="writeAllChapters"
                >
                  <AppIcon name="sparkle" :size="13" />
                  展开剩下 {{ expandable }} 章
                </button>
              </template>

              <span v-if="locked" class="doc__live"><span class="dot" /> AI 正在写</span>
              <button
                v-if="locked && writer.running"
                class="btn btn--danger btn--sm"
                type="button"
                @click="stopWriting"
              >
                停
              </button>
              <!-- 空章要让 AI 写、写过的要改，都在底下那条 AI 栏上说。
                   「…」里只剩重写整章 / 朗读 / 折大纲 / 删这一章。 -->
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
                placeholder="从这儿开始写。或者按右下角 AI（Ctrl+K），说一句这一章写什么。"
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

          <!-- 一章都建不出来（引擎没起、目录没了）。别自动重试，见 ensureChapter -->
          <EmptyState
            v-else-if="!loading"
            class="ed__center"
            icon="script"
            title="还没有第一章"
            hint="进来本该自动建一章空的，这一下没建成。"
          >
            <button class="btn btn--primary btn--sm" type="button" :disabled="isBusy('blank')" @click="ensureChapter">
              再建一次
            </button>
          </EmptyState>
        </div>

        <!-- 选中一段才浮出来的那两个按钮。贴在这一格底边中间，不在字上。
             栏开着就不用它——栏就是对着选区说话的地方。
             mousedown 拦掉，不然一点按钮输入框就失焦、选区就散了。 -->
        <div v-if="liveSel && target && !locked && !barOpen" class="ed__mini">
          <button
            class="btn btn--ai btn--sm"
            type="button"
            @mousedown.prevent
            @click="openBar"
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

        <!-- 右下角那两颗。
             ✨ ：点一下，让大模型自己上网看热点、写成这一章的正文。
             ⌄  ：展开菜单，里面是「一键处理」——把三件大模型的活串起来跑。
             输入框（改这一章里的某一段）走 Ctrl+K 或者选中一段浮出来的那颗。 -->
        <div v-if="chapter" class="ed__fabs">
          <button
            class="ed__fab"
            :class="{ 'is-on': writer.running }"
            type="button"
            :disabled="writer.running || isBusy('fromweb') || chainStep > 0"
            :title="writer.running ? (writer.state?.message || '正在写…') : '让 AI 上网看热点，写成这一章（Ctrl+K 是改这一章里的一段）'"
            @click="writeFromWeb"
          >
            <AppIcon name="sparkle" :size="16" />
          </button>
          <div class="menu">
            <button
              class="ed__fab ed__fab--more"
              :class="{ 'is-on': chainStep > 0 }"
              type="button"
              :title="chainStep > 0
                ? `一键处理：第 ${chainStep}/3 步 · ${CHAIN[chainStep - 1]}`
                : '更多：一键处理'"
              @click="fabMenu = !fabMenu"
            >
              <AppIcon name="chevron_down" :size="14" />
            </button>
            <template v-if="fabMenu">
              <div class="menu__veil" @click="fabMenu = false" />
              <div class="menu__pop menu__pop--up">
                <!-- **一件一件写清楚，别只写「一键处理」。** 这一按就是十几
                     分钟、而且会顶掉现有的正文和设定，人得先知道要跑什么。 -->
                <button
                  class="menu__item menu__item--tall"
                  type="button"
                  :disabled="writer.running || chainStep > 0 || !chapter"
                  @click="oneClick"
                >
                  <span class="menu__t">一键处理</span>
                  <span class="menu__sub">{{ CHAIN.join(' → ') }}</span>
                </button>
              </div>
            </template>
          </div>
        </div>

        <!-- 从网上写的时候输入框多半是收着的，进度就单独摆那一行。 -->
        <div v-if="!barOpen && (writer.running || chainStep)" class="ed__bar">
          <div class="bar__line is-live">
            <span class="dot" />
            <span v-if="chainStep" class="pill pill--accent tiny nowrap">
              一键处理 {{ chainStep }}/3 · {{ CHAIN[chainStep - 1] }}
            </span>
            <span class="bar__label">{{ thinkLine?.label }}</span>
            <span class="bar__tail truncate" :title="thinkLine?.tail">{{ thinkLine?.tail }}</span>
            <button class="status__btn" type="button" @click="stopNow">停</button>
          </div>
        </div>

        <!-- 底部浮着的那个输入框。用户 2026-09-17：「只要输入框（自适应高度和
             输入框里面的按钮）」。它在干什么的那一行只在真有事时才冒出来。 -->
        <div v-if="barOpen && chapter" class="ed__bar">
          <div v-if="thinkLine || pending || lastNote || chainStep" class="bar__line" :class="{ 'is-live': thinkLine }">
            <!-- **跑到第几步要摆出来。** 三件活各自的进度句子（thinkLine）
                 长得都差不多，只看那一句说不出"这是一键处理的第二步、后面
                 还有一步"，人会以为跑完了就走开。 -->
            <span v-if="chainStep" class="pill pill--accent tiny nowrap">
              一键处理 {{ chainStep }}/3 · {{ CHAIN[chainStep - 1] }}
            </span>
            <template v-if="thinkLine">
              <span class="dot" />
              <span class="bar__label">{{ thinkLine.label }}</span>
              <span v-if="thinkLine.secs != null" class="bar__secs numeric">{{ fmtSecs(thinkLine.secs) }}</span>
              <span class="bar__tail truncate" :title="thinkLine.tail">{{ thinkLine.tail }}</span>
              <button v-if="thinkLine.stop" class="status__btn" type="button" @click="stopNow">停</button>
            </template>
            <template v-else-if="pending">
              <span class="bar__label">已经落进稿子里了</span>
              <button class="status__btn" type="button" @click="pending = null">就这样</button>
              <button class="status__btn" type="button" @click="undoRevision">撤销 · Ctrl+Z</button>
            </template>
            <template v-else>
              <span class="bar__tail truncate">{{ lastNote }}</span>
            </template>
          </div>
          <div class="bar__box">
            <textarea
              ref="askBox"
              v-model="instruction"
              class="bar__ask"
              rows="1"
              :placeholder="askPlaceholder"
              @keydown="onAskKey"
              @input="fitAsk"
            />
            <button
              class="bar__send"
              type="button"
              :disabled="busyBar"
              :title="busyBar ? '正在做…' : '发出去（Enter；Shift+Enter 换行）'"
              @click="send"
            >
              ↩︎
            </button>
          </div>
        </div>

        <!-- 状态栏。一行，永远在。 -->
        <footer v-if="hasStory" class="ed__status">
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
            :class="{ 'is-on': ui.focusMode }"
            type="button"
            :title="ui.focusMode ? '退出专注（Esc）' : '专注：只留稿纸'"
            @click="ui.focusMode = !ui.focusMode"
          >
            专注
          </button>
        </footer>
      </section>

    </template>
  </div>
</template>

<style scoped>
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
/* 左栏顶上那一行：这本书叫什么、多长。原来是颗按钮，点开「这本书」那屏——
   那屏没了（梗概和体量都从底栏说），它就只是一行字。 */
.list__book {
  flex: 1;
  min-width: 0;
  display: grid;
  gap: 1px;
  padding: var(--s1) var(--s2);
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
  /* 窄屏下这一行还要多摆「展开剩下 N 章」「提人物」，挤不下就折行——
     钉死不折的话它们顶出屏幕外，等于还是够不着。 */
  flex-wrap: wrap;
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
  /* **挤的时候要让得动。** `<select>` 的固有宽度按最长那个选项算，而
     flex 子项默认 `min-width: auto`——也就是一步都不让。375px 上这一行
     （章号 182px + 「让 AI 写这一章」126px + 「…」28px）摆不下，溢出的
     那两个落到项目库那条栏底下：**「…」整个够不着**，而重写整章、
     从光标处朗读、删这一章只有那一个入口。
     让它先让位：章号让掉几个字还看得懂，按钮少一截就没法按了。 */
  min-width: 0;
  flex: 0 1 auto;
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
}
.menu__pop {
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
/* 禁用就变灰，别只是调淡。**和「删掉」那种红字尤其要分得开**——
   淡红还是红，看着只是"颜色浅一点的能点的那一项"。 */
.menu__item:disabled {
  color: var(--text-3);
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
  /* 摆不下就横着滚，别让右边那几颗按钮无声消失 */
  overflow-x: auto;
  scrollbar-width: none;
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
  /* 窄屏上这一格不能把「对话」「专注」挤出容器：外层是 overflow: hidden，
     挤出去就是按钮没了。 */
  max-width: 30vw;
  min-width: 0;
}

/* ---------- 底：AI 栏 ---------- */
/* 右下角那颗。栏开着就不渲染。 */
/* 右下角那一组：AI 那颗 + 展开菜单那颗小的。
   **定位挪到容器上**：原来只有一颗，它自己 absolute；两颗还各自 absolute
   的话会叠在同一个点上。 */
.ed__fabs {
  position: absolute;
  right: var(--s4);
  bottom: 44px;
  z-index: 5;
  display: flex;
  align-items: center;
  gap: 6px;
}
.ed__fab {
  width: 36px;
  height: 36px;
  display: grid;
  place-items: center;
  padding: 0;
  border: 1px solid var(--accent-line);
  border-radius: 50%;
  background: var(--accent-soft);
  color: var(--accent);
  box-shadow: var(--shadow-2);
  cursor: pointer;
}
.ed__fab:hover,
.ed__fab.is-on {
  background: var(--accent);
  color: var(--bg);
}
/* 展开菜单那颗做小一号：主角是 ✨ 那颗，这颗是它的附件。 */
.ed__fab--more {
  width: 28px;
  height: 28px;
}
/* 菜单朝上弹——这两颗贴着窗底，朝下弹会掉出可视区。 */
.menu__pop--up {
  top: auto;
  bottom: calc(100% + 6px);
  min-width: 13em;
}
/* 两行的菜单项：第一行是名字，第二行写清楚它到底要跑哪几步。 */
.menu__item--tall {
  display: grid;
  gap: 2px;
  padding: 8px 10px;
}
.menu__t {
  font-weight: 600;
}
.menu__sub {
  font-size: var(--fs-xs);
  color: var(--text-2);
}
/* 浮在稿纸底部、状态栏上面。左边留 16px，右边给右下角那颗按钮留位。
   纸底下垫一段（.ed__scroll--bar），最后几行不会被它盖住。 */
.ed__bar {
  position: absolute;
  left: var(--s4);
  /* 右边那一组现在是 28 + 6 + 36 = 70px，加上 16px 边距和一点空隙。
     还写 64 的话输入框会钻到那两颗按钮底下。 */
  right: 94px;
  bottom: 40px;
  z-index: 6;
  display: grid;
  gap: var(--s1);
  padding: var(--s2);
  border: 1px solid var(--line);
  border-radius: var(--r);
  background: color-mix(in srgb, var(--surface-2) 92%, transparent);
  box-shadow: var(--shadow-3);
  backdrop-filter: blur(6px);
}
.ed__scroll--bar {
  padding-bottom: 200px;
}
/* 固定一行：它在干什么。高度钉死，字多了截，别让这一行跳着长。 */
.bar__line {
  display: flex;
  align-items: center;
  gap: var(--s2);
  height: 20px;
  min-width: 0;
  font-size: var(--fs-xs);
  color: var(--text-3);
  white-space: nowrap;
}
.bar__line.is-live {
  color: var(--accent);
}
.bar__line .dot {
  flex: none;
}
.bar__label {
  flex: none;
}
.bar__secs {
  flex: none;
  font-variant-numeric: tabular-nums;
}
.bar__tail {
  flex: 1;
  min-width: 0;
  color: var(--text-3);
}
/* 框本身：输入框撑满，↩︎ 钉在框里右下角。 */
.bar__box {
  position: relative;
}
.bar__ask {
  display: block;
  width: 100%;
  min-height: 38px;
  max-height: 180px;
  padding: 9px 44px 9px 12px;
  border: 1px solid var(--line);
  border-radius: var(--r-sm);
  background: var(--surface);
  color: var(--text);
  font: inherit;
  font-size: var(--fs-sm);
  line-height: 1.5;
  resize: none;
}
.bar__ask:focus {
  outline: none;
  border-color: var(--accent-line);
}
.bar__send {
  position: absolute;
  right: 6px;
  bottom: 6px;
  width: 30px;
  height: 26px;
  padding: 0;
  border: 0;
  border-radius: var(--r-sm);
  background: var(--accent);
  color: var(--bg);
  font-size: 16px;
  line-height: 1;
  cursor: pointer;
}
.bar__send:disabled {
  opacity: 0.4;
  cursor: default;
}

/* ---------- 专注 ---------- */
.ed--focus .ed__list {
  display: none;
}

</style>
