<script setup>
/**
 * 故事。原稿，也只有原稿。
 *
 * 用户 2026-09-11：**故事这个页面重点在创作，创作就应该更多的是人和 AI 的
 * 互动，人可以选中某一段让 AI 继续修改优化，也可以通过对话形式修改原稿。**
 *
 * 所以这一页现在只有一件事：**看着自己的稿子，改它**。章节表、分集切线、
 * 每集时长、人物地点索引，全搬去「设定」那三格了——那些是在看创作出来的
 * 东西被切成什么样，是另一件事，摆在这儿只会跟写字抢注意力。
 *
 * **正文是一整篇连着读的**，不是一章一个折叠块。小说就是这么读的；折起来
 * 的话你永远看不到第三章接第四章那一下顺不顺，而那正是最该看的地方。
 *
 * 选中 → 说一句 → 它改 → 你看 → 用不用。**改完不直接落库**，摆出来等你
 * 点；也不是每次都从头说起，之前那几轮来回都带着，所以"再短一点"才有
 * 意义。
 *
 * ⚠️ 位置一律换算成 **Unicode 码点**再送给引擎。浏览器给的是 UTF-16 单元，
 * 碰上代理对（生僻字、emoji）会差一个，而差一个的后果是替换的时候切在
 * 半个字上。`[...s].length` 才是码点数。
 */
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'

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
// "把它切成多长一段"（设定，什么时候改都行）。摆在一起的话，改一个会让
// 人以为另一个也跟着变了。
const SCALES = [
  { key: 'short', label: '短篇', hint: '四章左右，一口气讲完' },
  { key: 'medium', label: '中篇', hint: '八章左右' },
  { key: 'long', label: '长篇', hint: '十六章左右，主线能铺开' },
]
const scale = ref('medium')

// ---- 改稿 ----
/** 选中的那一段：{chapter_id, from, to, text}。没选就是 null。 */
const sel = ref(null)
/** 这一段上聊过的来回。换一段就清空——"再短一点"是相对上一版说的。 */
const chat = ref([])
/** AI 改出来还没采用的那一版。 */
const revision = ref(null)
const instruction = ref('')
const msRef = ref(null)

const chapters = computed(() => story.value?.chapters ?? [])
const hasStory = computed(() => chapters.value.length > 0)
const premiseDirty = computed(() => premise.value.trim() !== savedPremise.value)
const writtenCount = computed(
  () => chapters.value.filter((c) => (c.text ?? '').trim()).length,
)
const unwritten = computed(() => chapters.value.length - writtenCount.value)
const totalChars = computed(() =>
  chapters.value.reduce((n, c) => n + [...(c.text ?? '')].length, 0),
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
}

async function load() {
  if (!session.projectPath) {
    story.value = null
    return
  }
  loading.value = true
  try {
    setStory(await api.getStory(session.projectPath))
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  load()
  writer.poll() // 可能是上次离开页面时还在跑的那一轮
  document.addEventListener('selectionchange', onSelect)
})
onUnmounted(() => {
  writer.stop()
  document.removeEventListener('selectionchange', onSelect)
})
watch(() => session.projectPath, load)
watch(
  () => writer.running,
  (now, before) => {
    if (before && !now) load()
  },
)

// ---------------------------------------------------------------------------
// 选中一段
// ---------------------------------------------------------------------------

/** 这个节点属于哪一章。选中跨章时两头会不一样。 */
function chapterOf(node) {
  let el = node instanceof Element ? node : node?.parentElement
  while (el && !el.dataset?.chapter) el = el.parentElement
  return el?.dataset?.chapter ?? ''
}

/**
 * 浏览器给的 UTF-16 偏移换算成**码点**偏移。
 *
 * 中文基本都在 BMP 里，两者相等；但生僻字和 emoji 是代理对，差一个。
 * 差一个的后果不是显示错位，是替换的时候切在半个字上——存进 story.json
 * 的就是一段非法 UTF-8，一路流到提示词和字幕。
 */
function codePoints(text, utf16Offset) {
  return [...text.slice(0, utf16Offset)].length
}

function onSelect() {
  const s = window.getSelection()
  if (!s || s.isCollapsed || !msRef.value) {
    // **不清掉已经选好的那一段。** 点进右边的输入框时浏览器会收掉选区，
    // 收一次就把面板关掉的话，这个功能永远用不成。
    return
  }
  const a = chapterOf(s.anchorNode)
  const b = chapterOf(s.focusNode)
  if (!a || !b) return
  if (a !== b) {
    ui.warn('一次只能改一章里的一段')
    return
  }
  const el = msRef.value.querySelector(`[data-chapter="${a}"]`)
  if (!el || !el.contains(s.anchorNode)) return

  const full = el.textContent ?? ''
  const lo = Math.min(s.anchorOffset, s.focusOffset)
  const hi = Math.max(s.anchorOffset, s.focusOffset)
  // 两头都在同一个文本节点里才算数：正文是一整个文本节点渲染的，
  // 跨节点说明选到了别的东西（标题、按钮），那不是正文。
  if (s.anchorNode !== s.focusNode) return

  const from = codePoints(full, lo)
  const to = codePoints(full, hi)
  if (to - from < 2) return // 手滑点一下不算选中

  const picked = [...full].slice(from, to).join('')
  if (sel.value?.chapter_id === a && sel.value?.from === from && sel.value?.to === to) {
    return
  }
  sel.value = { chapter_id: a, from, to, text: picked }
  // 换了一段就从头聊：上一段的来回套在这一段上只会让它改错方向
  chat.value = []
  revision.value = null
}

function clearSelection() {
  sel.value = null
  chat.value = []
  revision.value = null
  instruction.value = ''
  window.getSelection()?.removeAllRanges()
}

// ---------------------------------------------------------------------------
// 让 AI 改
// ---------------------------------------------------------------------------

async function revise() {
  const want = instruction.value.trim()
  if (!sel.value) return
  if (!want) {
    ui.warn('说一句要改成什么样，比如「这儿太赶了，铺一下情绪」')
    return
  }
  const result = await run(
    () =>
      api.reviseStory({
        project: session.projectPath,
        chapter_id: sel.value.chapter_id,
        from_char: sel.value.from,
        to_char: sel.value.to,
        instruction: want,
        history: chat.value,
      }),
    { key: 'revise' },
  )
  if (!result) return
  chat.value = [
    ...chat.value,
    { role: 'user', text: want },
    { role: 'assistant', text: result.note || '改完了' },
  ]
  revision.value = result
  instruction.value = ''
}

/** 用这一版。**到这一步才落库。** */
async function applyRevision() {
  if (!revision.value) return
  const r = revision.value
  const result = await run(
    () =>
      api.applyRevision({
        project: session.projectPath,
        chapter_id: r.chapter_id,
        from_char: r.from_char,
        to_char: r.to_char,
        text: r.text,
      }),
    { key: 'apply' },
  )
  if (!result) return
  setStory(result)
  ui.ok(`改好了，这一章现在 ${result.chars} 字，分集重算过了`)
  clearSelection()
}

// ---------------------------------------------------------------------------
// 从无到有的三条路
// ---------------------------------------------------------------------------

async function savePremise() {
  if (!premiseDirty.value || !session.projectPath) return
  const result = await run(
    () => api.saveStory({ project: session.projectPath, premise: premise.value.trim() }),
    { key: 'premise', success: '梗概已存下' },
  )
  if (result) setStory(result)
}

/**
 * 写故事。
 *
 * **梗概不是必填的。** 三个入口里只有「我自己有个想法」那条是从手写的
 * 一句话开始的；给几个关键词、或者什么都不给让它来一个，同样正当。
 * 选题本来就是整条流水线上最难从零开始的一步，把它做成硬门槛等于又把人
 * 摁回空白框前面发呆。
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

/**
 * 让 AI 读一遍正文，把人物关系地点提出来。**正文一个字不动。**
 *
 * 粘进来和反推出来的故事都只有正文，不读一遍的话走到「设定」那一步资产库
 * 是空的，再往下分镜指不到任何角色。
 */
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
    setStory(result)
    draft.value = null
  }
}

// ---------------------------------------------------------------------------
// 展开正文
// ---------------------------------------------------------------------------

/** 展开一章的正文。**直接落库**：它只往一个空字段里填东西，没什么会被顶掉。 */
async function writeChapter(chapterId) {
  const result = await run(
    () =>
      api.writeChapter({ project: session.projectPath, chapter_id: chapterId }),
    { key: 'chapter:' + chapterId },
  )
  if (!result) return
  setStory(result)
  ui.ok(`${chapterId} 写了 ${result.chars} 字`)
  await nextTick()
  msRef.value
    ?.querySelector(`[data-chapter="${chapterId}"]`)
    ?.scrollIntoView({ behavior: 'smooth', block: 'start' })
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
          <button class="btn btn--ghost" type="button" @click="draft = null">
            丢弃
          </button>
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
              {{ isBusy('reverse') ? '正在反推…' : `从已有的 ${session.episodes.length} 集反推` }}
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

      <!-- ---- 原稿 ---- -->
      <div v-else-if="hasStory" class="ms" :class="{ 'ms--picked': sel }">
        <div ref="msRef" class="ms__paper">
          <p class="ms__meta tiny dim">
            {{ chapters.length }} 章 · {{ totalChars }} 字 ·
            已展开 {{ writtenCount }} 章
            <template v-if="unwritten">（还有 {{ unwritten }} 章只有梗概）</template>
            · 选中一段就能让 AI 改它
          </p>

          <article v-for="(c, i) in chapters" :key="c.chapter_id" class="ch">
            <h3 class="ch__title">
              <span class="ch__no numeric">{{ i + 1 }}</span>
              {{ c.title }}
            </h3>
            <!-- 正文渲染成**一个文本节点**：选区偏移直接就是这一章里的
                 位置，不用在 DOM 里爬着累加。中间插任何标签都会让偏移
                 算错，而算错的后果是替换时切在半句话上。 -->
            <div v-if="c.text" class="ms__text" :data-chapter="c.chapter_id">{{ c.text }}</div>
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

        <!-- 选中之后才出现。常驻一条空的对话栏是在跟正文抢地方，
             而这一页的正文才是主角。 -->
        <aside v-if="sel" class="ai stack stack--sm">
          <div class="row row--between">
            <b class="ai__head">改这一段</b>
            <button class="btn btn--ghost btn--sm" type="button" @click="clearSelection">
              收起
            </button>
          </div>
          <blockquote class="ai__quote small">{{ sel.text }}</blockquote>
          <p class="tiny dim">
            {{ sel.chapter_id }} 第 {{ sel.from }}–{{ sel.to }} 字，
            共 {{ sel.to - sel.from }} 字。只改这一段，别的一个字不动。
          </p>

          <div v-for="(t, i) in chat" :key="i" class="turn" :class="'turn--' + t.role">
            <span class="turn__who tiny">{{ t.role === 'user' ? '你' : 'AI' }}</span>
            <span class="small">{{ t.text }}</span>
          </div>

          <section v-if="revision" class="ai__draft">
            <div class="tiny dim">改完是这样（还没写进去）</div>
            <div class="ai__new small">{{ revision.text }}</div>
            <div class="row">
              <button
                class="btn btn--primary btn--sm"
                type="button"
                :disabled="isBusy('apply')"
                @click="applyRevision"
              >
                {{ isBusy('apply') ? '写着…' : '用这一版' }}
              </button>
              <button class="btn btn--ghost btn--sm" type="button" @click="revision = null">
                不要
              </button>
            </div>
          </section>

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
/* 原稿占主位，改稿栏在右边。选中之前右边这一条不存在——常驻一条空栏
   是在跟正文抢地方，而这一页的正文才是主角。 */
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

/* 一整篇连着读。行宽卡在 38 个中文字上下——再宽眼睛要回扫，
   而这一页是拿来读的。 */
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
  max-width: 38em;
}
.ch__title {
  font-size: var(--fs-lg);
  margin: 0 0 var(--s3);
}
.ch__no {
  color: var(--text-3);
  margin-right: 8px;
}
.ms__text {
  white-space: pre-wrap;
  line-height: 1.85;
  /* 选中是这一页的主要动作，给它一个明显的底色 */
  cursor: text;
}
.ms__text::selection,
.ms__text ::selection {
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
.ai__draft {
  border: 1px dashed var(--accent);
  border-radius: var(--r-sm);
  padding: var(--s3);
  display: grid;
  gap: var(--s2);
}
.ai__new {
  white-space: pre-wrap;
  max-height: 16em;
  overflow: auto;
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
