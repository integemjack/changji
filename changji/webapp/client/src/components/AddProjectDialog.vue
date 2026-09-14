<script setup>
/**
 * 加一个项目。弹窗。
 *
 * **两种「加」合在一个弹窗里**：新建一个，或者把磁盘上已有的目录接进来。
 * 对用户是同一件事——「我要多一个项目」，分成两张卡摆在页面上时，
 * 那两张卡常年占着位置而一年点不了两次。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'
import { useAction } from '@/composables/useAction'
import { useProjects } from '@/stores/projects'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const props = defineProps({ open: { type: Boolean, default: false } })
const emit = defineEmits(['close', 'added'])
/**
 * Esc 关掉。
 *
 * 抽屉那几处早就有（「Esc 是唯一不用先瞄准的出口」），而**弹窗比抽屉更该
 * 有**——它盖住整屏，除了右上角那个 ✕ 和点外面，没别的出路。三个弹窗原来
 * 一个都不认 Esc。
 *
 * 判 `props.open`：这个组件是**一直挂着**的（`v-if` 在模板里面），不判的话
 * 它在窗口关着的时候也吃 Esc。
 */
function onEsc(e) {
  if (e.key === 'Escape' && props.open) emit('close')
}
onMounted(() => window.addEventListener('keydown', onEsc))
onUnmounted(() => window.removeEventListener('keydown', onEsc))
/**
 * 打开之后把焦点放进来。
 *
 * **不然键盘上这个弹窗基本没法用。** 用 Tab 走到那颗按钮、回车打开，焦点
 * 还停在**遮罩后面**那颗按钮上：按 Tab 是在看不见的页面里一格格走，走到
 * 弹窗里之前，屏幕上一个焦点框都看不见。
 *
 * 焦点落在面板本身（tabindex="-1"），不猜第一个该聚焦的控件——猜错了会把
 * 人直接丢进某个输入框，而读屏也该先听见这是个什么窗。再按 Tab 就顺着进
 * 里面的控件了。
 */
const panel = ref(null)
// **盯着面板出现，不是盯着 open 变真。** 面板挂在内容上（拍摄那个是
// `v-if="video"`，模型那个是 `v-if="g"`），而内容是打开之后异步读回来的
// ——按 open 那一刻去聚焦会扑空，而扑空的表现正是这条要修的：焦点留在
// 遮罩后面。模板 ref 本身是响应式的，元素挂上来就聚焦，卸了就是 null。
watch(panel, (el) => el?.focus())

const session = useSession()
const store = useProjects()
const ui = useUi()
const { run, isBusy } = useAction()

const mode = ref('new') // new | existing
const draft = ref({ path: '', title: '', style_line: 'realistic' })
const openPath = ref('')

const workspace = computed(() => store.workspace || '工作目录')

/**
 * 填的这条路径在不在项目库目录下面。
 *
 * 库里的接进来之后栏上本来就有它那一条；**库外的没有**——引擎那份列表
 * 只扫工作目录，而全仓没有任何「最近打开」的登记。这件事要在人填之前说，
 * 不是切走之后才发现回不去。
 */
const inWorkspace = computed(() => {
  const w = (store.workspace || '').replace(/[\\/]+$/, '').toLowerCase()
  const v = openPath.value.trim().replace(/[\\/]+$/, '').toLowerCase()
  if (!w || !v) return true // 还没填就别先吓人
  return v.startsWith(w + '\\') || v.startsWith(w + '/')
})

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
      <section ref="panel" class="card modal__panel" tabindex="-1">
        <div class="card__head">
          <div>
            <div class="card__title">加一个项目</div>
            <div class="card__sub">新建一个，或者把已经有的目录接进来。</div>
          </div>
          <button
            class="btn btn--ghost btn--sm"
            type="button"
            aria-label="关闭"
            @click="emit('close')"
          >
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
              <!-- ⚠️ **别再写「它会出现在项目库里」。** 项目库那条栏只列
                   工作目录下面的项目（引擎 get_projects 扫的就是那一个
                   目录），库外的目录接进来只是把它设成当前项目，栏上没有
                   它那一条，也没有任何「最近打开」的登记——切走之后界面上
                   没有一个地方能把这条路径想起来。 -->
              <span class="field__hint">
                得是一个已经建好的项目目录（里面有 project.json）。
                <strong v-if="!inWorkspace">它不在项目库目录下，接进来只对这一次有效——
                栏上不会有它那一条，切走之后要再打一遍这条路径。</strong>
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
