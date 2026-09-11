<script setup>
/**
 * 空状态。一行：什么是空的，下一步点哪儿。
 *
 * 以前是一个虚线框、一个大图标、两段字。2026-09-11 起所有页面都照故事页
 * 的样子：不解释，只说缺什么、去哪儿。
 */
import AppIcon from '@/components/AppIcon.vue'

defineProps({
  icon: { type: String, default: 'info' },
  title: { type: String, required: true },
  hint: { type: String, default: '' },
  tone: { type: String, default: 'neutral' }, // neutral / warn
})
</script>

<template>
  <div class="empty" :class="`empty--${tone}`">
    <AppIcon :name="icon" :size="15" class="empty__icon" />
    <span class="empty__title">{{ title }}</span>
    <span v-if="hint" class="empty__hint">{{ hint }}</span>
    <span v-if="$slots.default" class="empty__actions">
      <slot />
    </span>
  </div>
</template>

<style scoped>
.empty {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: var(--s3);
  padding: var(--s2) 0;
  font-size: var(--fs-sm);
  color: var(--text-2);
}
.empty__icon {
  flex: none;
  color: var(--text-3);
}
.empty--warn .empty__icon,
.empty--warn .empty__title {
  color: var(--warn);
}
.empty__title {
  font-weight: 600;
  color: var(--text);
}
.empty__hint {
  color: var(--text-3);
}
.empty__actions {
  display: inline-flex;
  gap: var(--s2);
}
</style>
