<script setup>
/**
 * 这一集的剧本。
 *
 * 一集的剧本是四层，前三层来自设定和故事，只有第四层要 AI：
 *
 *   场次头  在哪、什么光、跟着谁、谁在场、停在什么钩子上   ← 分集表压着的那几场
 *   原文    这一集要拍的那段小说                           ← 分集表切出来的 [from, to)
 *   四段    开场钩子 / 冲突推进 / 情绪回报 / 集尾留扣，按秒排
 *   拍子    动作行 + 「名字：台词」                         ← AI 改编出来的
 *
 * **2026-09-12 之前这一页只有第四层。** 一集在设定里切好了 900 字正文，到这儿
 * 看到的是「还没有剧本」加一个「AI 写这一集」——内容在，页面装作没有；而 AI
 * 那一步拿到的十样上下文（人物、关系、前情、这一集的场、原文、钩子）一样都
 * 不给人看。用户的原话：「剧本不是在设定里已经处理获取了嘛？怎么还要 AI 重写？」
 *
 * 写这一集走 /api/script/write。这一集在分集表里有对应的一条时，引擎走故事
 * 那条路：内容照着故事的那一段展开，结尾停在给定的钩子上。回包里的 source
 * 说的就是走了哪条。**按钮的字也跟着 source 走**：照着原文是「改编」，照着
 * 一句梗概才是「写」——「AI 重写」这三个字就是上面那个问题的来源。
 *
 * 预算那一行（对白 x / 171 字 · 约 x / 60 秒）挂在存过的剧本上，不只是草稿。
 * 以前 fit 只在「写好了还没存」那个面板里闪一下，点采用就没了，人带着一个
 * 13 秒的剧本走到镜头页，直到出片才发现。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import ScriptReader from '@/components/ScriptReader.vue'
import { api } from '@/api'
import { countScriptChars } from '@/api/labels'
import { runAsyncJob } from '@/composables/useAsyncJob'
import { useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

const script = ref('')
const savedScript = ref('')
const ctx = ref(null)
const loading = ref(false)
const mode = ref('read')
/**
 * 写出来还没采用的那一份，**按「项目 + 集号」存**。
 *
 * 原来是一个裸 ref，而写一篇要一两分钟：这中间在顶栏换一集，回包落地那
 * 一下 `draft.value = result` 就把**上一集的剧本**贴在新这一集的抽屉里，
 * 而「采用」发的是当前的集号——按下去就是 ep01 的稿子盖掉 ep02 的正文。
 * `load()` 里那句 `draft.value = null` 挡不住它：清空发生在回包之前。
 *
 * 按集号存之后，落地那一下只填它自己那一格；人切回去还能接着看，不用再
 * 等一两分钟。这一份**不落盘**（引擎只存大纲草稿，不存剧本草稿），所以
 * 丢掉就是白跑一趟。
 */
const drafts = ref({})
function setDraft(key, v) {
  const next = { ...drafts.value }
  if (v) next[key] = v
  else delete next[key]
  drafts.value = next
}
/** 当前这一集那一份。可写——模板里「丢弃」和下面几处照旧 `draft = null`。 */
const draft = computed({
  get: () => drafts.value[ctxKey()] ?? null,
  set: (v) => setDraft(ctxKey(), v),
})

/**
 * 手里这份稿子是**哪一集**的。
 *
 * 这一页是手动保存——敲完得自己点「保存」。有三条路能把没存的字弄丢，
 * 而且一声不吭：
 *
 *   · 顶栏换一集：下面那个 watch 直接 load()，把编辑器里的字盖掉；
 *   · 走到别的页（故事 / 设定）：这一格连同「这一集」整个卸掉；
 *   · 刷新或者关标签页。
 *
 * 一集的剧本是几百上千字。故事页那边早就是「先冲再读」（见 StoryView 的
 * flushAll 和它 onUnmounted 上那段），这一页漏了，照它办。
 *
 * **换集之后 session 里的路径和集号已经是新的那一集了**，所以得记着这份
 * 稿子原来是谁的，存的时候用记下的这一份——不然就把 ep01 的剧本写进
 * ep02 了。目标时长一起记：它是从 ctx 推出来的，而 ctx 也会被 load 换掉。
 */
let owner = null

const dirty = computed(() => script.value !== savedScript.value)
/** 「现在在看哪部剧的哪一集」。异步那几趟拿它认自己有没有过期。 */
const ctxKey = () => `${session.projectPath}\u0000${session.episodeId}`
const wordCount = computed(() => countScriptChars(script.value))
// 时长和引擎写剧本时用的同源：有分集表按分集表，没有按这一集自己的
const durationS = computed(
  () => ctx.value?.target_duration_s || session.episode?.target_duration_s || 60,
)
const fromStory = computed(() => ctx.value?.source === 'story')
const scenes = computed(() => ctx.value?.scenes ?? [])
const sourceText = computed(() => ctx.value?.text ?? '')
const sourceChars = computed(() => [...sourceText.value].length)
const chapterNames = computed(() =>
  (ctx.value?.chapters ?? []).map((c) => c.title || c.chapter_id).join('、'),
)
const hasHead = computed(
  () => !!ctx.value && (scenes.value.length > 0 || !!ctx.value.hook || !!chapterNames.value),
)

/** 按钮上的字。照着原文是改编，照着梗概才是写。 */
const writeLabel = computed(() => {
  if (isBusy('write')) return fromStory.value ? '改编中…' : '写着…'
  if (script.value.trim()) return fromStory.value ? '重新改编' : 'AI 重写'
  return fromStory.value ? '改编成剧本' : 'AI 写这一集'
})

async function load() {
  if (!session.projectPath || !session.episodeId) {
    loading.value = false // 理由同镜头墙那处：被顶掉的那趟不会清它
    return
  }
  // **这一趟是给哪一集读的。**
  //
  // 顶栏连着换两集，两趟请求都在路上，回来的顺序不保证——慢的那一趟后落地
  // 就把上一集的剧本装进当前这一集的编辑器里。而 savedScript 也一起被设成
  // 它，于是"有没有改过"显示的是没改过，人接着敲两个字一存，**上一集的
  // 整篇剧本就写进这一集了**。原料那一份（story.json 可能几百 KB）比剧本
  // 本身慢，顺序反过来是真会发生的。
  const want = ctxKey()
  loading.value = true
  // **这儿原来有一句 `draft.value = null`。** 那时候草稿是个裸 ref、装的
  // 是上一集那一份，进来先清是对的。现在草稿按集号存，这一句清的正好是
  // **要进的这一集自己那一份**——在 ep02 写了一篇没采用、去 ep01 看一眼
  // 再回来，那一篇就没了，而它不落盘，等于白跑一两分钟。
  try {
    const [data, context] = await Promise.all([
      api.getScript(session.projectPath, session.episodeId),
      // 原料读不到不该挡住剧本本身：老项目没有 story.json，这一格就是空的
      api.getScriptContext(session.projectPath, session.episodeId).catch(() => null),
    ])
    if (want !== ctxKey()) return
    script.value = data.script ?? ''
    savedScript.value = script.value
    ctx.value = context
    mode.value = 'read'
    owner = {
      project: session.projectPath,
      episode: session.episodeId,
      duration: durationS.value,
    }
  } catch (err) {
    if (want !== ctxKey()) return
    // **读砸了就得把编辑器空出来。**
    //
    // 这儿原来只弹一句错，编辑器一个字不动——而这一趟只在换集/换剧时跑，
    // 于是砸了之后 ep02 的标签下面**摆着 ep01 的整篇剧本**，`savedScript`
    // 也还是它，所以连「未存」都不亮：看上去就是 ep02 本来就有这么一篇。
    // 接着敲两个字按保存，写进去的是 ep02（save 用的是当前的集号）——
    // 上面 owner 那段防的正是这件事，只是防住了乱序那条路，没防住这条。
    //
    // 引擎那头"这一集还没写剧本"回的是 200 加一个空串，不是错；能走到这儿
    // 的是这一集根本不在了（404）、项目读不出来、或者引擎连不上。
    if (owner?.project !== session.projectPath || owner?.episode !== session.episodeId) {
      script.value = ''
      savedScript.value = ''
      ctx.value = null
      owner = null
      mode.value = 'read'
    }
    ui.error(err.message)
  } finally {
    // 也要认一次：过期那一趟的 finally 会在新那趟还读着的时候把转圈关掉。
    if (want === ctxKey()) loading.value = false
  }
}

/**
 * 把手里这份没存的稿子冲出去，落在**它自己那一集**上。
 *
 * 不等它回来：请求发出去闭包就还活着，这一页该卸卸、该换换。
 *
 * 空白不冲。清空编辑器多半是打算重写，没存过就走开不该把原来那篇删掉；
 * 真要清空，「保存」按钮还在那儿。
 */
function flush() {
  if (!owner || !dirty.value) return
  const text = script.value
  if (!text.trim()) return
  const from = owner
  owner = null // 换集和卸载可能接连来，别存两遍
  api
    .saveScript({
      project: from.project,
      episode_id: from.episode,
      script: text,
      duration_s: from.duration,
    })
    .then(() => ui.ok(`${from.episode} 的剧本还没存，先存下了`))
    .catch((err) => ui.error(`${from.episode} 那篇没存进去：${err.message}`))
}

/**
 * 关标签页和刷新只能拦这一下——存是异步的，这儿等不了。
 *
 * 拦三种东西，而后两种是**这一页独有的**：
 *
 *   · 编辑器里改了还没存的字；
 *   · **AI 写着的那一份**（`isBusy('write')`）。活儿在引擎那头，关了标签页
 *     它照样跑完——但跑完的结果只从那条 socket 送回来一次，没人接就没了。
 *   · **写出来还没采用的那一份**（`draft`）。同样只活在这一次回包里。
 *
 * 大纲那条不一样：引擎把它落了盘（save_story_draft），刷新回来还在，所以
 * 故事页不用拦这一类。剧本这条没有落盘的地方——一份剧本要跑一两分钟，
 * 刷新一下就得重跑一遍，而屏幕上什么都不会说。
 */
function beforeUnload(e) {
  const unsaved = dirty.value && script.value.trim()
  const inFlight = isBusy('write')
  // 草稿按集号存着，别只看当前这一格：在 ep01 写了一篇没采用、切到 ep02
  // 再刷新，丢的是 ep01 那一篇。
  if (!unsaved && !inFlight && !Object.keys(drafts.value).length) return
  e.preventDefault()
  e.returnValue = ''
}

onMounted(() => window.addEventListener('beforeunload', beforeUnload))
onUnmounted(() => {
  window.removeEventListener('beforeunload', beforeUnload)
  flush()
})

watch(
  () => [session.projectPath, session.episodeId],
  () => {
    // **先冲再读。** load() 会把编辑器整个盖掉，理由见 owner 上面那段。
    flush()
    load()
  },
  { immediate: true },
)

async function write() {
  // 开工那一刻把四样都钉死。这一趟一两分钟，中途换集的话：请求本身会带
  // 着新集号（`runAsyncJob` 要等 socket 开才发，最多两秒）、梗概和目标时
  // 长也成了新那一集的，而写出来的东西还会落到新那一集的抽屉里。
  const project = session.projectPath
  const episodeId = session.episodeId
  const key = ctxKey()
  const premise = session.project?.premise ?? ''
  const seconds = durationS.value
  const result = await run(
    () =>
      runAsyncJob(
        (extra) =>
          api.writeScript({
            project,
            episode_id: episodeId,
            premise,
            duration_s: seconds,
            ...extra,
          }),
        { prefix: 'script', label: '写剧本' },
      ),
    { key: 'write' },
  )
  if (!result) return
  setDraft(key, result)
  // 人已经走了：别把它画在别的集上，也别当它没发生过——这一份没落盘。
  if (key !== ctxKey()) ui.info(`${episodeId} 的剧本写好了，切回那一集就能看`)
}

async function adopt() {
  const taking = draft.value
  if (!taking) return
  // **整件事钉在它自己那一集上。**
  //
  // 一个来回之间在顶栏换一集：请求本身是同步拼好的（发的是对的那一集），
  // 但落地这几句原来现读——`draft.value` 是按集号取的（见上面那段），换集
  // 之后它是新那一集的那一份、多半是 null，`draft.value.script` 当场抛
  // TypeError；就算不为 null，那也是把**上一集采用的稿子**画进新这一集的
  // 编辑器里，而 savedScript 一起被设成它，"有没有改过"显示没改过——接着
  // 敲两个字一存，整篇就写进这一集了（和 load 那条防的是同一件事）。
  const key = ctxKey()
  const project = session.projectPath
  const episodeId = session.episodeId
  const seconds = durationS.value
  const done = await run(
    () =>
      api.saveScript({
        project,
        episode_id: episodeId,
        script: taking.script,
        duration_s: seconds,
        synopsis: taking.logline,
      }),
    { key: 'adopt', success: '采用了', refresh: true },
  )
  if (!done) return
  setDraft(key, null)
  if (key !== ctxKey()) return   // 人已经走了：那一集自己的编辑器下次读就是新的
  script.value = taking.script
  savedScript.value = script.value
  mode.value = 'read'
  owner = { project, episode: episodeId, duration: seconds }
}

async function save() {
  // 同 adopt：存的是这一集这一份，落地那几句也只能动这一集。
  const key = ctxKey()
  const sent = script.value
  const done = await run(
    () =>
      api.saveScript({
        project: session.projectPath,
        episode_id: session.episodeId,
        script: sent,
        duration_s: durationS.value,
      }),
    { key: 'save', success: '剧本已保存', refresh: true },
  )
  if (!done) return
  // 换集了就别动新这一集的状态：`savedScript` 被设成上一集那份的话，
  // 新这一集会显示成"没改过"，而它可能正改着。
  if (key !== ctxKey()) return
  savedScript.value = sent
  if (sent.trim()) mode.value = 'read'
}
</script>

<template>
  <div class="scr">
    <div class="toolbar">
      <span class="tiny dim numeric">{{ wordCount }} 字 · 目标 {{ durationS }} 秒</span>
      <span v-if="dirty" class="pill pill--warn">未存</span>
      <span class="spacer" />
      <button
        v-if="script.trim() || mode === 'edit'"
        class="btn btn--ghost btn--sm"
        type="button"
        @click="mode = mode === 'read' ? 'edit' : 'read'"
      >
        {{ mode === 'read' ? '改' : '读' }}
      </button>
      <button v-else class="btn btn--ghost btn--sm" type="button" @click="mode = 'edit'">
        手写
      </button>
      <button
        v-if="dirty"
        class="btn btn--primary btn--sm"
        type="button"
        :disabled="isBusy('save')"
        @click="save"
      >
        保存
      </button>
      <button
        class="btn btn--ai btn--sm"
        type="button"
        :disabled="isBusy('write')"
        @click="write"
      >
        <AppIcon name="sparkle" :size="14" />
        {{ writeLabel }}
      </button>
    </div>

    <!-- 场次头：写剧本的依据，来自分集表压着的那几场，不来自 AI。
         **没剧本时摊开，有了剧本折起来**——那时它说的话剧本已经说过了，
         和底下「原文」同一条规矩。 -->
    <details v-if="hasHead" class="head" :open="!script.trim()">
      <summary class="source__sum">
        <span>这一集要拍什么</span>
        <span class="tiny dim numeric">{{ scenes.length }} 场</span>
      </summary>
      <div v-for="(s, i) in scenes" :key="i" class="head__scene">
        <div class="head__where">
          <AppIcon name="scene" :size="14" />
          <strong>{{ s.where || '地点没写' }}</strong>
          <span v-if="s.pov" class="dim">· 跟着{{ s.pov }}走</span>
          <span v-if="s.who" class="dim">· 在场：{{ s.who }}</span>
        </div>
        <div v-if="s.goal || s.obstacle || s.worse" class="head__beat tiny">
          <span v-if="s.goal">要的是 {{ s.goal }}</span>
          <span v-if="s.obstacle">· 拦着的是 {{ s.obstacle }}</span>
          <span v-if="s.worse">· 收场时更糟在 {{ s.worse }}</span>
        </div>
      </div>
      <div class="head__foot tiny dim">
        <span v-if="chapterNames">照 {{ chapterNames }} 展开</span>
        <span v-if="ctx.hook">· 停在「{{ ctx.hook }}」</span>
        <span v-if="!fromStory">这一集不在分集表上，照梗概写</span>
      </div>
    </details>

    <!-- 原文：这一集要拍的那段小说。没剧本时默认展开，人读的就是它 -->
    <details v-if="sourceText" class="source" :open="!script.trim()">
      <summary class="source__sum">
        <span>原文</span>
        <span class="tiny dim numeric">{{ sourceChars }} 字</span>
      </summary>
      <div class="source__body">{{ sourceText }}</div>
    </details>

    <!-- 草稿：写完先摆出来，点采用才落库 -->
    <section v-if="draft" class="sec draft">
      <div class="sec__head">
        <h2 class="sec__t">写好了，还没存</h2>
        <!-- 「够不够」那颗丸子挪到下面阅读器的信息条上了：它旁边就是
             「对白 123 / 171 字」那几个数，裁决挨着依据才读得懂。
             而且**这儿和那儿原来各摆一颗、各算各的**——引擎数的是结构化
             拍子，阅读器是把渲染好的文本猜回来，碰上「字幕：三年后」这种
             带冒号的描写就会打架，同一份稿子一个写偏短一个写合适。 -->
        <span class="tiny dim truncate" :title="draft.logline">{{ draft.logline }}</span>
        <span class="spacer" />
        <button
          class="btn btn--primary btn--sm"
          type="button"
          :disabled="isBusy('adopt')"
          @click="adopt"
        >
          采用
        </button>
        <button class="btn btn--ghost btn--sm" type="button" @click="draft = null">丢弃</button>
      </div>
      <!-- 走的哪条路。「这一集为什么是这些内容」靠它解释 -->
      <p class="tiny dim">
        <template v-if="draft.source === 'story'">
          照 {{ (draft.chapters ?? []).join('、') }} 改编<template v-if="draft.hook">，停在「{{ draft.hook }}」</template>
        </template>
        <template v-else>照梗概续写，这一集不在分集表上</template>
      </p>
      <ScriptReader
        :text="draft.script"
        :target-seconds="durationS"
        :budget-chars="draft.budget_chars ?? 0"
        :fit="draft.fit ?? ''"
      />
    </section>

    <div v-if="loading" class="tiny dim">读取中…</div>

    <EmptyState
      v-else-if="!script.trim() && !draft && mode !== 'edit'"
      icon="script"
      :title="fromStory ? '原文在上面，还没改编成剧本' : '这一集还没有剧本'"
    />

    <ScriptReader
      v-else-if="mode === 'read'"
      :text="script"
      :target-seconds="durationS"
      :budget-chars="ctx?.budget_chars ?? 0"
    />

    <textarea
      v-else
      v-model="script"
      class="textarea textarea--script"
      placeholder="名字：台词。动作单独成行。段头照「【开场钩子 0–5 秒】」这样写"
    />
  </div>
</template>

<style scoped>
.scr {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
}

/* 场次头 */
.head {
  display: flex;
  flex-direction: column;
  gap: var(--s2);
  padding: var(--s3) var(--s4);
  border: 1px solid var(--line);
  border-radius: var(--r);
  background: var(--surface-2);
}
.head__scene {
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.head__where {
  display: flex;
  align-items: center;
  gap: var(--s2);
  flex-wrap: wrap;
  font-size: var(--fs-base);
}
.head__beat {
  display: flex;
  gap: var(--s2);
  flex-wrap: wrap;
  color: var(--text-2);
  padding-left: calc(14px + var(--s2));
}
.head__foot {
  display: flex;
  gap: var(--s2);
  flex-wrap: wrap;
}

/* 原文 */
.source {
  border: 1px solid var(--line);
  border-radius: var(--r);
  background: var(--bg-sunken);
}
.source__sum {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s2) var(--s4);
  cursor: pointer;
  font-size: var(--fs-sm);
  font-weight: 600;
  color: var(--text-2);
  list-style: none;
}
.source__sum::-webkit-details-marker {
  display: none;
}
.source__sum::before {
  content: '▸';
  color: var(--text-3);
}
.source[open] .source__sum::before {
  content: '▾';
}
.source__body {
  padding: var(--s3) var(--s5) var(--s4);
  max-height: 40vh;
  overflow-y: auto;
  white-space: pre-wrap;
  line-height: 1.9;
  font-size: var(--fs-base);
  color: var(--text-2);
  border-top: 1px solid var(--line);
}

.draft {
  border: 1px dashed var(--accent-line);
  border-radius: var(--r);
  padding: var(--s3);
}
.textarea--script {
  min-height: 60vh;
  font-size: 15px;
  line-height: 1.9;
}
</style>
