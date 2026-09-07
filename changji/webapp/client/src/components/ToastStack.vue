<script setup>
import { onMounted, onUnmounted } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import { useUi } from '@/stores/ui'

const ui = useUi()
const ICONS = { ok: 'check', info: 'info', warn: 'warn', error: 'warn' }

// main.js 里的全局错误处理够不到 store（会绕成循环依赖），
// 所以它发一个事件，由这儿接住变成提示条。
function onGlobalError(event) {
  ui.error(event.detail)
}
onMounted(() => window.addEventListener('changji:error', onGlobalError))
onUnmounted(() => window.removeEventListener('changji:error', onGlobalError))
</script>

<template>
  <TransitionGroup name="toast" tag="div" class="toasts">
    <div
      v-for="t in ui.toasts"
      :key="t.id"
      class="toast"
      :class="`toast--${t.kind}`"
      role="status"
    >
      <AppIcon :name="ICONS[t.kind] || 'info'" :size="15" class="toast__icon" />
      <span class="toast__text">{{ t.text }}</span>
      <button
        class="toast__close"
        type="button"
        aria-label="关闭"
        @click="ui.dismiss(t.id)"
      >
        <AppIcon name="close" :size="13" />
      </button>
    </div>
  </TransitionGroup>
</template>

<style scoped>
.toasts {
  position: fixed;
  right: var(--s5);
  bottom: var(--s5);
  z-index: 100;
  display: flex;
  flex-direction: column;
  gap: var(--s2);
  max-width: min(420px, calc(100vw - var(--s8)));
  pointer-events: none;
}
.toast {
  pointer-events: auto;
  display: flex;
  align-items: flex-start;
  gap: var(--s3);
  padding: var(--s3) var(--s3) var(--s3) var(--s4);
  border-radius: var(--r);
  border: 1px solid var(--line-strong);
  background: var(--surface-2);
  box-shadow: var(--shadow-2);
  font-size: var(--fs-base);
  line-height: 1.5;
}
.toast__icon {
  margin-top: 3px;
}
.toast__text {
  flex: 1;
  min-width: 0;
  word-break: break-word;
}
.toast__close {
  background: none;
  border: none;
  padding: 3px;
  color: var(--text-3);
  cursor: pointer;
  border-radius: var(--r-sm);
}
.toast__close:hover {
  color: var(--text);
  background: var(--surface-3);
}

.toast--ok {
  border-color: color-mix(in srgb, var(--ok) 45%, transparent);
}
.toast--ok .toast__icon {
  color: var(--ok);
}
.toast--warn {
  border-color: color-mix(in srgb, var(--warn) 45%, transparent);
}
.toast--warn .toast__icon {
  color: var(--warn);
}
.toast--error {
  border-color: color-mix(in srgb, var(--danger) 55%, transparent);
  background: color-mix(in srgb, var(--danger) 10%, var(--surface-2));
}
.toast--error .toast__icon {
  color: var(--danger);
}
.toast--info .toast__icon {
  color: var(--info);
}

.toast-enter-active,
.toast-leave-active {
  transition: all 0.22s var(--ease);
}
.toast-enter-from {
  opacity: 0;
  transform: translateX(24px);
}
.toast-leave-to {
  opacity: 0;
  transform: scale(0.96);
}

@media (max-width: 860px) {
  .toasts {
    left: var(--s3);
    right: var(--s3);
    bottom: calc(var(--s3) + 56px);
    max-width: none;
  }
}
</style>
