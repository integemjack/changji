<script setup>
/**
 * 故事。一整块编辑器，一次一章。
 *
 * 用户 2026-09-11 两次定方向：
 *
 *   「故事这个页面重点在创作，人可以选中某一段让 AI 继续修改优化，
 *     也可以通过对话形式修改原稿」
 *   「故事页面应该像程序编辑一样，主要突出写作的过程，
 *     **ai 生成也应该在编辑器里面流式插入**」
 *
 * 所以正文**一直是可编辑的**，不是"只读 + 一个手改开关"。看到一个错别字
 * 还要打一句"把这里的'的'改成'地'"，那不叫创作工具。
 *
 * 而 AI 改出来的东西**一个字一个字长在编辑器里那一段的位置上**，不是摆在
 * 右边一个框里等你抄过去。你看到的就是改完的稿子本身，而且看得见它在写；
 * 不满意按「撤销」，整段退回改之前。
 *
 * 字走 WebSocket（`/api/story/revise` 带上 `stream`），请求本身照样在最后
 * 回完整的一份——**那一份才是权威的**：它剥过模型自作主张加的包装
 * （``` 代码块、「修改后：」），而流出来的是原始 token。收尾时拿它覆盖
 * 一次，不然编辑器里会留下一行 ``` 。WebSocket 连不上就退回一次性返回，
 * 少的只是"看着它写"这件事。
 *
 * 2026-09-11 又定细了一层：**章节用下拉框选，编辑器占满，右下角一排按钮
 * （自动生成、对话修改、朗读）。** 于是正文不再是"一整篇连着往下滚"，而是
 * 一次一章——一次一章才谈得上"占满"，十六章连着的话滚动条本身就是干扰。
 * 代价是看不到第三章接第四章那一下顺不顺，换来的是写这一章时眼前没有别的
 * 东西。下拉框里带着每章字数和"未存"，跳过去是一下的事。
 *
 * 右下角那排按钮**浮在正文上**，不占版面：这一页大多数时候是在读和写，
 * 按钮常驻一条的话，稿子就被挤窄了一截。
 *
 * **编辑器用 textarea，不是 contenteditable。** selectionStart/End 直接就是
 * 偏移，不用在 DOM 里爬；而 contenteditable 里每一次输入都可能重排节点，
 * 偏移随时失效——那正是"改到一半突然替换错地方"的来源。
 *
 * ⚠️ 偏移一律换算成 **Unicode 码点**再送给引擎。浏览器给的是 UTF-16 单元，
 * 碰上代理对（生僻字、emoji）差一个，而差一个就切在半个字上。
 */
import { computed, nextTick, onMounted, onUnmounted, reactive, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import StepHeader from '@/components/StepHeader.vue'
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
/** 这一段上聊过的来回。换一段就清空——"再短一点"是相对上一版说的。 */
const chat = ref([])
const instruction = ref('')
/**
 * AI 刚插进去、还没存的那一段。
 *
 * 留着改之前的**整章**正文，「撤销」就是把它放回去——比记住一段区间可靠：
 * 插进去之后用户可能又手动改了两个字，按区间回退会退错地方。
 */
const pending = ref(null)
/** 正在流式写入的那一段，非空时编辑器里那几个字正一个个冒出来。 */
const streaming = ref(null)
/** 当前在看哪一章。空串表示还没挑（进来时自动挑第一章）。 */
const current = ref('')
/** 右下角那排按钮展开的是哪一个面板：'' | 'ai' */
const panel = ref('')
/** 朗读出来的那段音频。 */
const audio = ref(null)
/** setStory 之前记一下哪几章是脏的。存完那一章会从这里拿掉。 */
const dirtySnapshot = new Set()

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
/** 当前这一章在编辑器里的那一份。 */
const body = computed(() => buf[current.value] ?? '')
const currentDirty = computed(
  () => chapter.value != null && body.value !== (chapter.value.text ?? ''),
)

function setStory(payload) {
  story.value = payload?.story ?? null
  premise.value = story.value?.premise ?? ''
  savedPremise.value = premise.value.trim()
  if (story.value?.scale) scale.value = story.value.scale
  // **只刷新没改过的那几章。** 引擎重算分集表也会回一份完整故事，照单
  // 全收的话，用户正在打字的那一章会被服务端那份盖掉。
  for (const c of chapters.value) {
    if (!dirtySnapshot.has(c.chapter_id)) buf[c.chapter_id] = c.text ?? ''
  }
  dirtySnapshot.clear()
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
      streaming.value = { chapter_id: msg.chapter_id, from: 0 }
      // **跟着它翻页。** 一次只看一章，不跟的话批量跑一个多小时，眼前
      // 这一章一个字都不动——"看着它写"就落空了。
      //
      // **手上有没存的改动就不跟**：翻走了那几个字还在 buf 里没丢，但人
      // 会以为自己刚打的东西没了。宁可这一章不跟，也别让人以为丢了稿子。
      //
      // 判据用 dirtySnapshot（人真敲过字）而不是"buf 和落库那份不一样"——
      // 后者在**刚流完上一章**时也成立（那一章 buf 里有字、故事里还没落），
      // 于是跟到第二章就停了。
      if (current.value !== msg.chapter_id && !dirtySnapshot.has(current.value)) {
        current.value = msg.chapter_id
      }
      await nextTick()
      fit(boxes[msg.chapter_id])
    },
    () => {
      batchSock = null
    },
  )
}

onMounted(() => {
  load()
  writer.poll() // 可能是上次离开页面时还在跑的那一轮
  watchBatch()
  window.addEventListener('beforeunload', beforeUnload)
})
onUnmounted(() => {
  writer.stop()
  batchSock?.close()
  batchSock = null
  window.removeEventListener('beforeunload', beforeUnload)
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

/** 高度跟着内容长。编辑器里不该有第二根滚动条。 */
function fit(el) {
  if (!el) return
  el.style.height = 'auto'
  el.style.height = el.scrollHeight + 'px'
}
function fitAll() {
  for (const el of Object.values(boxes)) fit(el)
}

function onInput(id, event) {
  buf[id] = event.target.value
  dirtySnapshot.add(id)
  fit(event.target)
  // 手一动就说明这一版是自己的了，AI 那条"撤销"没有意义了
  pending.value = null
}

/** 浏览器给的 UTF-16 偏移换算成**码点**偏移。 */
function codePoints(text, utf16Offset) {
  return [...text.slice(0, utf16Offset)].length
}
/** 码点偏移换回 UTF-16。插完字要用它把光标放回正确的位置。 */
function utf16At(text, cp) {
  return [...text].slice(0, cp).join('').length
}

function onSelectionChange(id, event) {
  const el = event.target
  const full = el.value ?? ''
  const from = codePoints(full, el.selectionStart)
  const to = codePoints(full, el.selectionEnd)
  if (to - from < 2) {
    // 光标只是移动了一下。**不清掉已经选好的那一段**——不然点进右边的
    // 输入框就把面板关了，这个功能永远用不成。
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
  chat.value = []
  instruction.value = ''
  pending.value = null
  audio.value = null
}
watch(current, clearSelection)

/** 存这一章。整章当成一个选区，走的就是 AI 改稿那条写回路径。 */
async function saveChapter(id) {
  const c = chapters.value.find((x) => x.chapter_id === id)
  if (!c) return
  const body = buf[id] ?? ''
  if (body === (c.text ?? '')) return
  if (!body.trim()) {
    ui.warn('正文不能是空的。真要清掉这一章的话，去大纲那边重写')
    return
  }
  const result = await run(
    () =>
      api.applyRevision({
        project: session.projectPath,
        chapter_id: id,
        from_char: 0,
        to_char: [...(c.text ?? '')].length,
        text: body,
      }),
    { key: 'save:' + id },
  )
  if (!result) return
  dirtySnapshot.delete(id)
  setStory(result)
  ui.ok(`存下了，这一章 ${result.chars} 字，分集重算过了`)
  pending.value = null
}

async function saveAllDirty() {
  for (const id of [...dirtyIds.value]) await saveChapter(id)
}

// ---------------------------------------------------------------------------
// 让 AI 改：改完**直接落到编辑器里那一段的位置上**
// ---------------------------------------------------------------------------

/**
 * 让 AI 改选中那一段。**边生边写进编辑器**。
 *
 * 字走 WebSocket 一个个推过来，请求本身照样在最后回完整的一份——那一份
 * 是剥过包装（``` 代码块、「修改后：」）的，所以收尾时拿它把流出来的那段
 * 覆盖一次，两边才一致。
 *
 * WebSocket 连不上就退回一次性返回：少了"看着它写"这件事，但功能还在。
 */
async function revise() {
  const want = instruction.value.trim()
  if (!sel.value) return
  if (!want) {
    ui.warn('说一句要改成什么样，比如「这儿太赶了，铺一下情绪」')
    return
  }
  const at = { ...sel.value }
  const id = at.chapter_id

  // 改之前那一章的整份，撤销和流式拼接都拿它当底
  const prev = buf[id] ?? ''
  const chars = [...prev]
  const head = chars.slice(0, at.from).join('')
  const tail = chars.slice(at.to).join('')

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
  pending.value = { chapter_id: id, prev, origin: at }
  await paint('')

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      (msg) => {
        if (msg.job_id !== streamId) return
        if (msg.type === 'story_token') {
          acc += msg.text ?? ''
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
  }
}

/** 不要这一版。整章退回改之前——比按区间回退可靠，见 pending 上的注释。 */
function undoRevision() {
  const p = pending.value
  if (!p) return
  buf[p.chapter_id] = p.prev
  sel.value = { ...p.origin }
  pending.value = null
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
// 从无到有的三条路
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
 * 写故事。
 *
 * **梗概不是必填的。** 选题本来就是整条流水线上最难从零开始的一步，把它
 * 做成硬门槛等于又把人摁回空白框前面发呆。
 */
async function writeStory() {
  const result = await run(
    () =>
      api.writeOutline({
        project: session.projectPath,
        premise: premise.value.trim(),
        scale: scale.value,
        keywords: keywords.value.trim(),
      }),
    { key: 'write' },
  )
  if (result) draft.value = result
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
  }
}

/** 老项目：把已经写好的那几集反推成故事骨架。不碰大模型，也不重新分集。 */
async function reverseFromEpisodes() {
  const result = await run(
    () => api.storyFromEpisodes({ project: session.projectPath, overwrite: true }),
    { key: 'reverse' },
  )
  if (!result) return
  setStory(result)
  ui.ok(`反推出 ${result.chapters} 章。接着点「让 AI 读一遍」把人物提出来`)
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

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      async (msg) => {
        if (msg.job_id !== streamId || msg.type !== 'story_token') return
        acc += msg.text ?? ''
        buf[chapterId] = acc
        streaming.value = { chapter_id: chapterId, from: 0 }
        await nextTick()
        fit(boxes[chapterId])
      },
      () => resolve(),
      () => {
        opened = true
        resolve()
      },
    )
    setTimeout(resolve, 2000)
  })

  const result = await run(
    () =>
      api.writeChapter({
        project: session.projectPath,
        chapter_id: chapterId,
        overwrite,
        ...(opened ? { stream: streamId } : {}),
      }),
    { key: 'chapter:' + chapterId },
  )
  sock?.close()
  streaming.value = null
  if (!result) {
    // 写砸了：把流出来那半截清掉，别在稿子里留一段没头没尾的东西。
    // **重写失败要放回原来那份**，不是清空——原来那一章是好好的。
    const was = chapters.value.find((c) => c.chapter_id === chapterId)
    buf[chapterId] = was?.text ?? ''
    return
  }
  // 落库那份才是权威的（解析、守卫、钩子都在那边）
  setStory(result)
  ui.ok(`${chapterId} 写了 ${result.chars} 字`)
  await nextTick()
  boxes[chapterId]?.scrollIntoView({ behavior: 'smooth', block: 'start' })
}

async function writeAllChapters() {
  const started = await run(
    () => api.writeChapters({ project: session.projectPath }),
    { key: 'chapters' },
  )
  if (started) {
    ui.ok(`开始展开 ${started.chapters} 章`)
    writer.start()
  }
}

async function stopWriting() {
  await run(() => api.stopSeries(), { key: 'stopWrite', success: '已停' })
  writer.poll()
}
</script>

<template>
  <div class="stack stack--lg">
    <!-- **有故事的时候整个页头都不要。** 这一页就是个编辑器，不用自我
         介绍「故事 / 原稿，选中一段就能让 AI 改它」；那几行吃掉的正是
         "占满"差的那点高度。控件全并进下面那条章节栏，一行装下。
         还没有故事时留着——那时候人是第一次进来，需要那句话。 -->
    <StepHeader v-if="!hasStory">
      <template #actions>
        <button
          v-if="dirtyIds.length > 1"
          class="btn btn--primary btn--sm"
          type="button"
          @click="saveAllDirty"
        >
          存下改的 {{ dirtyIds.length }} 章
        </button>
        <button
          v-if="hasStory && unwritten && !writer.running"
          class="btn btn--ai btn--sm"
          type="button"
          :disabled="isBusy('chapters')"
          @click="writeAllChapters"
        >
          <AppIcon name="sparkle" :size="14" />
          展开全部 {{ unwritten }} 章
        </button>
        <template v-if="writer.running">
          <span class="pill pill--accent nowrap">
            正在展开 {{ writer.state?.done ?? 0 }} / {{ writer.state?.total ?? 0 }}
          </span>
          <button class="btn btn--danger btn--sm" type="button" @click="stopWriting">
            停
          </button>
        </template>
        <button
          v-if="needsAnalysis"
          class="btn btn--ai btn--sm"
          type="button"
          :disabled="isBusy('analyze')"
          @click="analyzeStory"
        >
          {{ isBusy('analyze') ? '正在读…' : '让 AI 读一遍，提人物' }}
        </button>
      </template>
    </StepHeader>

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="故事挂在项目上。在项目库那条栏里点一个。"
    />

    <template v-else>
      <!-- 草稿。AI 写完先摆出来给人看，点了采用才落库 -->
      <section v-if="draft" class="card card--draft">
        <div class="card__head">
          <div>
            <div class="card__title">{{ draft.story?.logline || '一份新的故事' }}</div>
            <div class="card__sub">
              {{ draft.chapters }} 章 ·
              {{ draft.story?.characters?.length ?? 0 }} 个人 ·
              {{ draft.story?.locations?.length ?? 0 }} 个地方。
              采用之前原来那份一个字不动。
            </div>
          </div>
        </div>
        <div class="card__body stack stack--sm">
          <div v-for="c in draft.story?.chapters ?? []" :key="c.chapter_id" class="dch">
            <b>{{ c.title }}</b>
            <span class="small dim">{{ c.summary }}</span>
          </div>
        </div>
        <div class="card__foot">
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
      </section>

      <!-- 还没有故事：三条入口 -->
      <section v-if="!hasStory && !loading" class="card">
        <div class="card__body stack">
          <textarea
            v-model="premise"
            class="textarea"
            rows="3"
            placeholder="想好了就写一句，比如：深夜便利店，前任推门进来，手里拿着五年前她送的那把伞。&#10;没想好就空着，直接点下面那个按钮让它来一个。"
            @blur="savePremise"
          />
          <input
            v-model="keywords"
            class="input"
            placeholder="想往哪个方向？热点词、题材都行，可留空（比如：重生复仇、破镜重圆）"
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
            <button
              class="btn btn--ai"
              type="button"
              :disabled="isBusy('write')"
              @click="writeStory"
            >
              <AppIcon name="sparkle" :size="15" />
              {{ isBusy('write') ? '正在写…' : '让 AI 写一份' }}
            </button>
            <button class="btn" type="button" @click="pasting = !pasting">
              我有现成的，粘进来
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
            <span class="spacer" />
            <span class="tiny dim">分几集在「设定 · 分集」那儿定</span>
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
      </section>

      <div v-if="loading" class="tiny dim">读取中…</div>

      <!-- ---- 编辑器 ---- -->
      <div v-else-if="hasStory" class="ed">
        <!-- 一条窄头：挑哪一章、这一章多少字、存没存 -->
        <div class="ed__bar">
          <select v-model="current" class="select ed__pick">
            <option v-for="(c, i) in chapters" :key="c.chapter_id" :value="c.chapter_id">
              第 {{ i + 1 }} 章 · {{ c.title }}
              {{ (c.text ?? '').trim() ? `（${[...(buf[c.chapter_id] ?? '')].length} 字）` : '（还没写）' }}
              {{ (buf[c.chapter_id] ?? '') !== (c.text ?? '') ? ' ·未存' : '' }}
              {{ streaming?.chapter_id === c.chapter_id ? ' ·正在写' : '' }}
            </option>
          </select>
          <span class="spacer" />
          <span class="tiny dim nowrap">
            全书 {{ chapters.length }} 章 · {{ totalChars }} 字
          </span>
          <!-- 整本的那几件事并在这一行。它们不常用（写一次故事按一两次），
               但**要找得到**——单独占一条页头只是为了这一两次。 -->
          <button
            v-if="dirtyIds.length > 1"
            class="btn btn--primary btn--sm nowrap"
            type="button"
            @click="saveAllDirty"
          >
            存下改的 {{ dirtyIds.length }} 章
          </button>
          <button
            v-if="unwritten && !writer.running"
            class="btn btn--ai btn--sm nowrap"
            type="button"
            :disabled="isBusy('chapters')"
            @click="writeAllChapters"
          >
            <AppIcon name="sparkle" :size="14" />
            展开全部 {{ unwritten }} 章
          </button>
          <template v-if="writer.running">
            <span class="pill pill--accent nowrap">
              正在展开 {{ writer.state?.done ?? 0 }} / {{ writer.state?.total ?? 0 }}
            </span>
            <button class="btn btn--danger btn--sm" type="button" @click="stopWriting">
              停
            </button>
          </template>
          <button
            v-if="needsAnalysis"
            class="btn btn--ai btn--sm nowrap"
            type="button"
            :disabled="isBusy('analyze')"
            @click="analyzeStory"
          >
            {{ isBusy('analyze') ? '正在读…' : '让 AI 读一遍，提人物' }}
          </button>
          <!-- **没改动就不显示这个按钮。** 一个常年灰着的按钮只是在占地方，
               而"改了没存"这件事要显眼——它显眼靠的是它突然出现。 -->
          <button
            v-if="currentDirty"
            class="btn btn--primary btn--sm nowrap"
            type="button"
            :disabled="isBusy('save:' + current)"
            @click="saveChapter(current)"
          >
            {{ isBusy('save:' + current) ? '存着…' : '存 · Ctrl+S' }}
          </button>
        </div>

        <!-- 正文占满。textarea 而不是 contenteditable：selectionStart/End
             直接就是偏移，不用在 DOM 里爬。 -->
        <div class="ed__paper">
          <textarea
            v-if="chapter && (chapter.text || buf[current])"
            :ref="(el) => (boxes[current] = el)"
            class="ed__area"
            spellcheck="false"
            :value="body"
            @input="onInput(current, $event)"
            @select="onSelectionChange(current, $event)"
            @mouseup="onSelectionChange(current, $event)"
            @keyup="onSelectionChange(current, $event)"
            @blur="saveChapter(current)"
            @keydown.ctrl.s.prevent="saveChapter(current)"
            @keydown.meta.s.prevent="saveChapter(current)"
          />
          <div v-else-if="chapter" class="ed__todo">
            <p class="small dim">{{ chapter.summary }}</p>
            <button
              class="btn btn--ai"
              type="button"
              :disabled="isBusy('chapter:' + current)"
              @click="writeChapter(current)"
            >
              <AppIcon name="sparkle" :size="15" />
              {{ isBusy('chapter:' + current) ? '正在写…' : '自动生成这一章' }}
            </button>
          </div>

          <!-- 右下角那排。**浮在正文上**，不占版面——这一页大多数时候是在
               读和写，按钮常驻一条的话稿子就被挤窄了一截。 -->
          <div class="ed__acts">
            <div v-if="panel === 'ai'" class="ed__panel stack stack--sm">
              <div class="row row--between">
                <b class="ai__head">{{ sel ? '改选中的这一段' : '先选中一段再说' }}</b>
                <button class="btn btn--ghost btn--sm" type="button" @click="panel = ''">
                  收起
                </button>
              </div>
              <template v-if="sel">
                <blockquote class="ai__quote small">{{ sel.text }}</blockquote>
                <p class="tiny dim">
                  第 {{ sel.from }}–{{ sel.to }} 字，共 {{ sel.to - sel.from }} 字。
                  只改这一段，别的一个字不动。
                </p>
                <div v-for="(t, i) in chat" :key="i" class="turn">
                  <span class="turn__who tiny">{{ t.role === 'user' ? '你' : 'AI' }}</span>
                  <span class="small">{{ t.text }}</span>
                </div>
                <div v-if="pending" class="ai__done">
                  <span class="tiny">已经插进稿子里了，还没存</span>
                  <div class="row">
                    <button
                      class="btn btn--primary btn--sm"
                      type="button"
                      :disabled="isBusy('save:' + pending.chapter_id)"
                      @click="saveChapter(pending.chapter_id)"
                    >
                      存下来
                    </button>
                    <button class="btn btn--ghost btn--sm" type="button" @click="undoRevision">
                      撤销
                    </button>
                  </div>
                </div>
                <textarea
                  v-model="instruction"
                  class="textarea textarea--tight"
                  rows="3"
                  :placeholder="
                    chat.length
                      ? '接着说，比如「再短一点」「语气冷一些」'
                      : '要改成什么样？比如「这儿太赶了，铺一下情绪」'
                  "
                  @keydown.ctrl.enter="revise"
                />
                <div class="row">
                  <button
                    class="btn btn--ai btn--sm"
                    type="button"
                    :disabled="isBusy('revise')"
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
              </template>
              <p v-else class="tiny dim">
                在正文里拖选一段，这里就能对着它说话。
              </p>
            </div>

            <audio v-if="audio" :src="audio" class="say" controls />

            <div class="ed__row">
              <!-- **常驻，不看这一章写没写。** 写过的那一章点它是重写，
                   会先问一句——三个按钮固定在那儿，找按钮不用先想"现在是
                   哪种状态"。 -->
              <button
                class="btn btn--ai btn--sm"
                type="button"
                :disabled="!chapter || isBusy('chapter:' + current)"
                :title="body.trim() ? '这一章重写一遍（会先问一句）' : '照大纲把这一章写出来'"
                @click="writeChapter(current, !!body.trim())"
              >
                <AppIcon name="sparkle" :size="14" />
                {{
                  isBusy('chapter:' + current)
                    ? '正在写…'
                    : body.trim()
                      ? '重写整章'
                      : '自动生成'
                }}
              </button>
              <button
                class="btn btn--sm"
                :class="panel === 'ai' ? 'btn--ai' : 'btn--ghost'"
                type="button"
                @click="panel = panel === 'ai' ? '' : 'ai'"
              >
                对话修改
              </button>
              <button
                class="btn btn--ghost btn--sm"
                type="button"
                :disabled="isBusy('say') || !body.trim()"
                :title="'念选中的那一段；没选就从光标往下念'"
                @click="readAloud"
              >
                {{ isBusy('say') ? '念着…' : '朗读' }}
              </button>
            </div>
          </div>
        </div>

        <p v-if="writer.state?.message" class="tiny dim">{{ writer.state.message }}</p>
        <p
          v-for="(w, i) in writer.state?.episodes ?? []"
          :key="i"
          class="tiny"
          :class="w.error ? 'warn-text' : 'dim'"
        >
          {{ w.chapter_id }}
          {{ w.error ? '写砸了：' + w.error : w.title + ' · ' + w.chars + ' 字' }}
        </p>
      </div>
    </template>
  </div>
</template>

<style scoped>
/* 编辑器占满剩下的高度。**一整块**，不是稿纸上放了个输入框。 */
.ed {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
  /* 只减顶栏和边距。页头在这一页整个是不要的（见模板里那一段），
     所以能多吃几行——"占满"就差这几行。 */
  min-height: calc(100vh - 96px);
}
.ed__bar {
  display: flex;
  align-items: center;
  gap: var(--s3);
}
.ed__pick {
  max-width: 26em;
}

.ed__paper {
  position: relative;
  flex: 1;
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--r-md);
  padding: var(--s5) var(--s5) 64px;
  overflow: auto;
}
/* **不是 flex 容器。** 是过：那时右下角那排按钮成了正文的兄弟 flex item，
   把正文挤到左边去，右边空一大片——"占满"当场落空。按钮改成绝对定位挂在
   右下角之后，正文才真的是这块版面的唯一内容。 */
.ed__area {
  display: block;
  width: 100%;
  /* 行宽卡在 38 个中文字上下，再宽眼睛要回扫。**居中**：占满的是版面，
     不是行长——一行拉到一米二没人读得下去。 */
  max-width: 38em;
  margin: 0 auto;
  border: 0;
  padding: 0;
  background: transparent;
  color: inherit;
  font: inherit;
  line-height: 1.9;
  resize: none;
  overflow: hidden;
}
.ed__area:focus {
  outline: none;
}
.ed__area::selection {
  background: var(--accent-soft);
}
.ed__todo {
  margin: auto;
  display: grid;
  gap: var(--s3);
  justify-items: center;
  text-align: center;
  max-width: 32em;
}

/* 右下角那排。**钉在卡片的右下角**，不跟着正文走——正文短的时候它要是
   浮在文字正下方，中间隔着一大片空地，看着像掉队了。 */
.ed__acts {
  position: absolute;
  right: var(--s4);
  bottom: var(--s4);
  display: grid;
  gap: var(--s2);
  justify-items: end;
  pointer-events: none;   /* 空白处不挡正文的选中 */
}
.ed__acts > * {
  pointer-events: auto;
}
.ed__row {
  display: flex;
  gap: var(--s2);
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--r-md);
  padding: 6px;
  box-shadow: 0 2px 12px rgb(0 0 0 / 0.25);
}
.ed__panel {
  width: 320px;
  max-height: 60vh;
  overflow: auto;
  background: var(--surface);
  border: 1px solid var(--accent);
  border-radius: var(--r-md);
  padding: var(--s4);
  box-shadow: 0 2px 12px rgb(0 0 0 / 0.25);
}
.say {
  width: 320px;
  height: 36px;
}

.ai__head {
  color: var(--accent);
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
</style>
