<script setup>
/**
 * 引擎状态灯。
 *
 * 界面上八成的报错都源于「引擎没开」或「地址填错了」。
 * 顶栏常驻一盏灯，出问题时一眼就知道该去哪儿改，
 * 而不是在某个按钮上点出一句看不懂的超时。
 */
import { onMounted, onUnmounted, ref } from 'vue'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'

const status = ref(null)
const checking = ref(false)
let timer = null

async function check() {
  checking.value = true
  try {
    status.value = await api.engineStatus()
  } catch (err) {
    status.value = { online: false, error: err.message, baseUrl: '' }
  } finally {
    checking.value = false
  }
}

onMounted(() => {
  check()
  timer = setInterval(check, 15000)
})
onUnmounted(() => clearInterval(timer))
</script>

<template>
  <RouterLink
    to="/settings"
    class="lamp"
    :class="status?.online ? 'lamp--on' : 'lamp--off'"
    :title="
      status?.online
        ? `引擎已连接 ${status.baseUrl}（${status.latencyMs}ms）`
        : `引擎连不上：${status?.error || '检查中'}`
    "
  >
    <span class="lamp__dot" :class="{ 'lamp__dot--pulse': checking }" />
    <span class="lamp__text">{{ status?.online ? '引擎已连接' : '引擎离线' }}</span>
    <AppIcon v-if="!status?.online" name="warn" :size="13" />
  </RouterLink>
</template>

<style scoped>
.lamp {
  display: inline-flex;
  align-items: center;
  gap: var(--s2);
  height: 26px;
  padding: 0 var(--s3);
  border-radius: var(--r-pill);
  border: 1px solid var(--line);
  background: var(--surface-2);
  font-size: var(--fs-sm);
  font-weight: 500;
  color: var(--text-2);
  text-decoration: none;
}
.lamp:hover {
  text-decoration: none;
  border-color: var(--line-strong);
}
.lamp__dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--text-3);
}
.lamp--on .lamp__dot {
  background: var(--ok);
  box-shadow: 0 0 0 3px var(--ok-soft);
}
.lamp--off {
  color: var(--danger);
  border-color: color-mix(in srgb, var(--danger) 40%, transparent);
  background: var(--danger-soft);
}
.lamp--off .lamp__dot {
  background: var(--danger);
  box-shadow: 0 0 0 3px var(--danger-soft);
}
.lamp__dot--pulse {
  animation: pulse 1s var(--ease) infinite;
}
@keyframes pulse {
  50% {
    opacity: 0.35;
  }
}

@media (max-width: 640px) {
  .lamp__text {
    display: none;
  }
  .lamp {
    padding: 0 var(--s2);
  }
}
</style>
