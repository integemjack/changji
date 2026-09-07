<script setup>
/** 每一步页面的抬头。八页长得一样，用户才不用每页重新找方向。 */
import { computed } from 'vue'
import { useRoute } from 'vue-router'

import AppIcon from '@/components/AppIcon.vue'
import { STEP_ROUTES } from '@/router'
import { useSession } from '@/stores/session'

const props = defineProps({
  title: { type: String, default: '' },
  tagline: { type: String, default: '' },
})

const route = useRoute()
const session = useSession()

const index = computed(() => STEP_ROUTES.findIndex((s) => s.key === route.meta?.step))
const step = computed(() => STEP_ROUTES[index.value] ?? null)
const isDone = computed(() => Boolean(step.value && session.done[step.value.key]))
</script>

<template>
  <header class="head">
    <div class="head__row">
      <span v-if="step" class="head__badge" :class="{ 'head__badge--done': isDone }">
        <AppIcon v-if="isDone" name="check" :size="14" />
        <template v-else>{{ index + 1 }}</template>
      </span>
      <div class="head__text">
        <h1 class="head__title">{{ title || step?.title }}</h1>
        <p class="head__tagline">{{ tagline || step?.tagline }}</p>
      </div>
      <div class="head__actions">
        <slot name="actions" />
      </div>
    </div>
    <div v-if="$slots.note" class="head__note">
      <slot name="note" />
    </div>
  </header>
</template>

<style scoped>
.head {
  margin-bottom: var(--s6);
}
.head__row {
  display: flex;
  align-items: flex-start;
  gap: var(--s4);
}
.head__badge {
  flex: none;
  display: grid;
  place-items: center;
  width: 34px;
  height: 34px;
  margin-top: 3px;
  border-radius: 11px;
  background: var(--accent-soft);
  border: 1px solid var(--accent-line);
  color: var(--accent);
  font-weight: 700;
  font-size: var(--fs-md);
}
.head__badge--done {
  background: var(--ok-soft);
  border-color: color-mix(in srgb, var(--ok) 35%, transparent);
  color: var(--ok);
}
.head__text {
  min-width: 0;
  flex: 1;
}
.head__title {
  font-size: var(--fs-2xl);
  font-weight: 700;
  letter-spacing: -0.01em;
  line-height: 1.25;
}
.head__tagline {
  margin-top: 2px;
  color: var(--text-2);
  font-size: var(--fs-base);
}
.head__actions {
  display: flex;
  align-items: center;
  gap: var(--s2);
  flex-wrap: wrap;
  justify-content: flex-end;
}
.head__note {
  margin-top: var(--s4);
}

@media (max-width: 640px) {
  .head__row {
    flex-wrap: wrap;
  }
  .head__title {
    font-size: var(--fs-xl);
  }
  .head__actions {
    width: 100%;
    justify-content: stretch;
  }
  .head__actions :deep(.btn) {
    flex: 1;
  }
}
</style>
