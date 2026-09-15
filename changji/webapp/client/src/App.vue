<script setup>
/**
 * 外壳。**只有一条。**
 *
 * 原来是六层叠在一起：顶栏、左侧 StepRail、ContextBar（项目名 + 集号）、
 * 每页的 StepHeader、页面内容、底部 stepbar（上一步/下一步）。其中三层
 * 在说同一件事——rail 上有对勾、StepHeader 有编号徽标、stepbar 写着
 * 「第 3 / 8 步」。加起来约 190px 垂直空间，而内容区反而是最小的那块。
 *
 * 现在一条顶栏装下全部：品牌、项目、五步导航（带对勾）、集号、引擎灯、
 * 主题、设置。**导航只画一遍**。
 *
 * 集号只在分集那一步出现。全剧那几步摆一个「当前集」，会让人以为角色和
 * 场景也要每集重做一遍——这条是从原来的 ContextBar 继承下来的判断。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import ErrorBoundary from '@/components/ErrorBoundary.vue'
import ToastStack from '@/components/ToastStack.vue'
import EngineLamp from '@/components/EngineLamp.vue'
import ProjectRail from '@/components/ProjectRail.vue'
import JobBadge from '@/components/JobBadge.vue'
import ThinkingBadge from '@/components/ThinkingBadge.vue'
import SysMeter from '@/components/SysMeter.vue'
import { STEP_ROUTES } from '@/router'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const route = useRoute()
const router = useRouter()
const session = useSession()
const ui = useUi()

const stepKey = computed(() => route.meta?.step ?? '')
const current = computed(() => STEP_ROUTES.find((s) => s.key === stepKey.value) ?? null)
const isSettings = computed(() => route.name === 'settings')
// 宽页（故事）：页面不滚，滚的是页面里那一格。见 router 里那条注释。
const wide = computed(() => route.meta?.wide === true)

/**
 * 专注模式：只在**写故事**那一页生效。
 *
 * 别的页收起顶栏就等于没有导航了——那不是专注，是走不出去。故事页不一样：
 * 它整页就是一个编辑器，而顶栏和项目库在那儿唯一的作用是提醒你"还有别的
 * 事可以干"。
 *
 * 鼠标贴到窗口最上面那几个像素，顶栏浮回来；Esc 退出。
 */
const canFocus = computed(() => stepKey.value === 'story')
const focused = computed(() => canFocus.value && ui.focusMode)
const peeking = ref(false)
const chromeOff = computed(() => focused.value && !peeking.value)

function onEdgePeek(e) {
  if (!focused.value) return
  peeking.value = e.clientY <= 4
}
function onEsc(e) {
  if (e.key === 'Escape' && focused.value) ui.focusMode = false
}
onMounted(() => {
  window.addEventListener('mousemove', onEdgePeek)
  window.addEventListener('keydown', onEsc)
})
onUnmounted(() => {
  window.removeEventListener('mousemove', onEdgePeek)
  window.removeEventListener('keydown', onEsc)
})

const perEpisode = computed(() => current.value?.phase === 'episode')
/**
 * 顶栏那个项目名。
 *
 * **读不到的时候拿目录名顶上，别说「未命名项目」。** `session.project`
 * 为空有两种情形，没有一种是"这部剧没起名字"：
 *
 *   · `/bff/flow` 还没回来——每次进页面都有那么一小会儿，顶栏先闪一下
 *     「未命名项目」再变成真名；
 *   · flow 回了 400/404（项目被删了、挪了位置）——那时候它是**长期**空的，
 *     顶栏就一直挂着「未命名项目」，而那部剧其实好好地叫着别的名字。
 *
 * 而「未命名项目」说的偏偏就是"这部剧没起名字"。同一件事 JobBadge 的
 * `nameOf` 和项目页的 `dirName` 都是拿目录名顶的（各自注释里写着理由），
 * 这儿跟上它们。
 */
const projectName = computed(() => {
  const p = session.project
  if (p?.title) return p.title
  if (p?.project_id) return p.project_id
  const dir = (session.projectPath || '').split(/[\\/]+/).filter(Boolean).pop()
  return dir || '未命名项目'
})

/** 第一个还没做完的那一步。导航上给它一个点，代替原来那条 stepbar。 */
const nextKey = computed(() => {
  const step = STEP_ROUTES.find((s) => !session.done[s.key])
  return step ? step.key : ''
})

// 换项目、换集都要重新算一遍进度，否则导航上的对勾会停在上一个项目上
watch(
  () => [session.projectPath, session.episodeId],
  () => session.refresh(),
)

onMounted(() => session.refresh())

function onPickEpisode(event) {
  session.selectEpisode(event.target.value)
}

function cycleTheme() {
  const order = ['system', 'dark', 'light']
  ui.theme = order[(order.indexOf(ui.theme) + 1) % order.length]
}
</script>

<template>
  <div class="shell">
    <header v-if="!chromeOff" class="topbar">
      <RouterLink to="/project" class="brand" title="场记">
        <span class="brand__mark">场</span>
      </RouterLink>

      <!-- **写「这一部剧」不写「换个项目」。** 它去的是 /project，而那一页
           是这一部剧的进度和设置（画幅、模型、删除），根本没有项目列表——
           换项目唯一的地方是项目库那条栏。标签指错地方的后果：想删项目的
           人不会点它，想换项目的人点进去连提示都看不到。 -->
      <button
        v-if="session.hasProject"
        class="proj"
        type="button"
        title="这一部剧的进度和设置"
        @click="router.push('/project')"
      >
        <AppIcon name="folder" :size="14" />
        <span class="proj__name truncate">{{ projectName }}</span>
      </button>

      <nav class="nav">
        <RouterLink
          v-for="s in STEP_ROUTES"
          :key="s.key"
          :to="s.path"
          class="nav__item"
          :class="{
            'is-on': stepKey === s.key,
            'is-done': session.done[s.key],
            'is-next': nextKey === s.key,
          }"
        >
          <AppIcon v-if="session.done[s.key]" name="check" :size="12" />
          <span>{{ s.title }}</span>
        </RouterLink>
      </nav>

      <label v-if="perEpisode && session.hasProject" class="ep">
        <select
          class="select select--slim"
          :value="session.episodeId"
          :disabled="!session.episodes.length"
          @change="onPickEpisode"
        >
          <option v-if="!session.episodes.length" value="">还没有剧集</option>
          <option v-for="ep in session.episodes" :key="ep.episode_id" :value="ep.episode_id">
            {{ ep.episode_id }} · {{ ep.title || '未命名' }}（{{ ep.shots }} 镜）
          </option>
        </select>
      </label>

      <span class="spacer" />

      <!-- GPU / CPU / 内存三个小表，引擎走 WebSocket 推过来 -->
      <!-- 有活在跑才出现；鼠标放上去列出来，点一行直接过去。 -->
      <ThinkingBadge />
      <JobBadge />

      <SysMeter />

      <EngineLamp />

      <button
        class="btn btn--ghost topbar__icon"
        type="button"
        :title="`主题：${ui.theme === 'system' ? '跟随系统' : ui.theme === 'dark' ? '深色' : '浅色'}`"
        @click="cycleTheme"
      >
        <AppIcon :name="ui.theme === 'light' ? 'sun' : 'moon'" :size="17" />
      </button>

      <RouterLink
        to="/settings"
        class="btn btn--ghost topbar__icon"
        :class="{ 'is-on': isSettings }"
        title="设置"
      >
        <AppIcon name="gear" :size="17" />
      </RouterLink>
    </header>

    <div class="body">
      <main class="main">
        <div class="main__scroll" :class="{ 'main__scroll--wide': wide }">
        <div
          class="main__inner"
          :class="{
            'main__inner--focus': focused,
            'main__inner--wide': wide,
          }"
        >
          <!-- 页面崩了要说出来，而不是白屏。见 ErrorBoundary 里的说明。 -->
          <ErrorBoundary>
            <RouterView v-slot="{ Component }">
              <Transition name="fade" mode="out-in">
                <component :is="Component" />
              </Transition>
            </RouterView>
            </ErrorBoundary>
          </div>
        </div>
      </main>

      <!-- 项目库常驻在右边。换项目原来要走到第一步那一页，挑完再走回来，
           而当前这一页的状态就丢了。常驻之后点一下就换，人还停在原来那页。 -->
      <ProjectRail v-if="!chromeOff" />
    </div>

    <ToastStack />
  </div>
</template>

<style scoped>
.shell {
  display: flex;
  flex-direction: column;
  height: 100%;
  background: var(--bg);
}

/* ---------- 唯一的那条顶栏 ---------- */

.topbar {
  display: flex;
  align-items: center;
  gap: var(--s2);
  height: var(--topbar-h);
  padding: 0 var(--s4);
  padding-left: max(var(--s4), env(safe-area-inset-left));
  padding-right: max(var(--s4), env(safe-area-inset-right));
  background: color-mix(in srgb, var(--surface) 88%, transparent);
  backdrop-filter: blur(14px);
  border-bottom: 1px solid var(--line);
  flex: none;
  z-index: 30;
}
.topbar__icon {
  width: 34px;
  padding: 0;
  flex: none;
}
.topbar__icon.is-on {
  color: var(--accent);
  background: var(--accent-soft);
}

.brand {
  flex: none;
  color: var(--text);
  text-decoration: none;
}
.brand:hover {
  text-decoration: none;
}
.brand__mark {
  display: grid;
  place-items: center;
  width: 30px;
  height: 30px;
  border-radius: 9px;
  background: linear-gradient(
    135deg,
    var(--accent),
    color-mix(in srgb, var(--accent) 60%, #d9534f)
  );
  color: var(--accent-text);
  font-weight: 700;
  font-size: 15px;
  box-shadow: var(--shadow-1);
}

.proj {
  display: flex;
  align-items: center;
  gap: 6px;
  max-width: 11rem;
  flex: none;
  padding: 5px 10px;
  border: 1px solid var(--line);
  border-radius: 9px;
  background: var(--surface-2);
  color: var(--text-2);
  font-size: var(--fs-sm);
  cursor: pointer;
}
.proj:hover {
  color: var(--text);
  border-color: var(--accent-line);
}
.proj__name {
  min-width: 0;
}

/* 导航。**这是唯一的一张地图**——rail、编号徽标、stepbar 三处重复的
   「我在第几步」现在只剩这一处。 */
.nav {
  display: flex;
  align-items: center;
  gap: 2px;
  min-width: 0;
  overflow-x: auto;
  scrollbar-width: none;
}
.nav::-webkit-scrollbar {
  display: none;
}
.nav__item {
  display: flex;
  align-items: center;
  gap: 4px;
  flex: none;
  padding: 6px 12px;
  border-radius: 9px;
  color: var(--text-2);
  text-decoration: none;
  font-size: var(--fs-sm);
  white-space: nowrap;
}
.nav__item:hover {
  color: var(--text);
  background: var(--surface-2);
  text-decoration: none;
}
.nav__item.is-done {
  color: var(--ok);
}
.nav__item.is-on {
  background: var(--accent-soft);
  color: var(--accent);
  font-weight: 600;
}
/* 第一个还没做完的那一步。原来底下那条 stepbar 就是干这个的，
   一整条 44px 只为说一句「下一步去哪」。 */
.nav__item.is-next::after {
  content: '';
  width: 5px;
  height: 5px;
  border-radius: 50%;
  background: var(--accent);
}

.ep {
  flex: none;
  max-width: 14rem;
}
.ep .select {
  max-width: 100%;
}

/* ---------- 主体 ---------- */

.body {
  display: flex;
  flex: 1;
  min-height: 0;
}
.main {
  flex: 1;
  min-width: 0;
  min-height: 0;
  display: flex;
  flex-direction: column;
}
.main__scroll {
  flex: 1;
  overflow-y: auto;
  overscroll-behavior: contain;
}
/* **不居中、不卡宽。** 用户 2026-09-11：「不要居中显示，撑满屏幕，不要浪费
   空间，所有页面都是一样」。原来卡在 1180px 居中，宽屏上两边各空一大块。 */
.main__inner {
  /* 12px，和故事页一样：一个标点的宽度，字不压边线就行。 */
  padding: var(--s3) var(--s3) var(--s8);
  padding-left: max(var(--s6), env(safe-area-inset-left));
  padding-right: max(var(--s6), env(safe-area-inset-right));
}
/* 专注模式：稿纸铺满，两边不留边距。顶栏和项目库都收了，
   这一屏上除了字什么都没有。

   **这一条必须排在 `.main__inner` 后面**——两条同样的特指度（都是一个
   类），靠源码顺序覆盖；写在前面的话上面那三行内边距根本不会被去掉，
   「铺满」就是假的。

   （这段话原来的主语是"初始化页"。那一页 2026-09-14 删了——用户原话
   「不需要初始化页面」，见 ProjectView 开头——而这条排版要求跟着落到了
   下面这个 `--focus` 上。） */
.main__inner--focus {
  max-width: none;
  padding: 0;
}

/* 宽页：**页面不滚**。外层 overflow 关掉、整条高度交给页面，页面里
   哪一格该滚由它自己定。故事页靠这个让正文那一格独占滚动——
   之前页面和稿纸各有一根滚动条，高度算错一次整页就垮。 */
.main__scroll--wide {
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.main__inner--wide {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  width: 100%;
  max-width: none;
  padding: 0;
}

.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.16s var(--ease), transform 0.16s var(--ease);
}
.fade-enter-from {
  opacity: 0;
  transform: translateY(6px);
}
.fade-leave-to {
  opacity: 0;
}

/* ---------- 窄屏 ---------- */

@media (max-width: 860px) {
  /* 项目名和集号让位给导航——导航是一直要用的，那两个是偶尔换一次。
     它们仍然点得到：项目在第一步那一页，集号在「这一集」页里也有。 */
  .proj {
    display: none;
  }
  .ep {
    max-width: 8rem;
  }
  .main__inner {
    padding: var(--s4) var(--s4) var(--s10);
  }
  .main__inner--wide {
    padding: 0;
  }
}
</style>
