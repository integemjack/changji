<script setup>
import { computed, onMounted, watch } from 'vue'
import { useRoute } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import ErrorBoundary from '@/components/ErrorBoundary.vue'
import StepRail from '@/components/StepRail.vue'
import ToastStack from '@/components/ToastStack.vue'
import EngineLamp from '@/components/EngineLamp.vue'
import ContextBar from '@/components/ContextBar.vue'
import { STEP_ROUTES } from '@/router'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const route = useRoute()
const session = useSession()
const ui = useUi()

const stepIndex = computed(() =>
  STEP_ROUTES.findIndex((s) => s.key === route.meta?.step),
)
const current = computed(() => STEP_ROUTES[stepIndex.value] ?? null)
const isSettings = computed(() => route.name === 'settings')
// 初始化页要整块屏幕。顶栏、侧边栏、集号条这时候一个都点不动
// （还没有项目，引擎也还没有模型），摆在那儿只会让人以为哪里没加载出来。
const bare = computed(() => route.meta?.chrome === false)

// 换项目、换集都要重新算一遍进度，否则侧边栏的对勾会停在上一个项目上
watch(
  () => [session.projectPath, session.episodeId],
  () => session.refresh(),
)

onMounted(() => session.refresh())

// 手机上点完一步就该把抽屉收起来，否则挡住内容
watch(
  () => route.fullPath,
  () => {
    ui.railOpen = false
  },
)

function cycleTheme() {
  const order = ['system', 'dark', 'light']
  ui.theme = order[(order.indexOf(ui.theme) + 1) % order.length]
}
</script>

<template>
  <div class="shell">
    <header v-if="!bare" class="topbar">
      <button
        class="topbar__menu btn btn--ghost"
        type="button"
        aria-label="打开步骤导航"
        @click="ui.railOpen = !ui.railOpen"
      >
        <AppIcon :name="ui.railOpen ? 'close' : 'menu'" :size="18" />
      </button>

      <RouterLink to="/project" class="brand">
        <span class="brand__mark">场</span>
        <span class="brand__text">
          <span class="brand__name">场记</span>
          <span class="brand__sub">AI 短剧生产平台</span>
        </span>
      </RouterLink>

      <div class="spacer" />

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
      <StepRail v-if="!bare" :class="{ 'is-open': ui.railOpen }" />
      <div
        v-if="ui.railOpen && !bare"
        class="scrim"
        aria-hidden="true"
        @click="ui.railOpen = false"
      />

      <main class="main">
        <ContextBar v-if="!isSettings && !bare" />

        <div class="main__scroll">
          <div class="main__inner" :class="{ 'main__inner--bare': bare }">
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

        <nav v-if="current && !bare" class="stepbar">
          <RouterLink
            v-if="stepIndex > 0"
            class="btn btn--ghost"
            :to="STEP_ROUTES[stepIndex - 1].path"
          >
            <AppIcon name="arrowLeft" :size="15" />
            <span class="stepbar__label">{{ STEP_ROUTES[stepIndex - 1].title }}</span>
          </RouterLink>
          <span v-else class="stepbar__pad" />

          <span class="stepbar__pos numeric tiny dim">
            第 {{ stepIndex + 1 }} / {{ STEP_ROUTES.length }} 步
          </span>

          <RouterLink
            v-if="stepIndex < STEP_ROUTES.length - 1"
            class="btn btn--primary"
            :to="STEP_ROUTES[stepIndex + 1].path"
          >
            <span class="stepbar__label">{{ STEP_ROUTES[stepIndex + 1].title }}</span>
            <AppIcon name="arrowRight" :size="15" />
          </RouterLink>
          <span v-else class="stepbar__pad" />
        </nav>
      </main>
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

/* ---------- 顶栏 ---------- */

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
.topbar__menu {
  display: none;
  width: 34px;
  padding: 0;
}
.topbar__icon {
  width: 34px;
  padding: 0;
}
.topbar__icon.is-on {
  color: var(--accent);
  background: var(--accent-soft);
}

.brand {
  display: flex;
  align-items: center;
  gap: var(--s3);
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
.brand__text {
  display: flex;
  flex-direction: column;
  line-height: 1.15;
}
.brand__name {
  font-size: var(--fs-md);
  font-weight: 700;
  letter-spacing: 0.04em;
}
.brand__sub {
  font-size: var(--fs-xs);
  color: var(--text-3);
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
  display: flex;
  flex-direction: column;
}
.main__scroll {
  flex: 1;
  overflow-y: auto;
  overscroll-behavior: contain;
}
.main__inner {
  max-width: var(--content-max);
  margin: 0 auto;
  padding: var(--s6) var(--s6) var(--s12);
  padding-left: max(var(--s6), env(safe-area-inset-left));
  padding-right: max(var(--s6), env(safe-area-inset-right));
}
/* 初始化页自己排版。**必须排在上面那条后面**——同样的特指度，
   靠源码顺序覆盖，写在前面的话内边距根本不会被去掉。 */
.main__inner--bare {
  max-width: none;
  padding: 0;
  padding-left: env(safe-area-inset-left);
  padding-right: env(safe-area-inset-right);
}

.scrim {
  position: fixed;
  inset: var(--topbar-h) 0 0;
  background: rgba(0, 0, 0, 0.5);
  z-index: 24;
  display: none;
}

/* ---------- 上一步 / 下一步 ---------- */

.stepbar {
  flex: none;
  display: flex;
  align-items: center;
  gap: var(--s3);
  padding: var(--s3) var(--s6);
  padding-bottom: max(var(--s3), env(safe-area-inset-bottom));
  border-top: 1px solid var(--line);
  background: color-mix(in srgb, var(--surface) 92%, transparent);
  backdrop-filter: blur(10px);
}
.stepbar__pad {
  flex: 1;
}
.stepbar__pos {
  flex: 1;
  text-align: center;
}
.stepbar > .btn:first-child {
  flex: 1;
  justify-content: flex-start;
}
.stepbar > .btn:last-child {
  flex: 1;
  justify-content: flex-end;
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

/* ---------- 手机 ---------- */

@media (max-width: 860px) {
  .topbar__menu {
    display: inline-flex;
  }
  .brand__sub {
    display: none;
  }
  .scrim {
    display: block;
  }
  .main__inner {
    padding: var(--s4) var(--s4) var(--s10);
  }
  /* 手机上这条媒体查询排在后面，不再写一遍的话会把上面那条盖掉。 */
  .main__inner--bare {
    padding: 0;
  }
  .stepbar {
    padding: var(--s2) var(--s3);
    padding-bottom: max(var(--s2), env(safe-area-inset-bottom));
  }
  .stepbar__label {
    max-width: 5.5em;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
}
</style>
