<script setup>
/**
 * 顶栏那三个小表：GPU、CPU、内存。
 *
 * 用户 2026-09-11：「在顶部导航栏增加 GPU，CPU，内存的使用情况实时显示」，
 * 「使用 ws 方式」。
 *
 * **能推就推。** 订 "system" 这一类，引擎两秒推一条；没人订它就不采样。
 * 断了五秒后再连，这几秒里退回拉 `/api/system`（同一份 body）——
 * 连不上 WebSocket 的部署上这一块原来是永远空白的。
 * 两条都没有才整块不显示：顶栏上一个不动的数比没有更误导。
 *
 * 每一项一个数加一条 3px 的小杠。杠是给眼角看的：写字的时候不会去读
 * 数字，但余光看得见杠满没满；满了变红。
 */
import { computed } from 'vue'

import { useSystemFeed } from '@/composables/useSystemFeed'

/**
 * 那份表在哪儿接的：`useSystemFeed`（模块级一份）。
 *
 * **这儿原来自己开一条 socket**，和 JobBadge 逐字重复一份——它们的注释
 * 也写着「两个组件是同一份写法，也就有同一个毛病」（讣告串台那个，确实
 * 分别修了两遍）。两条订的还是同一个频道，引擎两秒一次那份表要发两遍。
 *
 * 收在一处顺带补上了**连不上 WebSocket 时退回轮询**：原来这一块在代理
 * 掐了 Upgrade 的部署上是永远空白的。
 */
const { stat } = useSystemFeed()

/**
 * 这份读数旧了没有。
 *
 * **"读不动"和"真没在动"是两回事，而屏幕上长得一模一样。** 显卡满负荷时
 * 问 NVML 会被驱动挂住好几秒（服务器上实测大模型生成时 13 秒都有），
 * 那几秒里引擎推过来的必然是旧读数。不说的话，用户看到的是一个不动的
 * 数字——2026-09-11 就为这个来问过一次「GPU 基本 0、显存基本 22.1 都
 * 没变过」。
 *
 * 三秒以内不吭声：正常就是一秒半采一次，一两秒的抖动说出来只是噪音。
 */
const stale = computed(() => (stat.value?.age_s ?? 0) >= 3)

function pct(used, total) {
  return total > 0 ? Math.min(100, Math.round((used / total) * 100)) : 0
}
function gb(v) {
  return v >= 100 ? String(Math.round(v)) : v.toFixed(1)
}
</script>

<template>
  <div
    v-if="stat"
    class="sys"
    :class="{ 'is-stale': stale }"
    :title="stale ? `显卡正忙，问不动它——这是 ${Math.round(stat.age_s)} 秒前的读数` : ''"
  >
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
/* 旧读数：整块压暗，鼠标放上去说清楚是多久以前的。**不是清空**——
   显卡忙的时候旧读数仍然有用（"刚才是 98%"），清掉反而少了一半信息。 */
.sys.is-stale {
  opacity: 0.45;
}
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
