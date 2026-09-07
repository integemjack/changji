<script setup>
defineProps({
  percent: { type: Number, default: 0 },
  label: { type: String, default: '' },
  detail: { type: String, default: '' },
  tone: { type: String, default: 'accent' }, // accent / ok / danger
  // 进度不可知时画一条来回跑的条，好过停在 0% 让人以为卡死了
  indeterminate: { type: Boolean, default: false },
})
</script>

<template>
  <div class="prog">
    <div v-if="label || detail" class="prog__top">
      <span class="prog__label truncate">{{ label }}</span>
      <span class="prog__detail tiny dim numeric nowrap">{{ detail }}</span>
    </div>
    <div class="prog__track" :class="`prog__track--${tone}`">
      <div
        class="prog__fill"
        :class="{ 'prog__fill--indeterminate': indeterminate }"
        :style="indeterminate ? undefined : { width: Math.max(0, Math.min(100, percent)) + '%' }"
      />
    </div>
  </div>
</template>

<style scoped>
.prog {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.prog__top {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--s3);
}
.prog__label {
  font-size: var(--fs-base);
  font-weight: 500;
}
.prog__track {
  height: 6px;
  border-radius: var(--r-pill);
  background: var(--surface-3);
  overflow: hidden;
}
.prog__fill {
  height: 100%;
  border-radius: var(--r-pill);
  background: var(--accent);
  transition: width 0.4s var(--ease);
}
.prog__track--ok .prog__fill {
  background: var(--ok);
}
.prog__track--danger .prog__fill {
  background: var(--danger);
}
.prog__fill--indeterminate {
  width: 35%;
  animation: slide 1.4s var(--ease) infinite;
}
@keyframes slide {
  0% {
    transform: translateX(-110%);
  }
  100% {
    transform: translateX(320%);
  }
}
</style>
