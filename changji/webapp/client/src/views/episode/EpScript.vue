<script setup>
/**
 * 这一集的剧本。
 *
 * 写这一集走的是 /api/script/write。这一集在分集表里有对应的一条时，引擎
 * 自动走故事那条路：内容照着故事的那一段展开，结尾停在给定的钩子上。
 * 回包里的 source 说的就是走了哪条，一行小字摆出来——「这一集为什么是这些
 * 内容」全靠它解释。
 *
 * 2026-09-11 起照故事页的样子：顶上一条工具行，剩下全是剧本。
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
  <div class="scr">
    <div class="toolbar">
      <span class="tiny dim numeric">{{ wordCount }} 字 · 目标 {{ durationS }} 秒</span>
      <span v-if="dirty" class="pill pill--warn">未存</span>
      <span class="spacer" />
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
        <AppIcon name="sparkle" :size="14" />
        {{ isBusy('write') ? '写着…' : script.trim() ? 'AI 重写' : 'AI 写这一集' }}
      </button>
    </div>

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
          照 {{ (draft.chapters ?? []).join('、') }} 展开<template v-if="draft.hook">，停在「{{ draft.hook }}」</template>
        </template>
        <template v-else>照梗概续写，这一集不在分集表上</template>
      </p>
      <ScriptReader :text="draft.script" />
    </section>

    <div v-if="loading" class="tiny dim">读取中…</div>

    <EmptyState v-else-if="!script.trim() && !draft" icon="script" title="这一集还没有剧本" />

    <ScriptReader v-else-if="mode === 'read'" :text="script" />

    <textarea
      v-else
      v-model="script"
      class="textarea textarea--script"
      placeholder="名字：台词。动作单独成行"
    />
  </div>
</template>

<style scoped>
.scr {
  display: flex;
  flex-direction: column;
  gap: var(--s3);
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
