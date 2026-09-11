<script setup>
/**
 * 顶栏那三个小表：GPU、CPU、内存。
 *
 * 用户 2026-09-11：「在顶部导航栏增加 GPU，CPU，内存的使用情况实时显示」，
 * 「使用 ws 方式」。
 *
 * **推，不轮询。** 订 "system" 这一类，引擎两秒推一条；没人订它就不采样。
 * 断了五秒后再连；连不上就整块不显示——顶栏上一个不动的数比没有更误导。
 *
 * 每一项一个数加一条 3px 的小杠。杠是给眼角看的：写字的时候不会去读
 * 数字，但余光看得见杠满没满；满了变红。
 */
import { onMounted, onUnmounted, ref } from 'vue'

import { openJobSocket } from '@/composables/useJobSocket'

const stat = ref(null)
let sock = null
let retry = null
let gone = false

function connect() {
  if (gone) return
  sock = openJobSocket(
    'system',
    (msg) => {
      if (msg.type === 'system') stat.value = msg
    },
    () => {
      sock = null
      stat.value = null
      clearTimeout(retry)
      retry = setTimeout(connect, 5000)
    },
  )
}

onMounted(connect)
onUnmounted(() => {
  gone = true
  clearTimeout(retry)
  sock?.close()
})

function pct(used, total) {
  return total > 0 ? Math.min(100, Math.round((used / total) * 100)) : 0
}
function gb(v) {
  return v >= 100 ? String(Math.round(v)) : v.toFixed(1)
}
</script>

<template>
  <div v-if="stat" class="sys">
    <span
      v-for="g in stat.gpus"
      :key="g.index"
      class="sys__item"
      :title="`${g.name || 'GPU'} · 利用率 ${g.util_percent < 0 ? '—' : g.util_percent + '%'} · 显存 ${gb(g.vram_used_gb)} / ${gb(g.vram_total_gb)} GB`"
    >
      <span class="sys__k">GPU{{ stat.gpus.length > 1 ? g.index : '' }}</span>
      <span class="sys__v">{{ g.util_percent < 0 ? '—' : g.util_percent + '%' }}</span>
      <span class="sys__bar">
        <i
          :class="{ 'is-hot': g.util_percent >= 90 }"
          :style="{ width: Math.max(0, g.util_percent) + '%' }"
        />
      </span>
      <span class="sys__v">{{ gb(g.vram_used_gb) }}/{{ gb(g.vram_total_gb) }}G</span>
      <span class="sys__bar">
        <i
          :class="{ 'is-hot': pct(g.vram_used_gb, g.vram_total_gb) >= 90 }"
          :style="{ width: pct(g.vram_used_gb, g.vram_total_gb) + '%' }"
        />
      </span>
    </span>

    <span class="sys__item" title="CPU">
      <span class="sys__k">CPU</span>
      <span class="sys__v">{{ stat.cpu_percent < 0 ? '—' : Math.round(stat.cpu_percent) + '%' }}</span>
      <span class="sys__bar">
        <i
          :class="{ 'is-hot': stat.cpu_percent >= 90 }"
          :style="{ width: Math.max(0, stat.cpu_percent) + '%' }"
        />
      </span>
    </span>

    <span class="sys__item" :title="`内存 ${gb(stat.mem_used_gb)} / ${gb(stat.mem_total_gb)} GB`">
      <span class="sys__k">内存</span>
      <span class="sys__v">{{ gb(stat.mem_used_gb) }}/{{ gb(stat.mem_total_gb) }}G</span>
      <span class="sys__bar">
        <i
          :class="{ 'is-hot': pct(stat.mem_used_gb, stat.mem_total_gb) >= 90 }"
          :style="{ width: pct(stat.mem_used_gb, stat.mem_total_gb) + '%' }"
        />
      </span>
    </span>
  </div>
</template>

<style scoped>
.sys {
  display: flex;
  align-items: center;
  gap: var(--s3);
  flex: none;
  font-size: var(--fs-xs);
  color: var(--text-3);
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}
.sys__item {
  display: inline-flex;
  align-items: center;
  gap: 5px;
}
.sys__k {
  color: var(--text-3);
}
.sys__v {
  color: var(--text-2);
}
.sys__bar {
  width: 34px;
  height: 3px;
  border-radius: 2px;
  background: var(--surface-3);
  overflow: hidden;
}
.sys__bar i {
  display: block;
  height: 100%;
  background: var(--accent);
  transition: width 0.5s var(--ease);
}
.sys__bar i.is-hot {
  background: var(--danger);
}

/* 窄屏上让位给导航。 */
@media (max-width: 1000px) {
  .sys {
    display: none;
  }
}
</style>
