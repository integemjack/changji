<script setup>
/**
 * 这一部剧。
 *
 * **这一页只显示答案，改东西一律在弹窗里。** 2026-09-14 重排的规矩：
 * 每次打开这一页都要回答的问题，才配留在页面上；其余三条出路——改的时候
 * 才要的进弹窗，出问题才要的条件出现，别处已经说了的删掉。
 *
 * 按这条过完，原来那 41 件东西剩 5 件：这是哪部剧（logline）、到哪一步了
 * （进度条）、这部片子长什么样（一行答案）、拿哪几个模型跑（四个名字）、
 * 删掉它。整页从 1826px 压到一屏以内。
 *
 * 砍掉的里面值得记的几条：
 *   · `0 集 0 镜 0 成片` —— 进度条和阶段文字已经说完了，而且还没到那一步
 *     时三个 0 是噪音不是信息；
 *   · `19 小时前` —— 没有任何决定依赖它；
 *   · 完整路径 —— 一年用一次（找文件），却天天占半行，换成一个 📁；
 *   · 「写实线」标签 —— 画风那段文字自己就写着"实拍摄影"。
 *
 * **模型那一行不在 hasProject 分支里**：一台刚装好的机器上一个项目都没有，
 * 而那正是最需要挑模型的时候。初始化页 2026-09-14 删了（用户：「不需要
 * 初始化页面」），这一行就是它的替代品——没有项目时整页只有它和"新建项目"，
 * 看不见反而难。
 */
import { computed, onMounted, ref, watch } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import ModelDialog from '@/components/ModelDialog.vue'
import ShowDialog from '@/components/ShowDialog.vue'
import { api } from '@/api'
import { qualitySize } from '@/api/labels'
import { projectStage } from '@/composables/project-stage'
import { useAction } from '@/composables/useAction'
import { useModels } from '@/stores/models'
import { useProjects } from '@/stores/projects'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const store = useProjects()
const session = useSession()
const models = useModels()
const ui = useUi()
const { run, isBusy } = useAction()

const removing = ref(false)
const confirmName = ref('')
const showOpen = ref(false)
const modelKey = ref('') // 打开的是哪一组的弹窗，空串是没开

/** 项目库里这一条。统计和阶段都从它来，和右边那条栏读的是同一份。 */
const me = computed(() => store.byPath(session.projectPath))
const stage = computed(() => (me.value ? projectStage(me.value) : null))
const title = computed(
  () => session.project?.title || session.project?.project_id || '',
)

/** 「这部片子」那一行的答案：画幅 · 尺寸 · 画风开头一句。 */
const show = ref(null)

async function loadShow() {
  if (!session.projectPath) {
    show.value = null
    return
  }
  const [v, a] = await Promise.allSettled([
    api.projectVideo(session.projectPath),
    api.assets(session.projectPath),
  ])
  const video = v.status === 'fulfilled' ? v.value : null
  const style = a.status === 'fulfilled' ? a.value.style : null
  show.value = {
    orientation: video?.orientation === 'landscape' ? '横屏' : '竖屏',
    size: video ? qualitySize(video.quality, video.orientation) : '',
    look: style?.global_style || '',
  }
}

async function remove() {
  const path = session.projectPath
  const done = await run(
    () => api.deleteProject({ path, confirm_name: confirmName.value }),
    { key: 'delete', success: '已删除' },
  )
  if (!done) return
  removing.value = false
  confirmName.value = ''
  session.clear()
  await store.load()
}

/**
 * 把项目目录的路径抄到剪贴板。
 *
 * 原来这儿是页面上印着的一长串路径——一年用一次（找文件），却天天占半行。
 * **不做"在文件管理器里打开"**：那要引擎加一个接口，而为一个图标去开一条
 * 能执行 shell 的路不值当。抄走自己去粘，够用。
 */
async function copyPath() {
  const path = session.projectPath
  try {
    await navigator.clipboard.writeText(path)
    ui.ok('路径抄好了')
  } catch {
    // 没有剪贴板权限（http 下的非 localhost 就没有）。把路径显示出来，
    // 让他自己选——总比一个点了没反应的图标强。
    ui.info(path)
  }
}

onMounted(() => {
  if (!store.loaded) store.load()
  models.load()
})

watch(() => session.projectPath, loadShow, { immediate: true })
</script>

<template>
  <!-- 根元素的类名不能叫 .proj：子组件的根会带上父组件的 scoped 属性，
       App.vue 里给顶栏项目按钮写的 .proj { max-width: 11rem } 会套到这儿，
       整页被压成 176px 宽。栽过一次。 -->
  <div class="pj">
    <template v-if="session.hasProject">
      <!-- 这是哪部剧 -->
      <div class="head">
        <p class="lead" :class="{ dim: !me?.logline }">{{ me?.logline || '还没写故事' }}</p>
        <button class="btn btn--ghost btn--sm" type="button" title="抄走路径" @click="copyPath">
          <AppIcon name="folder" :size="14" />
        </button>
        <button
          class="btn btn--ghost btn--sm"
          :class="{ 'danger-text': !removing }"
          type="button"
          @click="removing = !removing"
        >
          {{ removing ? '算了' : '删掉' }}
        </button>
      </div>

      <div v-if="removing" class="row row--wrap confirm-row">
        <input
          v-model="confirmName"
          class="input confirm"
          :placeholder="'照着打一遍「' + title + '」'"
        />
        <button
          class="btn btn--danger btn--sm"
          type="button"
          :disabled="confirmName !== title || isBusy('delete')"
          @click="remove"
        >
          永久删除
        </button>
      </div>

      <!-- 到哪一步了 -->
      <div v-if="stage" class="progress">
        <span class="stage tiny" :class="`stage--${stage.tone}`">{{ stage.label }}</span>
        <div class="bar">
          <div
            class="bar__fill"
            :class="`bar__fill--${stage.tone}`"
            :style="{ width: stage.percent + '%' }"
          />
        </div>
      </div>

      <!-- 这部片子长什么样：一行答案，✎ 开弹窗 -->
      <button class="line" type="button" @click="showOpen = true">
        <span class="line__k">这部片子</span>
        <span class="line__v truncate">
          <template v-if="show">
            {{ show.orientation }} · {{ show.size }}<template v-if="show.look"> · {{ show.look }}</template>
          </template>
          <span v-else class="dim">读取中…</span>
        </span>
        <AppIcon name="wand" :size="14" class="line__go" />
      </button>
    </template>

    <!-- 拿哪几个模型跑。**没有项目也显示**，理由见文件开头。 -->
    <div class="line line--static">
      <span class="line__k">模型</span>
      <span class="line__v names">
        <button
          v-for="m in models.inUse"
          :key="m.key"
          class="name"
          :class="{ 'name--missing': m.missing }"
          type="button"
          :title="m.title + (m.missing ? '（还没下全）' : '')"
          @click="modelKey = m.key"
        >
          {{ m.name }}<span v-if="m.missing" class="warn">⚠</span>
        </button>
        <span v-if="!models.inUse.length" class="dim tiny">读取中…</span>
      </span>
    </div>

    <EmptyState
      v-if="!session.hasProject"
      icon="folder"
      tone="warn"
      title="还没选项目"
      hint="在右边项目库里点一个，或者新建一个"
    />

    <ShowDialog :open="showOpen" @close="showOpen = false" @saved="loadShow" />
    <ModelDialog :open="!!modelKey" :group-key="modelKey" @close="modelKey = ''" />
  </div>
</template>

<style scoped>
.pj {
  display: flex;
  flex-direction: column;
  gap: var(--s2);
  padding-bottom: var(--s4);
}

.head {
  display: flex;
  align-items: flex-start;
  gap: var(--s2);
}

.lead {
  flex: 1;
  margin: 0;
  font-size: var(--fs-md);
  line-height: 1.6;
}

.confirm-row {
  margin-top: calc(var(--s2) * -1);
}

.confirm {
  max-width: 22rem;
}

.progress {
  display: flex;
  align-items: center;
  gap: var(--s2);
}

.bar {
  flex: 1;
  height: 4px;
  border-radius: 999px;
  background: var(--line);
  overflow: hidden;
}

.bar__fill {
  height: 100%;
  background: var(--accent);
}

/* 一行答案。整行可点——点哪儿都是"我要改这个"。 */
.line {
  display: flex;
  align-items: center;
  gap: var(--s2);
  width: 100%;
  padding: 8px 10px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: transparent;
  color: var(--text);
  text-align: left;
  cursor: pointer;
}

.line:hover {
  border-color: var(--accent);
}

/* 模型那行整行不可点——可点的是里面每一个名字 */
.line--static {
  cursor: default;
}

.line--static:hover {
  border-color: var(--line);
}

.line__k {
  flex: 0 0 4.5rem;
  color: var(--text-dim);
  font-size: 12px;
}

.line__v {
  flex: 1;
  min-width: 0;
  font-size: 13px;
}

.line__go {
  flex: 0 0 auto;
  color: var(--text-dim);
}

.names {
  display: flex;
  flex-wrap: wrap;
  gap: 4px 10px;
}

.name {
  padding: 2px 6px;
  border: 1px solid transparent;
  border-radius: 6px;
  background: transparent;
  color: var(--text);
  font-size: 13px;
  cursor: pointer;
}

.name:hover {
  border-color: var(--accent);
}

.name--missing {
  color: var(--warn, #f5a524);
}

.warn {
  margin-left: 3px;
}
</style>
