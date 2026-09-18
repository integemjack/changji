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
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import ErrorBoundary from '@/components/ErrorBoundary.vue'
import ToastStack from '@/components/ToastStack.vue'
import EngineLamp from '@/components/EngineLamp.vue'
import ProjectRail from '@/components/ProjectRail.vue'
import JobBadge from '@/components/JobBadge.vue'
import ThinkingBadge from '@/components/ThinkingBadge.vue'
import SysMeter from '@/components/SysMeter.vue'
import { useRetryWhenBack } from '@/composables/useSystemFeed'
import { STEP_ROUTES } from '@/router'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const route = useRoute()
const router = useRouter()
const session = useSession()

/**
 * 这一集和别的集挂在同一章上时，把章号显示出来。
 *
 * 「一章一集」立起来之前留下的重复：两个集挂同一章、标题和镜头数都一样，
 * 下拉里两行一模一样。引擎那头 sync_episodes_to_chapters 早就把这种算进
 * `orphans` 回给调用方了，只是**前端一个字都没读**——所以人只看到两条重复。
 *
 * 不撞车就返回空串：平时不给每一行加长。
 */
function dupChapterNote(ep) {
  const mine = (ep?.chapter_refs ?? [])[0]
  if (!mine) return ''
  const n = session.episodes.filter((e) => (e.chapter_refs ?? [])[0] === mine).length
  return n > 1 ? ` ⚠ ${mine}` : ''
}

const ui = useUi()

const stepKey = computed(() => route.meta?.step ?? '')
const current = computed(() => STEP_ROUTES.find((s) => s.key === stepKey.value) ?? null)
const isSettings = computed(() => route.name === 'settings')
const isTasks = computed(() => route.name === 'tasks')
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
/**
 * **窄屏上把"我在第几步"那一格滚进视野。**
 *
 * 这一排是唯一的一张地图（见 .nav 上面那段），而窄屏上它是横着能滚的
 * （`overflow-x: auto`，手机上连滚动条都不画）。滚动位置一直停在 0，
 * 于是在手机上打开「这一集」——四步里的最后一步、也是整个分集流程的入口
 * ——那一格在可视区外七十多像素，屏幕上只有前面那两三步，**没有任何东西
 * 说明当前在哪一步，也看不出还有第四步**。
 *
 * 不摆箭头、不加渐隐：这一排本来就能滑，缺的只是"打开时停在该停的地方"。
 *
 * 直接算 scrollLeft，不用 `scrollIntoView`——后者会把**外层每一个**可滚动
 * 祖先一起挪，页面主体跟着跳一下。
 */
const navEl = ref(null)
function revealStep() {
  const nav = navEl.value
  const on = nav?.querySelector('.nav__item.is-on')
  if (!nav || !on) return
  const nr = nav.getBoundingClientRect()
  const r = on.getBoundingClientRect()
  const left = r.left - nr.left + nav.scrollLeft
  const right = left + r.width
  if (left < nav.scrollLeft) nav.scrollLeft = left
  else if (right > nav.scrollLeft + nav.clientWidth) nav.scrollLeft = right - nav.clientWidth
}
// 换页之后那一格才挂上 is-on，所以等一拍。宽屏上滚不动，这两行什么都不做。
watch(() => route.path, () => nextTick(revealStep), { immediate: true })
onMounted(() => {
  nextTick(revealStep)
  window.addEventListener('resize', revealStep)
})
onUnmounted(() => window.removeEventListener('resize', revealStep))

/**
 * 引擎回来之后，把那一趟砸了的 `session.refresh()` 重来一次。
 *
 * **挂在外壳上，一处管全应用。** `session` 是个 store，用不了组件那套
 * 挂载钩子；而这一份（项目、集号、侧边那几个对勾、每一集多少镜）是**每一页
 * 都在读**的，砸了之后停在上一次那一集的样子上，要等下一次换项目 / 换集 /
 * 干完点带 refresh 的事才更新。引擎重启两秒就回来了，不该让人等那么久。
 */
useRetryWhenBack(() => session.failed, () => session.refresh())

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

/**
 * 流程这会儿**读不出来**，不是"一件都没做"。
 *
 * `session.done` 是 `flow.done ?? {}`——读砸了之后它是个空对象，而空对象
 * 和"真的一步都没做"在导航上长得一模一样：没有一个对勾，那个「下一步」
 * 的点落在第一步上。于是一部写完了故事、出完了设定的剧，在引擎抽一下的
 * 时候看着像刚建。**这条规矩仓库里写了好几处**（EpShots、StoryView：
 * 「读不出来」和「还没有」是两回事），导航上漏了。
 *
 * 只认"什么都没有"这一种：5xx 之后上一份 flow 是留着的（见 session.js
 * 那段注释），那时候导航说的是上一次那一集的实情，有 `useRetryWhenBack`
 * 去把它换新，不该顺手压暗。
 */
const stepsUnknown = computed(() => session.failed && !session.flow)

/**
 * 顶栏上哪几步显示。**按状态来，不按历史**（用户 2026-09-17）：
 *   项目、故事一直在；故事有字了才有「设定」；**理解完故事就有「这一章」**；
 *   有一章出了片就有「成片」（没片的章跳过，出了再切一次）。
 * 流程读不出来（stepsUnknown）就全显示——藏起来的话人连去哪儿都不知道。
 * 正站着的那一步永远显示，不然直接输地址进来会看到一条没有自己的导航。
 *
 * ---- 「这一章」原来等的是参考图（2026-09-18 改掉） ----
 *
 * 判据原来是 `counters.refsOk`——**要等每个人三张脸、每个地方一张空景全画完**。
 * 用户报的就是它：设定页点完「理解故事」，这一章还是不出现。
 *
 * 而理解故事那一件活干的是「提结构 → 定长相 → 章对集 → **逐章写剧本**」
 *（AssetsView 的 understand 那段），跑完剧本已经躺在那儿了；这一步自己的
 * 说明也写着「**剧本**、镜头、出片，都在这一章上」（flow.cpp 里那句 hint）。
 * 也就是说：页面上第一块内容早就有了，人却进不去看——而出图是接下来按小时
 * 算的一步，让人在看不到剧本的情况下先去画一小时的图，顺序是反的。
 *
 * 换成 `counters.understood`（引擎那头 = 故事里有人 + 库里有人 + 至少一集
 * 对上了章 + 每一集都写出了剧本）。它正好是「理解故事跑完了」。
 */
const visibleSteps = computed(() =>
  STEP_ROUTES.filter((s) => {
    if (stepsUnknown.value || s.key === stepKey.value) return true
    if (s.key === 'assets') return !!session.done.story
    if (s.key === 'episode') return !!session.counters.understood
    if (s.key === 'film') return Number(session.counters.filmedChapters ?? 0) > 0
    return true
  }),
)

/** 第一个还没做完的那一步。导航上给它一个点，代替原来那条 stepbar。 */
const nextKey = computed(() => {
  // 不知道做到哪儿了，就别指路——指错的那一下会把人支回第一步。
  if (stepsUnknown.value) return ''
  const step = visibleSteps.value.find((s) => !session.done[s.key])
  return step ? step.key : ''
})

/** 顶栏下拉里一集的名字：按章说，不按集说——集是最后按时长切出来的。 */
function chapterLabel(ep, i) {
  const ref = (ep?.chapter_refs ?? [])[0] || ''
  const n = Number(/^ch(\d+)$/.exec(ref)?.[1] ?? i + 1)
  return `第 ${n} 章${ep?.title ? ' · ' + ep.title : ''}（${ep?.shots ?? 0} 镜）`
}

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

      <!-- 读不出来的时候整条压暗：那几个对勾这会儿什么都不代表。 -->
      <nav
        ref="navEl"
        class="nav"
        :class="{ 'nav--unknown': stepsUnknown }"
        :title="stepsUnknown ? '流程这会儿读不出来，这几个对勾说明不了什么' : ''"
      >
        <RouterLink
          v-for="s in visibleSteps"
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
          <option v-if="!session.episodes.length" value="">还没有章</option>
          <!-- **两个集挂同一章时要能分辨。** 一章一集是现在的规矩，而规矩
               立起来之前留下的重复在这儿长得一模一样：2026-09-16 实见
               ep08 和 ep09 都写着「了结（17 镜）」，选哪个全靠猜。
               只在真撞车时才多显示那一句，平时不加长。 -->
          <option v-for="(ep, i) in session.episodes" :key="ep.episode_id" :value="ep.episode_id">
            {{ chapterLabel(ep, i) }}{{ dupChapterNote(ep) }}
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

      <!-- **任务这一页要一直进得去。**
           它旁边那块牌子（JobBadge）只在有活在跑的时候才出现——没活的时候
           整个顶栏一个入口都没有，而那一页最有用的一栏恰恰是「做完的」：
           刚才那几件各花了多久、哪一件砸了、大模型当时想了什么。干完了才
           想去看，正是常态。 -->
      <RouterLink
        to="/tasks"
        class="btn btn--ghost topbar__icon"
        :class="{ 'is-on': isTasks }"
        title="任务：在跑的、排着的、刚做完的"
      >
        <AppIcon name="board" :size="17" />
      </RouterLink>

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
/* 流程读不出来：整条压暗，别让一排"没对勾"被当成"一步都没做"。
   **不是藏起来**——那几个链接照样要能点，人正是要靠它们去看个究竟。 */
.nav--unknown {
  opacity: 0.5;
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
  /* **集号让位给导航，而不是反过来。** 上面那句话原来只兑现了一半：
     `.ep` 是 `flex: none`，一步都不让；真正被挤掉的是 `.nav`（它
     `min-width: 0` 又能横滚，缩起来没有下限）。375px 上进「这一集」那一页
     时，导航只剩 44px——一整排四步里连一格都摆不下。
     现在让它可缩，留 5rem 的底：原生 select 上那几个字是 `ep01 · …`，
     点开是系统的整屏选单，窄一点照样挑得到。 */
  .ep {
    flex: 0 1 8rem;
    min-width: 5rem;
  }
  .main__inner {
    padding: var(--s4) var(--s4) var(--s10);
  }
  .main__inner--wide {
    padding: 0;
  }
}
</style>
