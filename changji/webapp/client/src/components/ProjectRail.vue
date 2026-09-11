<script setup>
/**
 * 项目库。**常驻在右边，不在项目页里。**
 *
 * 换项目原来要先走到第一步那一页，挑完再走回来——而人在写故事、看镜头的
 * 时候想起来要对照另一部剧，这一来一回就把当前这一页的状态丢了。
 * 常驻之后：**点一下就换，人还停在原来那一页**。
 *
 * 每条只放两样：剧名，和到哪一步了。路径、画风、镜头数都不在这儿——
 * 那些是「这一部剧的详情」，归项目页；这条栏只回答「切到哪一部」。
 */
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import AddProjectDialog from '@/components/AddProjectDialog.vue'
import { projectStage } from '@/composables/project-stage'
import { useProjects } from '@/stores/projects'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const store = useProjects()
const session = useSession()
const ui = useUi()

const route = useRoute()
const keyword = ref('')
const adding = ref(false)

/**
 * 收没收起，**故事页和别处各记各的**。
 *
 * 故事页默认收起：那一页是个编辑器，而这条栏列着另外几部剧——你正在写
 * 第一章，旁边摆着"你还可以去干的别的事"。别的页默认展开，换项目本来就是
 * 在那些页上干的事。两边分开记，在故事页展开过一次不会让别处也跟着变。
 */
const onStory = computed(() => route.meta?.step === 'story')
const foldKey = computed(() => (onStory.value ? 'changji.rail.story' : 'changji.rail'))
const foldTick = ref(0)
const collapsed = computed({
  get() {
    foldTick.value // 写 localStorage 之后靠它重算
    const v = localStorage.getItem(foldKey.value)
    return v === null ? onStory.value : v === '1'
  },
  set(v) {
    localStorage.setItem(foldKey.value, v ? '1' : '0')
    foldTick.value++
  },
})

/**
 * 拖着栏头把整条挪到另一边。
 *
 * **用指针事件而不是 HTML5 拖放**：后者在触屏上不触发，而且拖起来会带一张
 * 半透明的整栏截图（那是浏览器画的，改不掉），一条 14rem 宽的栏拖着一张
 * 一样宽的影子，挡住的正是要看的落点。
 *
 * 判定只看一件事：松手时指针在屏幕的哪一半。够用，而且**不需要落区**——
 * 落区要么小得点不准，要么大得挡住内容。
 */
const dragging = ref(false)
const hintSide = ref('')

function onGrab(event) {
  // 只认主键。右键拖出来的是浏览器菜单，中键是自动滚动。
  if (event.button !== undefined && event.button !== 0) return
  dragging.value = true
  hintSide.value = ui.railSide
  event.currentTarget.setPointerCapture?.(event.pointerId)
}

function onMove(event) {
  if (!dragging.value) return
  hintSide.value = event.clientX < window.innerWidth / 2 ? 'left' : 'right'
}

function onDrop(event) {
  if (!dragging.value) return
  dragging.value = false
  event.currentTarget.releasePointerCapture?.(event.pointerId)
  const side = hintSide.value
  hintSide.value = ''
  if (!side || side === ui.railSide) return
  ui.railSide = side
  ui.ok(side === 'left' ? '项目库挪到左边了' : '项目库挪到右边了')
}

/** 键盘也得能换边——拖拽这种操作光有鼠标那条路是不行的。 */
function flip() {
  ui.railSide = ui.railSide === 'left' ? 'right' : 'left'
}

const shown = computed(() => {
  const kw = keyword.value.trim().toLowerCase()
  if (!kw) return store.items
  return store.items.filter(
    (p) =>
      p.name?.toLowerCase().includes(kw) ||
      p.dir?.toLowerCase().includes(kw) ||
      p.logline?.toLowerCase().includes(kw),
  )
})

onMounted(() => store.load())

// 别处动过项目（建了、删了、跑完一集）之后列表要跟上
watch(() => session.projectPath, () => store.load())

function pick(p) {
  if (p.path === session.projectPath) return
  if (p.broken) {
    ui.error(`这个项目读不了：${p.broken}`)
    return
  }
  // **不跳转。** 人在哪一页就留在哪一页——换项目是换上下文，不是换任务。
  session.selectProject(p.path)
  ui.ok(`已切到「${p.name}」`)
}
</script>

<template>
  <aside
    class="rail"
    :class="{
      'rail--collapsed': collapsed,
      'rail--left': ui.railSide === 'left',
      'rail--dragging': dragging,
    }"
  >
    <!-- 拖的时候在目标那一边画一条，告诉人松手会落到哪 -->
    <Teleport to="body">
      <div
        v-if="dragging && hintSide"
        class="dockhint"
        :class="`dockhint--${hintSide}`"
      />
    </Teleport>

    <div
      class="rail__head"
      :title="'拖我换边，双击也行（现在靠' + (ui.railSide === 'left' ? '左' : '右') + '）'"
      @pointerdown="onGrab"
      @pointermove="onMove"
      @pointerup="onDrop"
      @pointercancel="onDrop"
      @dblclick="flip"
    >
      <button
        class="btn btn--ghost btn--sm rail__fold"
        type="button"
        :title="collapsed ? '展开项目库' : '收起项目库'"
        @click.stop="collapsed = !collapsed"
      >
        <AppIcon
          :name="
            (ui.railSide === 'left') === collapsed ? 'arrowRight' : 'arrowLeft'
          "
          :size="15"
        />
      </button>
      <template v-if="!collapsed">
        <span class="rail__title">项目库</span>
        <span v-if="store.count" class="pill pill--neutral tiny">{{ store.count }}</span>
        <span class="spacer" />
        <button
          class="btn btn--ghost btn--sm"
          type="button"
          title="加一个项目"
          @click.stop="adding = true"
        >
          <AppIcon name="plus" :size="15" />
        </button>
      </template>
    </div>

    <template v-if="!collapsed">
      <input
        v-if="store.count > 6"
        v-model="keyword"
        class="input input--search rail__search"
        placeholder="搜项目"
      />

      <div class="rail__list">
        <p v-if="store.loading && !store.loaded" class="rail__note tiny dim">读取中…</p>
        <p v-else-if="store.error" class="rail__note tiny warn-text">{{ store.error }}</p>
        <p v-else-if="!store.count" class="rail__note tiny dim">
          还没有项目。点上面那个加号建一个。
        </p>

        <button
          v-for="p in shown"
          :key="p.path"
          class="item"
          :class="{
            'is-on': p.path === session.projectPath,
            'is-broken': p.broken,
          }"
          type="button"
          :title="p.logline || p.dir"
          @click="pick(p)"
        >
          <span class="item__name truncate">{{ p.name }}</span>
          <span class="item__stage tiny truncate" :class="`item__stage--${projectStage(p).tone}`">
            {{ projectStage(p).label }}
          </span>
        </button>
      </div>
    </template>

    <AddProjectDialog :open="adding" @close="adding = false" />
  </aside>
</template>

<style scoped>
.rail {
  flex: none;
  width: 14rem;
  display: flex;
  flex-direction: column;
  min-height: 0;
  border-left: 1px solid var(--line);
  background: color-mix(in srgb, var(--surface) 60%, transparent);
}
/* 靠左时用 order 把它排到内容前面，边框也换一边。
   **不用 row-reverse**：那会把主内容区里所有 flex 行的方向一起翻过来。 */
.rail--left {
  order: -1;
  border-left: 0;
  border-right: 1px solid var(--line);
}
.rail--dragging {
  opacity: 0.6;
}
.rail--collapsed {
  width: auto;
}

.rail__head {
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: var(--s3);
  border-bottom: 1px solid var(--line);
  flex: none;
  cursor: grab;
  touch-action: none; /* 触屏上不给它滚页面，不然拖不动 */
  user-select: none;
}
.rail--dragging .rail__head {
  cursor: grabbing;
}
.rail--collapsed .rail__head {
  border-bottom: 0;
}
.rail__title {
  font-size: var(--fs-sm);
  font-weight: 600;
  color: var(--text-2);
}
.rail__fold {
  width: 26px;
  padding: 0;
}
.rail__search {
  margin: var(--s3) var(--s3) 0;
  flex: none;
}
.rail__list {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
  padding: var(--s3);
  display: grid;
  gap: 2px;
  align-content: start;
}
.rail__note {
  padding: var(--s2);
  line-height: 1.6;
}

.item {
  display: grid;
  gap: 1px;
  width: 100%;
  padding: var(--s2) var(--s3);
  border: 0;
  border-radius: 9px;
  background: transparent;
  text-align: left;
  cursor: pointer;
}
.item:hover {
  background: var(--surface-2);
}
.item.is-on {
  background: var(--accent-soft);
}
.item.is-broken {
  opacity: 0.55;
}
.item__name {
  font-size: var(--fs-sm);
  color: var(--text);
}
.item.is-on .item__name {
  color: var(--accent);
  font-weight: 600;
}
.item__stage {
  color: var(--text-3);
}
.item__stage--ok {
  color: var(--ok);
}
.item__stage--warn {
  color: var(--warn);
}

/* 拖的时候在要落的那一边画一条。**不做落区**——落区要么小得点不准，
   要么大得挡住内容；一条提示线说清了就够。 */
.dockhint {
  position: fixed;
  top: var(--topbar-h);
  bottom: 0;
  width: 14rem;
  z-index: 70;
  pointer-events: none;
  background: color-mix(in srgb, var(--accent) 14%, transparent);
  border: 2px dashed var(--accent);
}
.dockhint--left {
  left: 0;
}
.dockhint--right {
  right: 0;
}

/* 窄屏上整条收起来：那点宽度让给内容，换项目走顶栏的项目名进项目页。 */
@media (max-width: 900px) {
  .rail {
    display: none;
  }
}
</style>
