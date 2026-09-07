<script setup>
/**
 * 空状态。
 *
 * 空页面必须说三件事：现在是空的、为什么是空的、下一步点哪儿。
 * 只画一个灰色的框会让人以为是加载失败。
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
    <span class="empty__icon">
      <AppIcon :name="icon" :size="22" />
    </span>
    <p class="empty__title">{{ title }}</p>
    <p v-if="hint" class="empty__hint">{{ hint }}</p>
    <div v-if="$slots.default" class="empty__actions">
      <slot />
    </div>
  </div>
</template>

<style scoped>
.empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  gap: var(--s2);
  padding: var(--s10) var(--s6);
  border: 1px dashed var(--line-strong);
  border-radius: var(--r-lg);
  background: var(--surface);
}
.empty__icon {
  display: grid;
  place-items: center;
  width: 44px;
  height: 44px;
  margin-bottom: var(--s2);
  border-radius: 14px;
  background: var(--surface-3);
  color: var(--text-3);
}
.empty--warn {
  border-color: color-mix(in srgb, var(--warn) 40%, transparent);
  background: color-mix(in srgb, var(--warn) 6%, var(--surface));
}
.empty--warn .empty__icon {
  background: var(--warn-soft);
  color: var(--warn);
}
.empty__title {
  font-size: var(--fs-lg);
  font-weight: 600;
}
.empty__hint {
  max-width: 46ch;
  color: var(--text-2);
  font-size: var(--fs-base);
  line-height: 1.7;
}
.empty__actions {
  display: flex;
  gap: var(--s2);
  flex-wrap: wrap;
  justify-content: center;
  margin-top: var(--s3);
}
</style>
