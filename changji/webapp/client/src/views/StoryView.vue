<script setup>
/**
 * 故事。一个写稿子的编辑器。
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
 * 而 AI 改出来的东西**直接落到编辑器里那一段的位置上**，不是摆在右边一个
 * 框里等你抄过去。你看到的就是改完的稿子本身；不满意按「撤销」，整段退回
 * 改之前。（还差最后一步：现在是一次性插进去，逐字流式插入要引擎那边先
 * 能吐 token，见下一轮。）
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
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
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

onMounted(() => {
  load()
  writer.poll() // 可能是上次离开页面时还在跑的那一轮
  window.addEventListener('beforeunload', beforeUnload)
})
onUnmounted(() => {
  writer.stop()
  window.removeEventListener('beforeunload', beforeUnload)
})
watch(() => session.projectPath, load)
watch(
  () => writer.running,
  (now, before) => {
    if (before && !now) load()
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

function clearSelection() {
  sel.value = null
  chat.value = []
  instruction.value = ''
  pending.value = null
}

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

async function revise() {
  const want = instruction.value.trim()
  if (!sel.value) return
  if (!want) {
    ui.warn('说一句要改成什么样，比如「这儿太赶了，铺一下情绪」')
    return
  }
  const at = { ...sel.value }
  const result = await run(
    () =>
      api.reviseStory({
        project: session.projectPath,
        chapter_id: at.chapter_id,
        from_char: at.from,
        to_char: at.to,
        instruction: want,
        history: chat.value,
      }),
    { key: 'revise' },
  )
  if (!result) return

  // **插进去，不摆在旁边。** 你看到的就是改完的稿子本身。
  const full = buf[at.chapter_id] ?? ''
  const chars = [...full]
  const head = chars.slice(0, at.from).join('')
  const tail = chars.slice(at.to).join('')
  pending.value = { chapter_id: at.chapter_id, prev: full, origin: at }
  buf[at.chapter_id] = head + result.text + tail
  dirtySnapshot.add(at.chapter_id)

  chat.value = [
    ...chat.value,
    { role: 'user', text: want },
    { role: 'assistant', text: result.note || '改完了' },
  ]
  instruction.value = ''

  // 选中刚插进去那一段：接着说"再短一点"时，说的还是这一段
  const to = at.from + [...result.text].length
  sel.value = { chapter_id: at.chapter_id, from: at.from, to, text: result.text }
  await nextTick()
  const el = boxes[at.chapter_id]
  if (el) {
    fit(el)
    el.focus()
    const now = buf[at.chapter_id]
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

/** 展开一章。**直接落库**：它只往一个空字段里填东西，没什么会被顶掉。 */
async function writeChapter(chapterId) {
  const result = await run(
    () => api.writeChapter({ project: session.projectPath, chapter_id: chapterId }),
    { key: 'chapter:' + chapterId },
  )
  if (!result) return
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
    <StepHeader>
      <template #actions>
        <button
          v-if="dirtyIds.length"
          class="btn btn--primary"
          type="button"
          @click="saveAllDirty"
        >
          存下改的 {{ dirtyIds.length }} 章
        </button>
        <button
          v-if="hasStory && unwritten && !writer.running"
          class="btn btn--ai"
          type="button"
          :disabled="isBusy('chapters')"
          @click="writeAllChapters"
        >
          <AppIcon name="sparkle" :size="15" />
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
          class="btn btn--ai"
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
      <div v-else-if="hasStory" class="ms" :class="{ 'ms--picked': sel }">
        <div class="ms__paper">
          <p class="ms__meta tiny dim">
            {{ chapters.length }} 章 · {{ totalChars }} 字
            <template v-if="unwritten">· 还有 {{ unwritten }} 章只有梗概</template>
            · 直接改就行，选中一段能让 AI 改它 · Ctrl+S 存这一章
          </p>

          <article v-for="(c, i) in chapters" :key="c.chapter_id" class="ch">
            <h3 class="ch__title">
              <span class="ch__no numeric">{{ i + 1 }}</span>
              {{ c.title }}
              <span
                v-if="(buf[c.chapter_id] ?? '') !== (c.text ?? '')"
                class="ch__dirty tiny"
                >未存</span
              >
              <span class="spacer" />
              <span class="tiny dim numeric">
                {{ [...(buf[c.chapter_id] ?? '')].length }} 字
              </span>
            </h3>

            <!-- textarea 而不是 contenteditable：selectionStart/End 直接就是
                 偏移，不用在 DOM 里爬；contenteditable 每次输入都可能重排
                 节点，偏移随时失效——那正是"替换错地方"的来源。 -->
            <textarea
              v-if="c.text || buf[c.chapter_id]"
              :ref="(el) => (boxes[c.chapter_id] = el)"
              class="ms__ed"
              spellcheck="false"
              :value="buf[c.chapter_id] ?? ''"
              @input="onInput(c.chapter_id, $event)"
              @select="onSelectionChange(c.chapter_id, $event)"
              @mouseup="onSelectionChange(c.chapter_id, $event)"
              @keyup="onSelectionChange(c.chapter_id, $event)"
              @blur="saveChapter(c.chapter_id)"
              @keydown.ctrl.s.prevent="saveChapter(c.chapter_id)"
              @keydown.meta.s.prevent="saveChapter(c.chapter_id)"
            />
            <div v-else class="ch__todo">
              <p class="small dim">{{ c.summary }}</p>
              <button
                class="btn btn--ai btn--sm"
                type="button"
                :disabled="isBusy('chapter:' + c.chapter_id)"
                @click="writeChapter(c.chapter_id)"
              >
                {{ isBusy('chapter:' + c.chapter_id) ? '写着…' : '展开这一章的正文' }}
              </button>
            </div>
          </article>

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

        <!-- 选中之后才出现。常驻一条空的对话栏是在跟正文抢地方。 -->
        <aside v-if="sel" class="ai stack stack--sm">
          <div class="row row--between">
            <b class="ai__head">改这一段</b>
            <button class="btn btn--ghost btn--sm" type="button" @click="clearSelection">
              收起
            </button>
          </div>
          <blockquote class="ai__quote small">{{ sel.text }}</blockquote>
          <p class="tiny dim">
            {{ sel.chapter_id }} 第 {{ sel.from }}–{{ sel.to }} 字，共
            {{ sel.to - sel.from }} 字。只改这一段，别的一个字不动。
          </p>

          <div v-for="(t, i) in chat" :key="i" class="turn" :class="'turn--' + t.role">
            <span class="turn__who tiny">{{ t.role === 'user' ? '你' : 'AI' }}</span>
            <span class="small">{{ t.text }}</span>
          </div>

          <!-- 改完的东西已经在左边稿子里了，这里只留一个后悔的口子 -->
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
                : '要改成什么样？比如「这儿太赶了，铺一下情绪」「这句对白太书面」'
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
              {{ isBusy('revise') ? '改着…' : chat.length ? '再改一版' : '改' }}
            </button>
            <span class="tiny dim">Ctrl+Enter</span>
          </div>
        </aside>
      </div>
    </template>
  </div>
</template>

<style scoped>
.ms {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: var(--s4);
  align-items: start;
}
.ms--picked {
  grid-template-columns: minmax(0, 1fr) 320px;
}
@media (max-width: 900px) {
  .ms--picked {
    grid-template-columns: minmax(0, 1fr);
  }
}

.ms__paper {
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--r-md);
  padding: var(--s5) var(--s5) var(--s6);
}
.ms__meta {
  margin: 0 0 var(--s4);
}
.ch {
  margin: 0 0 var(--s5);
  max-width: 44em;
}
.ch__title {
  display: flex;
  align-items: baseline;
  gap: 8px;
  font-size: var(--fs-lg);
  margin: 0 0 var(--s3);
}
.ch__no {
  color: var(--text-3);
}
.ch__dirty {
  color: var(--accent);
}

/* 编辑器。**没有边框没有底色**——它就是稿纸本身，不是稿纸上放了个输入框。
   行宽卡在 38 个中文字上下，再宽眼睛要回扫。高度跟着内容长，
   编辑器里不该有第二根滚动条。 */
.ms__ed {
  display: block;
  width: 100%;
  max-width: 38em;
  border: 0;
  padding: 0;
  background: transparent;
  color: inherit;
  font: inherit;
  line-height: 1.85;
  resize: none;
  overflow: hidden;
}
.ms__ed:focus {
  outline: none;
}
.ms__ed::selection {
  background: var(--accent-soft);
}
.ch__todo {
  border-left: 2px solid var(--line);
  padding-left: var(--s3);
}

.ai {
  position: sticky;
  top: var(--s4);
  background: var(--surface);
  border: 1px solid var(--accent);
  border-radius: var(--r-md);
  padding: var(--s4);
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
