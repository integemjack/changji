<script setup>
/**
 * 加一个项目。弹窗。
 *
 * **两种「加」合在一个弹窗里**：新建一个，或者把磁盘上已有的目录接进来。
 * 对用户是同一件事——「我要多一个项目」，分成两张卡摆在页面上时，
 * 那两张卡常年占着位置而一年点不了两次。
 */
import { computed, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useProjects } from '@/stores/projects'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const props = defineProps({ open: { type: Boolean, default: false } })
const emit = defineEmits(['close', 'added'])

const session = useSession()
const store = useProjects()
const ui = useUi()
const { run, isBusy } = useAction()

const mode = ref('new') // new | existing
const draft = ref({ path: '', title: '', style_line: 'realistic' })
const openPath = ref('')

const workspace = computed(() => store.workspace || '工作目录')

// 每次打开都从头来。留着上次的输入，第二次打开会看见一个填了一半的框，
// 而那多半是上次放弃掉的东西。
watch(
  () => props.open,
  (now) => {
    if (!now) return
    mode.value = 'new'
    draft.value = { path: '', title: '', style_line: 'realistic' }
    openPath.value = ''
  },
)

async function create() {
  const name = draft.value.path.trim()
  if (!name) {
    ui.warn('先给项目起个名字')
    return
  }
  const result = await run(() => api.newProject({ ...draft.value, path: name }), {
    key: 'create',
    success: '项目建好了',
  })
  if (!result) return
  await store.load()
  session.selectProject(result.root)
  emit('added', result.root)
  emit('close')
}

/** 按路径接一个项目库以外的项目。先问引擎认不认，再切过去。 */
async function openExisting() {
  const path = openPath.value.trim()
  if (!path) {
    ui.warn('填一个项目目录的完整路径')
    return
  }
  const info = await run(() => api.project(path), { key: 'open' })
  if (!info) return
  await store.load()
  session.selectProject(info.root)
  ui.ok(`已切到「${info.title || info.project_id}」`)
  emit('added', info.root)
  emit('close')
}
</script>

<template>
  <Teleport to="body">
    <div v-if="open" class="modal" @click.self="emit('close')">
      <section class="card modal__panel">
        <div class="card__head">
          <div>
            <div class="card__title">加一个项目</div>
            <div class="card__sub">新建一个，或者把已经有的目录接进来。</div>
          </div>
          <button class="btn btn--ghost btn--sm" type="button" @click="emit('close')">
            <AppIcon name="close" :size="15" />
          </button>
        </div>

        <div class="card__body stack">
          <div class="modes">
            <button
              class="modes__btn"
              :class="{ 'is-on': mode === 'new' }"
              type="button"
              @click="mode = 'new'"
            >
              新建
            </button>
            <button
              class="modes__btn"
              :class="{ 'is-on': mode === 'existing' }"
              type="button"
              @click="mode = 'existing'"
            >
              打开已有目录
            </button>
          </div>

          <template v-if="mode === 'new'">
            <label class="field">
              <span class="field__label">目录名</span>
              <input
                v-model="draft.path"
                class="input"
                placeholder="例如：深夜便利店"
                @keyup.enter="create"
              />
              <span class="field__hint">
                建在 <span class="mono">{{ workspace }}</span> 下面。
                整个目录拷到别的机器就能接着做。
              </span>
            </label>
            <label class="field">
              <span class="field__label">剧名（可空）</span>
              <input v-model="draft.title" class="input" placeholder="不填就用目录名" />
            </label>
            <label class="field">
              <span class="field__label">画风</span>
              <select v-model="draft.style_line" class="select">
                <option value="realistic">真人写实</option>
                <option value="anime">动漫</option>
              </select>
              <span class="field__hint">
                决定用哪套出图基座。建好之后在项目页还能改。
              </span>
            </label>
          </template>

          <template v-else>
            <label class="field">
              <span class="field__label">项目目录的完整路径</span>
              <input
                v-model="openPath"
                class="input mono"
                placeholder="例如：E:\AI短剧\雨夜天台"
                @keyup.enter="openExisting"
              />
              <span class="field__hint">
                得是一个已经建好的项目目录（里面有 project.json）。
                接进来之后它会出现在右边的项目库里。
              </span>
            </label>
          </template>
        </div>

        <div class="card__foot">
          <button
            v-if="mode === 'new'"
            class="btn btn--primary"
            type="button"
            :disabled="isBusy('create')"
            @click="create"
          >
            {{ isBusy('create') ? '建着…' : '建好并切过去' }}
          </button>
          <button
            v-else
            class="btn btn--primary"
            type="button"
            :disabled="isBusy('open')"
            @click="openExisting"
          >
            {{ isBusy('open') ? '找着…' : '打开并切过去' }}
          </button>
          <button class="btn btn--ghost" type="button" @click="emit('close')">取消</button>
        </div>
      </section>
    </div>
  </Teleport>
</template>

<style scoped>
.modal__panel {
  width: min(30rem, calc(100vw - 2rem));
}
.modes {
  display: flex;
  gap: 2px;
  padding: 2px;
  background: var(--surface-2);
  border: 1px solid var(--line);
  border-radius: 10px;
}
.modes__btn {
  flex: 1;
  padding: 6px 12px;
  border: 0;
  border-radius: 8px;
  background: transparent;
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.modes__btn.is-on {
  background: var(--surface);
  color: var(--accent);
  font-weight: 600;
}
</style>
