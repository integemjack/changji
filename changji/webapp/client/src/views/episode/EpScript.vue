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
import { computed, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import ScriptReader from '@/components/ScriptReader.vue'
import { api } from '@/api'
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
const draft = ref(null)

const dirty = computed(() => script.value !== savedScript.value)
const wordCount = computed(() => script.value.replace(/\s/g, '').length)
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
  if (!session.projectPath || !session.episodeId) return
  loading.value = true
  draft.value = null
  try {
    const [data, context] = await Promise.all([
      api.getScript(session.projectPath, session.episodeId),
      // 原料读不到不该挡住剧本本身：老项目没有 story.json，这一格就是空的
      api.getScriptContext(session.projectPath, session.episodeId).catch(() => null),
    ])
    script.value = data.script ?? ''
    savedScript.value = script.value
    ctx.value = context
    mode.value = 'read'
  } catch (err) {
    ui.error(err.message)
  } finally {
    loading.value = false
  }
}

watch(() => [session.projectPath, session.episodeId], load, { immediate: true })

async function write() {
  const result = await run(
    () =>
      runAsyncJob(
        (extra) =>
          api.writeScript({
            project: session.projectPath,
            episode_id: session.episodeId,
            premise: session.project?.premise ?? '',
            duration_s: durationS.value,
            ...extra,
          }),
        { prefix: 'script', label: '写剧本' },
      ),
    { key: 'write' },
  )
  if (result) draft.value = result
}

async function adopt() {
  if (!draft.value) return
  const done = await run(
    () =>
      api.saveScript({
        project: session.projectPath,
        episode_id: session.episodeId,
        script: draft.value.script,
        duration_s: durationS.value,
        synopsis: draft.value.logline,
      }),
    { key: 'adopt', success: '采用了', refresh: true },
  )
  if (done) {
    script.value = draft.value.script
    savedScript.value = script.value
    draft.value = null
    mode.value = 'read'
  }
}

async function save() {
  const done = await run(
    () =>
      api.saveScript({
        project: session.projectPath,
        episode_id: session.episodeId,
        script: script.value,
        duration_s: durationS.value,
      }),
    { key: 'save', success: '剧本已保存', refresh: true },
  )
  if (done) {
    savedScript.value = script.value
    if (script.value.trim()) mode.value = 'read'
  }
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
        <span class="pill nowrap" :class="draft.fit === '合适' ? 'pill--ok' : 'pill--warn'">
          {{ draft.fit }}
        </span>
        <span class="tiny dim truncate">{{ draft.logline }}</span>
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
