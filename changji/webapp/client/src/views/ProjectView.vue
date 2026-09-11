<script setup>
/**
 * 第一步：项目。
 *
 * 进来第一眼要看到「我有哪些项目」，而不是一个让人填绝对路径的输入框。
 * 容器里项目库挂在哪，用户根本不知道，问引擎要列表才是对的。
 */
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { projectStage } from '@/composables/project-stage'
import StepHeader from '@/components/StepHeader.vue'
import { api } from '@/api'
import { humanAgo, useAction } from '@/composables/useAction'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const router = useRouter()
const session = useSession()
const ui = useUi()
const { run, isBusy, busy } = useAction()

const workspace = ref('')
const projects = ref([])
const loading = ref(true)
const loadError = ref('')
const keyword = ref('')

const creating = ref(false)
const draft = ref({ path: '', title: '', style_line: 'realistic' })

// 项目库以外的项目。整个目录拷到别的机器就能接着做，所以项目常常躺在
// 移动硬盘或者共享盘上，不在项目库根目录下面——那些以前在界面上够不着。
const opening = ref(false)
const openPath = ref('')

const removing = ref(null) // 待删项目
const confirmName = ref('')

const shown = computed(() => {
  const kw = keyword.value.trim().toLowerCase()
  if (!kw) return projects.value
  return projects.value.filter(
    (p) =>
      p.name?.toLowerCase().includes(kw) || p.dir?.toLowerCase().includes(kw),
  )
})

async function load() {
  loading.value = true
  loadError.value = ''
  try {
    const data = await api.projects()
    workspace.value = data.workspace
    projects.value = data.projects ?? []
  } catch (err) {
    loadError.value = err.message
  } finally {
    loading.value = false
  }
}

onMounted(load)

function open(project) {
  if (project.broken) {
    ui.error(`这个项目读不了：${project.broken}`)
    return
  }
  session.selectProject(project.path)
  ui.ok(`已切到「${project.name}」`)
  router.push('/story')
}

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
  creating.value = false
  draft.value = { path: '', title: '', style_line: 'realistic' }
  await load()
  session.selectProject(result.root)
  router.push('/story')
}

/** 按路径打开一个项目库以外的项目。先问引擎认不认，再切过去。 */
async function openByPath() {
  const path = openPath.value.trim()
  if (!path) {
    ui.warn('填一个项目目录的完整路径')
    return
  }
  const info = await run(() => api.project(path), { key: 'open' })
  if (!info) return
  session.selectProject(info.root)
  opening.value = false
  openPath.value = ''
  ui.ok(`已切到「${info.title || info.project_id}」`)
  router.push('/story')
}

async function remove() {
  if (!removing.value) return
  const target = removing.value
  const done = await run(
    () => api.deleteProject({ path: target.path, confirm_name: confirmName.value }),
    { key: 'delete', success: '已删除' },
  )
  if (!done) return
  removing.value = null
  confirmName.value = ''
  if (session.projectPath === target.path) session.clear()
  await load()
}

/** 这部剧到哪一步了。判断在 composables/project-stage.js，那儿有用例盯着。 */
function stageOf(p) {
  return projectStage(p)
}
</script>

<template>
  <div class="stack--lg stack">
    <StepHeader>
      <template #actions>
        <button class="btn btn--ghost" type="button" :disabled="loading" @click="load">
          <AppIcon name="refresh" :size="15" />
          刷新
        </button>
        <button class="btn btn--ghost" type="button" @click="opening = !opening">
          <AppIcon name="folder" :size="15" />
          打开其他目录
        </button>
        <button class="btn btn--primary" type="button" @click="creating = !creating">
          <AppIcon name="plus" :size="15" />
          新建项目
        </button>
      </template>
    </StepHeader>

    <!-- 打开项目库以外的项目 -->
    <Transition name="fold">
      <section v-if="opening" class="card">
        <div class="card__head">
          <div>
            <div class="card__title">打开其他目录的项目</div>
            <div class="card__sub">
              项目整个目录拷到哪儿都能接着做。不在项目库里的，填完整路径打开。
            </div>
          </div>
          <button class="btn btn--ghost btn--sm" type="button" @click="opening = false">
            <AppIcon name="close" :size="14" />
          </button>
        </div>
        <div class="card__body row">
          <input
            v-model="openPath"
            class="input mono"
            placeholder="D:\短剧\雪夜  或者  /data/projects/雪夜"
            @keyup.enter="openByPath"
          />
          <button
            class="btn btn--primary nowrap"
            type="button"
            :disabled="isBusy('open')"
            @click="openByPath"
          >
            {{ isBusy('open') ? '打开中…' : '打开' }}
          </button>
        </div>
      </section>
    </Transition>

    <!-- 新建 -->
    <Transition name="fold">
      <section v-if="creating" class="card">
        <div class="card__head">
          <div>
            <div class="card__title">新建项目</div>
            <div class="card__sub">
              只填名字就落在项目库里；填完整路径可以建在别处。
            </div>
          </div>
          <button class="btn btn--ghost btn--sm" type="button" @click="creating = false">
            <AppIcon name="close" :size="14" />
          </button>
        </div>
        <div class="card__body grid grid--form">
          <label class="field">
            <span class="field__label">项目名或路径</span>
            <input
              v-model="draft.path"
              class="input"
              placeholder="例如：雪夜"
              @keyup.enter="create"
            />
            <span class="field__hint">项目库：<code class="mono">{{ workspace || '读取中' }}</code></span>
          </label>
          <label class="field">
            <span class="field__label">剧名</span>
            <input v-model="draft.title" class="input" placeholder="不填就用项目名" />
          </label>
          <label class="field">
            <span class="field__label">风格线</span>
            <select v-model="draft.style_line" class="select">
              <option value="realistic">真人写实</option>
              <option value="anime">动漫</option>
            </select>
            <span class="field__hint">决定出图基座和提示词范式，建好之后不建议再改。</span>
          </label>
        </div>
        <div class="card__foot">
          <button
            class="btn btn--primary"
            type="button"
            :disabled="isBusy('create')"
            @click="create"
          >
            {{ isBusy('create') ? '正在建…' : '建好，去写剧本' }}
          </button>
          <button class="btn btn--ghost" type="button" @click="creating = false">
            取消
          </button>
        </div>
      </section>
    </Transition>

    <!-- 列表 -->
    <section class="stack">
      <div class="row row--between">
        <div class="row">
          <h2 class="section-title">项目库</h2>
          <span v-if="projects.length" class="pill pill--neutral">
            {{ projects.length }} 个
          </span>
        </div>
        <input
          v-if="projects.length > 6"
          v-model="keyword"
          class="input input--search"
          placeholder="搜项目"
        />
      </div>

      <div v-if="loading" class="grid grid--cards">
        <div v-for="i in 3" :key="i" class="card skeleton" />
      </div>

      <EmptyState
        v-else-if="loadError"
        icon="warn"
        tone="warn"
        title="读不到项目库"
        :hint="loadError"
      >
        <button class="btn" type="button" @click="load">重试</button>
        <RouterLink to="/settings" class="btn btn--primary">去设置里检查引擎地址</RouterLink>
      </EmptyState>

      <EmptyState
        v-else-if="!projects.length"
        icon="folder"
        title="项目库还是空的"
        :hint="`新建一个项目就能开始。它会落在 ${workspace}，整个目录拷到别的机器就能接着做。`"
      >
        <button class="btn btn--primary" type="button" @click="creating = true">
          <AppIcon name="plus" :size="15" />
          新建第一个项目
        </button>
      </EmptyState>

      <div v-else class="grid grid--cards">
        <article
          v-for="p in shown"
          :key="p.path"
          class="proj card"
          :class="{
            'proj--current': p.path === session.projectPath,
            'proj--broken': p.broken,
          }"
          tabindex="0"
          @click="open(p)"
          @keyup.enter="open(p)"
        >
          <div class="proj__top">
            <span v-if="p.path === session.projectPath" class="pill pill--accent">
              当前
            </span>
            <span class="spacer" />
            <button
              class="btn btn--ghost btn--sm proj__del"
              type="button"
              title="删除项目"
              @click.stop="((removing = p), (confirmName = ''))"
            >
              <AppIcon name="trash" :size="14" />
            </button>
          </div>

          <h3 class="proj__name truncate">{{ p.name }}</h3>
          <!-- **一句话说清这是讲什么的剧。** 名字常常就是目录名（321、
               雨夜天台），认不出剧情；logline 是故事层写下来的那一句。
               没有故事的老项目退回用梗概，两个都没有才摆路径。 -->
          <p v-if="p.logline" class="proj__line small truncate">{{ p.logline }}</p>
          <p v-else class="proj__dir tiny dim truncate mono">{{ p.dir }}</p>

          <p v-if="p.broken" class="proj__broken small">读不了：{{ p.broken }}</p>

          <template v-else>
            <!-- **进度说的是「到哪一步了」，不是「出片百分比」。**
                 原来只有后者，于是刚建的空项目和故事写完还没分镜的项目
                 都是 0%，看上去一模一样。 -->
            <div class="proj__stage" :class="`proj__stage--${stageOf(p).tone}`">
              {{ stageOf(p).label }}
            </div>
            <div class="proj__bar">
              <div
                class="proj__bar-fill"
                :class="`proj__bar-fill--${stageOf(p).tone}`"
                :style="{ width: stageOf(p).percent + '%' }"
              />
            </div>
            <!-- 原始计数降级到底栏：挑项目时先看「到哪一步了」，
                 数字是确认用的，不是用来认项目的。 -->
            <div class="proj__foot tiny dim">
              <span class="numeric">
                <template v-if="p.chapters">{{ p.chapters }} 章 · </template>
                <template v-if="p.episodes">{{ p.episodes }} 集 · </template>
                <template v-if="p.shots">{{ p.shots }} 镜</template>
                <template v-if="!p.chapters && !p.episodes && !p.shots">空的</template>
              </span>
              <span>{{ humanAgo(p.mtime) }}</span>
            </div>
          </template>
        </article>
      </div>
    </section>

    <!-- 删除确认 -->
    <div v-if="removing" class="modal" @click.self="removing = null">
      <div class="modal__box card">
        <div class="card__head">
          <div class="card__title">删掉「{{ removing.name }}」？</div>
        </div>
        <div class="card__body stack">
          <p class="small muted">
            连同素材、配音和已经跑出来的成片一起删，删了找不回来。
            确认的话，把目录名一字不差地打一遍：
          </p>
          <code class="modal__name mono">{{ removing.dir }}</code>
          <input
            v-model="confirmName"
            class="input"
            :placeholder="removing.dir"
            autofocus
            @keyup.enter="remove"
          />
        </div>
        <div class="card__foot">
          <button
            class="btn btn--danger"
            type="button"
            :disabled="confirmName !== removing.dir || busy"
            @click="remove"
          >
            {{ isBusy('delete') ? '正在删…' : '确认删除' }}
          </button>
          <button class="btn btn--ghost" type="button" @click="removing = null">
            取消
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.section-title {
  font-size: var(--fs-lg);
  font-weight: 600;
}
.input--search {
  width: 200px;
}
.grid--form {
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
}
.grid--cards {
  grid-template-columns: repeat(auto-fill, minmax(248px, 1fr));
}

.textarea--tight {
  min-height: 0;
}
.chips {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}
.chip {
  padding: 4px var(--s3);
  border-radius: var(--r-pill);
  border: 1px solid var(--line);
  background: var(--surface-2);
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.chip--on {
  background: var(--accent-soft);
  border-color: var(--accent-line);
  color: var(--accent);
  font-weight: 600;
}
.switch {
  display: grid;
  grid-template-columns: auto 1fr;
  align-items: center;
  gap: var(--s2);
  cursor: pointer;
  font-size: var(--fs-base);
}
.switch input {
  width: 16px;
  height: 16px;
  accent-color: var(--accent);
}
.switch .field__hint {
  grid-column: 2;
  margin-top: -4px;
}

.proj {
  padding: var(--s4);
  cursor: pointer;
  transition: border-color 0.15s var(--ease), transform 0.12s var(--ease),
    box-shadow 0.15s var(--ease);
}
.proj:hover {
  border-color: var(--line-strong);
  transform: translateY(-2px);
  box-shadow: var(--shadow-2);
}
.proj--current {
  border-color: var(--accent-line);
  background: linear-gradient(var(--accent-soft), transparent 60%), var(--surface);
}
.proj--broken {
  border-color: color-mix(in srgb, var(--danger) 40%, transparent);
}

.proj__top {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin-bottom: var(--s3);
}
.proj__del {
  width: 26px;
  padding: 0;
  color: var(--text-3);
  opacity: 0;
}
.proj:hover .proj__del,
.proj:focus-within .proj__del {
  opacity: 1;
}
.proj__del:hover {
  color: var(--danger);
}

.proj__name {
  font-size: var(--fs-lg);
  font-weight: 600;
  line-height: 1.3;
}
.proj__dir {
  margin-bottom: var(--s3);
}
.proj__broken {
  color: var(--danger);
  margin-top: var(--s2);
}

.proj__stats {
  display: flex;
  gap: var(--s4);
  font-size: var(--fs-sm);
  color: var(--text-2);
  margin-bottom: var(--s2);
}
.proj__stats b {
  color: var(--text);
  font-weight: 600;
}
.proj__line {
  margin-top: 2px;
  color: var(--text-2);
  line-height: 1.5;
}
.proj__stage {
  margin-top: var(--s3);
  font-size: var(--fs-sm);
  font-weight: 600;
}
.proj__stage--ok {
  color: var(--ok);
}
.proj__stage--warn {
  color: var(--warn);
}
.proj__stage--accent {
  color: var(--accent);
}
.proj__stage--dim,
.proj__stage--bad {
  color: var(--text-3);
  font-weight: 400;
}
.proj__bar-fill--ok {
  background: var(--ok);
}
.proj__bar-fill--warn {
  background: var(--warn);
}
.proj__bar {
  height: 4px;
  border-radius: var(--r-pill);
  background: var(--surface-3);
  overflow: hidden;
}
.proj__bar-fill {
  height: 100%;
  background: var(--accent);
  border-radius: var(--r-pill);
  transition: width 0.3s var(--ease);
}
.proj__foot {
  display: flex;
  justify-content: space-between;
  margin-top: 6px;
}

.skeleton {
  height: 168px;
  background: linear-gradient(
    100deg,
    var(--surface) 30%,
    var(--surface-2) 50%,
    var(--surface) 70%
  );
  background-size: 220% 100%;
  animation: shimmer 1.3s linear infinite;
}
@keyframes shimmer {
  to {
    background-position: -120% 0;
  }
}

.modal {
  position: fixed;
  inset: 0;
  z-index: 80;
  display: grid;
  place-items: center;
  padding: var(--s4);
  background: rgba(0, 0, 0, 0.6);
  backdrop-filter: blur(3px);
}
.modal__box {
  width: min(440px, 100%);
  box-shadow: var(--shadow-3);
}
.modal__name {
  display: block;
  padding: var(--s2) var(--s3);
  border-radius: var(--r);
  background: var(--bg-sunken);
  border: 1px solid var(--line);
  color: var(--accent);
}

.fold-enter-active,
.fold-leave-active {
  transition: opacity 0.18s var(--ease), transform 0.18s var(--ease);
}
.fold-enter-from,
.fold-leave-to {
  opacity: 0;
  transform: translateY(-8px);
}

@media (max-width: 640px) {
  .input--search {
    width: 130px;
  }
}
</style>
