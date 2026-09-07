<script setup>
/**
 * 左侧的八步导航。
 *
 * 每一步的圆点有三种样子：做完了打勾、正在这一步高亮、还没到是空心。
 * 不锁死顺序——用户想回头改第二步的剧本，不该被「你还没做完第五步」拦住。
 * 引导的意思是给出建议路径，不是把人关在流水线上。
 */
import { computed } from 'vue'
import { useRoute } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import { STEP_ROUTES } from '@/router'
import { useSession } from '@/stores/session'

const route = useRoute()
const session = useSession()

const currentIndex = computed(() =>
  STEP_ROUTES.findIndex((s) => s.key === route.meta?.step),
)
const doneCount = computed(
  () => STEP_ROUTES.filter((s) => session.done[s.key]).length,
)
const percent = computed(() =>
  Math.round((doneCount.value / STEP_ROUTES.length) * 100),
)

function stateOf(step, index) {
  if (session.done[step.key]) return 'done'
  if (index === currentIndex.value) return 'current'
  return 'todo'
}
</script>

<template>
  <aside class="rail">
    <div class="rail__progress">
      <div class="rail__progress-top">
        <span class="tiny dim">整体进度</span>
        <span class="tiny strong numeric">{{ doneCount }} / {{ STEP_ROUTES.length }}</span>
      </div>
      <div class="bar">
        <div class="bar__fill" :style="{ width: percent + '%' }" />
      </div>
    </div>

    <nav class="rail__nav">
      <RouterLink
        v-for="(step, i) in STEP_ROUTES"
        :key="step.key"
        class="step"
        :class="[`step--${stateOf(step, i)}`, { 'step--active': i === currentIndex }]"
        :to="step.path"
      >
        <span class="step__line" :class="{ 'step__line--first': i === 0 }" />
        <span class="step__bullet">
          <AppIcon v-if="session.done[step.key]" name="check" :size="12" />
          <span v-else class="step__num numeric">{{ i + 1 }}</span>
        </span>
        <span class="step__text">
          <span class="step__title">{{ step.title }}</span>
          <span class="step__tagline">{{ step.tagline }}</span>
        </span>
        <AppIcon class="step__icon" :name="step.icon" :size="15" />
      </RouterLink>
    </nav>

    <RouterLink to="/settings" class="rail__settings">
      <AppIcon name="gear" :size="15" />
      <span>设置</span>
      <span class="spacer" />
      <span class="tiny dim">环境与参数</span>
    </RouterLink>
  </aside>
</template>

<style scoped>
.rail {
  flex: none;
  width: var(--rail-w);
  display: flex;
  flex-direction: column;
  border-right: 1px solid var(--line);
  background: var(--bg-sunken);
  overflow-y: auto;
  padding-bottom: var(--s4);
}

.rail__progress {
  padding: var(--s4) var(--s4) var(--s3);
}
.rail__progress-top {
  display: flex;
  justify-content: space-between;
  margin-bottom: var(--s2);
}
.bar {
  height: 4px;
  border-radius: var(--r-pill);
  background: var(--surface-3);
  overflow: hidden;
}
.bar__fill {
  height: 100%;
  border-radius: var(--r-pill);
  background: linear-gradient(
    90deg,
    var(--accent),
    color-mix(in srgb, var(--accent) 55%, var(--ok))
  );
  transition: width 0.3s var(--ease);
}

.rail__nav {
  display: flex;
  flex-direction: column;
  padding: var(--s2) var(--s3);
  flex: 1;
}

.step {
  position: relative;
  display: flex;
  align-items: center;
  gap: var(--s3);
  padding: var(--s2) var(--s3);
  border-radius: var(--r);
  color: var(--text-2);
  text-decoration: none;
  transition: background 0.15s var(--ease), color 0.15s var(--ease);
}
.step:hover {
  background: var(--surface-2);
  color: var(--text);
  text-decoration: none;
}
.step--active {
  background: var(--accent-soft);
  color: var(--text);
}
.step--active .step__title {
  color: var(--accent);
}

/* 连接圆点的竖线。画在圆点上方，第一个不画。 */
.step__line {
  position: absolute;
  left: calc(var(--s3) + 10px);
  top: -6px;
  width: 1px;
  height: 14px;
  background: var(--line-strong);
}
.step__line--first {
  display: none;
}

.step__bullet {
  flex: none;
  display: grid;
  place-items: center;
  width: 21px;
  height: 21px;
  border-radius: 50%;
  border: 1px solid var(--line-strong);
  background: var(--surface);
  font-size: 10px;
  font-weight: 700;
  color: var(--text-3);
  transition: all 0.18s var(--ease);
}
.step--done .step__bullet {
  background: var(--ok);
  border-color: var(--ok);
  color: #06210d;
}
.step--current .step__bullet,
.step--active .step__bullet {
  border-color: var(--accent);
  color: var(--accent);
  box-shadow: 0 0 0 3px var(--accent-soft);
}
.step--done.step--active .step__bullet {
  color: #06210d;
}

.step__text {
  display: flex;
  flex-direction: column;
  min-width: 0;
  line-height: 1.3;
}
.step__title {
  font-size: var(--fs-base);
  font-weight: 600;
}
.step__tagline {
  font-size: var(--fs-xs);
  color: var(--text-3);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.step__icon {
  margin-left: auto;
  opacity: 0.35;
}
.step--active .step__icon {
  opacity: 0.9;
}

.rail__settings {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin: var(--s2) var(--s3) 0;
  padding: var(--s3);
  border-radius: var(--r);
  border: 1px solid var(--line);
  color: var(--text-2);
  font-size: var(--fs-base);
  text-decoration: none;
}
.rail__settings:hover {
  background: var(--surface-2);
  color: var(--text);
  text-decoration: none;
}

@media (max-width: 860px) {
  .rail {
    position: fixed;
    top: var(--topbar-h);
    bottom: 0;
    left: 0;
    z-index: 25;
    width: min(84vw, 300px);
    transform: translateX(-100%);
    transition: transform 0.22s var(--ease);
    box-shadow: var(--shadow-3);
    background: var(--surface);
  }
  .rail.is-open {
    transform: none;
  }
}
</style>
