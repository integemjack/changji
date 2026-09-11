<script setup>
/**
 * 这一集的剧本。
 *
 * 原来这块在 ScriptView 里——那一页同时是全剧梗概编辑器、整季批量写、
 * 草稿审阅、预告片、剧集列表和单集编辑器，六件事挤在一页，所以没有重点。
 * 全剧那半搬去了故事页，这里只剩「这一集」。
 *
 * 写这一集走的是 /api/script/write。这一集在分集表里有对应的一条时，引擎
 * 自动走故事那条路：内容照着故事的那一段展开，结尾停在给定的钩子上。
 * 回包里的 source 说的就是走了哪条，摆出来给人看——「这一集为什么是这些
 * 内容」全靠它解释。
 */
import { computed, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import ScriptReader from '@/components/ScriptReader.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()
const { run, isBusy } = useAction()

const script = ref('')
const savedScript = ref('')
const loading = ref(false)
const mode = ref('read')
const draft = ref(null)

const dirty = computed(() => script.value !== savedScript.value)
const wordCount = computed(() => script.value.replace(/\s/g, '').length)
const durationS = computed(() => session.episode?.duration_s || 60)

async function load() {
  if (!session.projectPath || !session.episodeId) return
  loading.value = true
  draft.value = null
  try {
    const data = await api.getScript(session.projectPath, session.episodeId)
    script.value = data.script ?? ''
    savedScript.value = script.value
    mode.value = script.value.trim() ? 'read' : 'edit'
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
      api.writeScript({
        project: session.projectPath,
        episode_id: session.episodeId,
        premise: session.project?.premise ?? '',
        duration_s: durationS.value,
      }),
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
  if (done) savedScript.value = script.value
}
</script>

<template>
  <div class="stack">
    <div class="row row--between">
      <span class="tiny dim numeric">
        {{ wordCount }} 字 · 目标 {{ durationS }} 秒
        <span v-if="dirty" class="pill pill--warn">未保存</span>
      </span>
      <div class="row">
        <button
          v-if="script.trim()"
          class="btn btn--ghost btn--sm"
          type="button"
          @click="mode = mode === 'read' ? 'edit' : 'read'"
        >
          {{ mode === 'read' ? '改' : '读' }}
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
          <AppIcon name="sparkle" :size="15" />
          {{ isBusy('write') ? '写着…' : script.trim() ? 'AI 重写' : 'AI 写这一集' }}
        </button>
      </div>
    </div>

    <!-- 草稿：写完先摆出来，点采用才落库 -->
    <section v-if="draft" class="card card--draft">
      <div class="card__head">
        <div>
          <div class="card__title">
            <AppIcon name="sparkle" :size="15" class="inline-icon" />
            写好了，还没存
          </div>
          <div class="card__sub">{{ draft.logline }}</div>
        </div>
        <span class="pill nowrap" :class="draft.fit === '合适' ? 'pill--ok' : 'pill--warn'">
          {{ draft.fit }}
        </span>
      </div>
      <div class="card__body stack stack--sm">
        <!-- 走的哪条路。「这一集为什么是这些内容」靠它解释 -->
        <p class="tiny dim">
          <template v-if="draft.source === 'story'">
            照着故事的
            <b>{{ (draft.chapters ?? []).join('、') }}</b> 展开的<template
              v-if="draft.hook"
            >，结尾停在「{{ draft.hook }}」</template>
          </template>
          <template v-else>
            照着一句梗概续写的——这一集还没挂在故事的分集表上。
          </template>
        </p>
        <ScriptReader :text="draft.script" />
        <div class="row">
          <button
            class="btn btn--primary"
            type="button"
            :disabled="isBusy('adopt')"
            @click="adopt"
          >
            采用
          </button>
          <button class="btn btn--ghost" type="button" @click="draft = null">丢弃</button>
        </div>
      </div>
    </section>

    <div v-if="loading" class="tiny dim">读取中…</div>

    <EmptyState
      v-else-if="!script.trim() && !draft"
      icon="script"
      title="这一集还没有剧本"
      hint="有故事的话，AI 会照着分集表里这一集对应的那一段展开，结尾停在给定的钩子上。"
    />

    <ScriptReader v-else-if="mode === 'read'" :text="script" />

    <textarea
      v-else
      v-model="script"
      class="textarea textarea--script"
      rows="24"
      placeholder="对白一行一句，写成「名字：台词」；动作单独成行。"
    />
  </div>
</template>

<style scoped>
.textarea--script {
  min-height: 28rem;
  line-height: 1.9;
}
</style>
