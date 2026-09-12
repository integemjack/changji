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
import { openJobSocket } from '@/composables/useJobSocket'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'
import { useWriter } from '@/stores/run'

const session = useSession()
const ui = useUi()
const writer = useWriter()
const { run, isBusy } = useAction()
// **存稿走自己的一条线。** useAction 一次只跑一件事，而改一段要跑一分钟——
// 存稿排在它后面的话，这一分钟里敲的字一个都存不下去。
const saver = useAction()

const story = ref(null)
const loading = ref(false)
const draft = ref(null) // AI 写完、还没采用的那一份大纲
const keywords = ref('')
const pasting = ref(false)
const pasted = ref('')
const premise = ref('')
const savedPremise = ref('')

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
  const v = localStorage.getItem(key)
  return v === null ? fallback : v === '1'
}
function remember(key, v) {
  localStorage.setItem(key, v ? '1' : '0')
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
const dirtyIds = computed(() =>
  chapters.value
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
const locked = computed(() => streaming.value?.chapter_id === current.value)
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
  return n ? String(n) : '—'
}
function stateClass(id) {
  if (streaming.value?.chapter_id === id) return 'is-live'
  if (failedText(id)) return 'is-bad'
  if (isDirty(id)) return 'is-dirty'
  return ''
}

function setStory(payload) {
  story.value = payload?.story ?? null
  premise.value = story.value?.premise ?? ''
  savedPremise.value = premise.value.trim()
  if (story.value?.scale) scale.value = story.value.scale
  // **只刷新没改过的那几章。** 引擎重算分集表也会回一份完整故事，照单
  // 全收的话，用户正在打字的那一章会被服务端那份盖掉。
  //
  // 脏的那几章里，手里那份已经和服务端一样的才摘掉——不能整个清空：
  // 存的那一会儿又敲了字的章仍然是脏的，下一次刷新还得护着它。
  for (const c of chapters.value) {
    const id = c.chapter_id
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

async function load() {
  if (!session.projectPath) {
    story.value = null
    return
  }
  loading.value = true
  try {
    for (const k of Object.keys(buf)) delete buf[k]
    dirtySnapshot.clear()
    setStory(await api.getStory(session.projectPath))
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
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
function watchBatch() {
  if (batchSock) return
  batchSock = openJobSocket(
    'write',
    async (msg) => {
      if (msg.type !== 'story_token' || !msg.chapter_id) return
      // **落到它自己那一章上。** 批量是一章一章顺着写的，但消息里带着
      // chapter_id，不靠顺序猜——猜错的话字会长进隔壁那一章。
      //
      // **seq 归零就是重头来。** 批量那条写砸了会自动再要一次（实跑里
      // 第一次就用上了），而重试是从头生成的——照旧往后接的话，编辑器里
      // 会是"写砸的那半截 + 重写的全文"接在一起。跑完 load() 会把它冲掉，
      // 但那之前这一章看着就是坏的。
      buf[msg.chapter_id] =
        msg.seq === 0 ? (msg.text ?? '') : (buf[msg.chapter_id] ?? '') + (msg.text ?? '')
      // at：AI 写到哪个字了。批量是从头往下写，所以就是当前长度。
      streaming.value = {
        chapter_id: msg.chapter_id,
        from: 0,
        at: buf[msg.chapter_id].length,
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
      batchSock = null
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

onMounted(() => {
  load()
  writer.poll() // 可能是上次离开页面时还在跑的那一轮
  watchBatch()
  window.addEventListener('beforeunload', beforeUnload)
  window.addEventListener('keydown', onKey)
  narrowQuery.addEventListener('change', onNarrow)
})
onUnmounted(() => {
  writer.stop()
  batchSock?.close()
  batchSock = null
  sizer?.disconnect()
  for (const t of Object.values(timers)) clearTimeout(t)
  window.removeEventListener('beforeunload', beforeUnload)
  window.removeEventListener('keydown', onKey)
  narrowQuery.removeEventListener('change', onNarrow)
})
watch(() => session.projectPath, load)
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

/** AI 往眼前这一章写字时，把正在长的那一头一直留在视野里。 */
function keepEndVisible(id) {
  if (id !== current.value || !scroller.value) return
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
const timers = {}
function scheduleSave(id, delay = 1500) {
  clearTimeout(timers[id])
  timers[id] = setTimeout(() => saveChapter(id), delay)
}
function flushSave(id) {
  if (!id) return
  clearTimeout(timers[id])
  delete timers[id]
  return saveChapter(id)
}

/**
 * 存这一章。整章当成一个选区，走的就是 AI 改稿那条写回路径。
 *
 * 存的那一会儿又敲了字的话，这一章仍算脏、再排一次；不然 setStory 会拿
 * 服务端那份（旧的）把刚敲的字盖掉。
 */
async function saveChapter(id) {
  const c = chapters.value.find((x) => x.chapter_id === id)
  if (!c) return
  const sent = buf[id] ?? ''
  if (sent === (c.text ?? '')) return
  // AI 正往这一章写：写完那份由引擎落库，这里存的是半截
  if (streaming.value?.chapter_id === id) return
  // 引擎不收空章。清空了就先不存，敲下一个字再说
  if (!sent.trim()) return
  // 上一章还在存，排在后面
  if (saver.busy.value) {
    scheduleSave(id, 400)
    return
  }
  const result = await saver.run(
    () =>
      api.applyRevision({
        project: session.projectPath,
        chapter_id: id,
        from_char: 0,
        to_char: [...(c.text ?? '')].length,
        text: sent,
      }),
    { key: 'save:' + id },
  )
  if (!result) return
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

  const streamId = 'story-' + Math.random().toString(36).slice(2, 10)
  let acc = ''
  let sock = null
  let opened = false

  const paint = async (text) => {
    buf[id] = head + text + tail
    dirtySnapshot.add(id)
    await nextTick()
    fit(boxes[id])
  }

  const finish = () => {
    sock?.close()
    sock = null
    streaming.value = null
  }

  const post = () =>
    api.reviseStory({
      project: session.projectPath,
      chapter_id: id,
      from_char: at.from,
      to_char: at.to,
      instruction: want,
      history: chat.value,
      // 订阅没发出去就别开流：头几个字推出来时没人听，而漏掉的那几个字
      // 不会有任何提示，只是那段话缺了个开头。
      ...(opened ? { stream: streamId } : {}),
    })

  // 先把选中那段清掉，字就从那个位置长出来——这一下就是"开始写了"
  streaming.value = { chapter_id: id, from: at.from }
  pending.value = { chapter_id: id, prev, origin: at, after: null }
  await paint('')

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      (msg) => {
        if (msg.job_id !== streamId) return
        if (msg.type === 'story_token') {
          acc += msg.text ?? ''
          // 改一段是插在中间的，所以是选区起点加上已经流出来的长度
          streaming.value = { chapter_id: id, from: at.from, at: at.from + acc.length }
          paint(acc)
        }
        // done / error 不在这儿收尾：请求本身的返回才是权威的那一份
      },
      () => resolve(),
      () => {
        opened = true
        resolve()
      },
    )
    // 连不上也别卡着。两秒够本机的 WebSocket 握完手了。
    setTimeout(resolve, 2000)
  })

  const result = await run(post, { key: 'revise' })
  finish()
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

  const result = await run(
    () => api.say({ project: session.projectPath, text: picked }),
    { key: 'say' },
  )
  if (!result) return
  if (result.backend === 'estimate') {
    // 估算后端出来的是等长静音。不说的话用户会对着一段没声音的音频
    // 以为是自己音箱坏了。
    ui.warn('还没配上配音模型，念出来的是一段等长静音。去设置页看看')
  } else if (result.truncated) {
    ui.ok(`念了前 ${result.chars} 字（一次最多这么多）`)
  }
  // 加个时间戳绕开浏览器缓存：落点是固定名字，不加的话第二次点还是老那段
  audio.value = mediaUrl(session.projectPath, result.rel) + '&t=' + Date.now()
  await nextTick()
  document.querySelector('audio.say')?.play?.()
}

// ---------------------------------------------------------------------------
// 从无到有的几条路，和「这本书」
// ---------------------------------------------------------------------------

async function savePremise() {
  if (!premiseDirty.value || !session.projectPath) return
  const result = await run(
    () =>
      api.saveStory({ project: session.projectPath, premise: premise.value.trim() }),
    { key: 'premise', success: '梗概已存下' },
  )
  if (result) setStory(result)
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

function emptyOutlineLive() {
  return { premise: '', logline: '', genre: '', tone: '', characters: [], chapters: [] }
}

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

  // 从这一刻起就摆出那块"正在写"的板子，不等第一帧到。
  outlineLive.value = emptyOutlineLive()

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      (msg) => {
        if (msg.job_id !== streamId) return
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

  const started = await run(
    () =>
      api.writeOutline({
        project: session.projectPath,
        premise: premise.value.trim(),
        scale: scale.value,
        keywords: keywords.value.trim(),
        // socket 没开就退回老路：HTTP 一直等到写完。慢，但至少拿得到结果。
        ...(opened ? { stream: streamId, async: true } : {}),
      }),
    { key: 'write' },
  )

  let result = started
  if (started && started.started) {
    const fin = await finished
    result = fin.ok ? fin.result : null
    if (!fin.ok) ui.error(fin.message || '这份大纲没写成')
  }

  sock?.close()
  outlineLive.value = null
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
  const result = await run(
    () => api.importStory({ project: session.projectPath, text: pasted.value }),
    { key: 'import' },
  )
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
  const result = await run(
    () =>
      api.adoptStory({
        project: session.projectPath,
        story: {
          premise: premise.value.trim(),
          scale: scale.value,
          episode_duration_s: 60,
          chapters: [{ chapter_id: 'ch01', title: '第一章', summary: '', text: '' }],
        },
        overwrite: true,
      }),
    { key: 'blank' },
  )
  if (!result) return
  setStory(result)
  await nextTick()
  boxes[current.value]?.focus()
}

/** 加一章。空的，接着写。 */
async function addChapter() {
  const next = chapters.value.map((c) => ({ ...c }))
  const id = 'ch' + String(next.length + 1).padStart(2, '0')
  next.push({ chapter_id: id, title: `第 ${next.length + 1} 章`, summary: '', text: '' })
  const result = await run(
    () =>
      api.adoptStory({
        project: session.projectPath,
        story: { ...story.value, chapters: next },
        overwrite: true,
      }),
    { key: 'addch' },
  )
  if (!result) return
  setStory(result)
  current.value = id
  await nextTick()
  boxes[id]?.focus()
}

/** 老项目：把已经写好的那几集反推成故事骨架。不碰大模型，也不重新分集。 */
async function reverseFromEpisodes() {
  const result = await run(
    () => api.storyFromEpisodes({ project: session.projectPath, overwrite: true }),
    { key: 'reverse' },
  )
  if (!result) return
  setStory(result)
  ui.ok(`反推出 ${result.chapters} 章。接着点左边「提人物」把人物提出来`)
}

/** 让 AI 读一遍正文，把人物关系地点提出来。**正文一个字不动。** */
async function analyzeStory() {
  const result = await run(
    () => api.analyzeStory({ project: session.projectPath }),
    { key: 'analyze' },
  )
  if (result) draft.value = result
}

async function adoptDraft() {
  if (!draft.value) return
  // 重出的大纲会把现在这几章整份换掉。提人物那种草稿正文不变，不用问。
  const replacing =
    hasStory.value &&
    draft.value.story?.chapters?.some((c, i) => c.text !== chapters.value[i]?.text)
  if (replacing && !confirm(`采用会把现在这 ${chapters.value.length} 章整份换掉。确定？`)) return
  const result = await run(
    () =>
      api.adoptStory({
        project: session.projectPath,
        story: draft.value.story,
        overwrite: true,
      }),
    { key: 'adopt', success: '采用了，写进项目了', refresh: true },
  )
  if (result) {
    for (const k of Object.keys(buf)) delete buf[k]
    dirtySnapshot.clear()
    setStory(result)
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

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      async (msg) => {
        if (msg.job_id !== streamId) return
        // job_done / job_error 是"这件活完了"的通用信号（见 job_stream.hpp）。
        // story_token 和 story_error 是这条路独有的：前者是正在长出来的
        // 正文，后者是"把流了一半的字撤掉"——**撤字归撤字，完事归完事**，
        // 砸了的时候两条都会来。
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

  const started = await run(
    () =>
      api.writeChapter({
        project: session.projectPath,
        chapter_id: chapterId,
        overwrite,
        // socket 没开就退回老路：让 HTTP 那个请求一直等到写完。慢，但至少
        // 拿得到结果——没有 socket 的话异步那条根本没地方把结果送回来。
        ...(opened ? { stream: streamId, async: true } : {}),
      }),
    { key: 'chapter:' + chapterId },
  )

  let result = started
  if (started && started.started) {
    // 异步那条：HTTP 只说了"开始了"，真正的结果在 socket 上。
    const fin = await finished
    result = fin.ok ? fin.result : null
    if (!fin.ok) ui.error(fin.message || '这一章没写成')
  }

  sock?.close()
  streaming.value = null
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
            :class="{ 'is-on': !draft && c.chapter_id === current, 'is-ghost': !!draft }"
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

        <!-- 整本书的那几件事。不常用，但要找得到。 -->
        <div v-if="!draft" class="list__foot">
          <template v-if="writer.running">
            <span class="list__run tiny">
              <span class="dot" />
              展开中 {{ writer.state?.done ?? 0 }} / {{ writer.state?.total ?? 0 }}
            </span>
            <button class="btn btn--danger btn--sm" type="button" @click="stopWriting">
              停
            </button>
          </template>
          <button
            v-else-if="unwritten"
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
        <div ref="scroller" class="ed__scroll" @mousedown="onPaperDown">
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
              <button class="btn btn--ghost" type="button" @click="draft = null">丢弃</button>
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
              placeholder="这部剧讲什么？想好了就写一句，比如：深夜便利店，前任推门进来，手里拿着五年前她送的那把伞。&#10;没想好就空着，让 AI 来一个。"
              @blur="savePremise"
            />
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
              <span class="tiny dim">分几集在「设定 · 分集」那儿定</span>
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
                  {{ isBusy('write') || outlineLive ? '正在写…' : '让 AI 重出一份大纲' }}
                </button>
                <span class="tiny dim">出来先是草稿，采用了才会换掉现在这 {{ chapters.length }} 章</span>
              </template>
            </div>

            <!-- 正在长出来的那份大纲。**边写边看**，见 outlineLive。
                 只摆已经有字的那几项：一上来全是空框的话，看着像坏了。 -->
            <div v-if="outlineLive" class="live stack stack--sm">
              <div class="row tiny dim">
                <span class="live__dot" />
                正在写…（先出选题和人物，再一章一章往下列）
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
              <ol v-if="outlineLive.chapters?.length" class="live__chapters">
                <li v-for="(c, i) in outlineLive.chapters" :key="i">
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
              <button
                v-if="!listShown && !ui.focusMode"
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
                    ? '这一章要写的是：' + chapter.summary + '\n\n从这儿开始写，或者点上面「照大纲写这一章」让 AI 先来一版。'
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
              改整章，{{ chars }} 字。在正文里拖选一段就只改那一段。
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
/* 「正在写」那块板子。刻意做得轻：它是过程，不是结果——
   一会儿就被真正的草稿顶掉，做重了反而让人以为已经写完了。 */
.live {
  border: 1px dashed var(--line);
  border-radius: var(--r);
  padding: 0.75rem 0.9rem;
  background: var(--bg-soft);
}
.live__dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--ai, #7c5cff);
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
.list__run {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 4px var(--s2);
  color: var(--accent);
}

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
.status__ai .dot,
.list__run .dot {
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
.status__item.is-writing,
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
