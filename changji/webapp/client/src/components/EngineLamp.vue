<script setup>
/**
 * 引擎状态灯。
 *
 * 界面上八成的报错都源于「引擎没开」或「地址填错了」。
 * 顶栏常驻一盏灯，出问题时一眼就知道该去哪儿改，
 * 而不是在某个按钮上点出一句看不懂的超时。
 */
import { computed, onMounted, onUnmounted, ref } from 'vue'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'

const status = ref(null)
const checking = ref(false)

/**
 * 鼠标停上去那句话。
 *
 * **自带 webapp 的二进制上，地址和延迟是两个占位的空值**：引擎就是发这个
 * 页面的那个进程，`/bff/settings/status` 回的是 `baseUrl: ""` 加
 * `latencyMs: 0`（没有一趟网络往返可量）。原来那句话不判，于是鼠标停上去
 * 写的是「引擎已连接 （0ms）」——一个空地址和一个没意义的 0。
 * 起 Node 那层转发时那两个值才是真的，那时照常显示。
 * （同一件事设置页那两个输入框早判过：embedded 时整块不摆出来。）
 */
const hint = computed(() => {
  const s = status.value
  if (!s) return '正在看引擎在不在'
  if (!s.online) return `引擎连不上：${s.error || '没说原因'}`
  const where = s.baseUrl ? ` ${s.baseUrl}` : ''
  const ms = s.latencyMs ? `（${s.latencyMs}ms）` : ''
  return where || ms ? `引擎已连接${where}${ms}` : '引擎已连接：就是发这个页面的那个进程'
})
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
  <!-- **「还不知道」不是「离线」。**
       `status` 初值是 null，而原来那三行判的都是 `status?.online`——于是
       每次打开页面，这盏灯先写着「引擎离线」加一个警告图标，直到第一次
       探测回来。平时那是一瞬间，可引擎正忙时一个请求卡十几秒是常事
       （长任务占着 Crow 的一条 I/O 线程，见 server.cpp 那段），那就是
       十几秒的假离线——而这盏灯是整屏唯一一处回答"引擎在不在"的地方。
       第三种状态：没探过就说「检查中」，点不亮也不报警。 -->
  <RouterLink
    to="/settings"
    class="lamp"
    :class="status ? (status.online ? 'lamp--on' : 'lamp--off') : ''"
    :title="hint"
  >
    <span class="lamp__dot" :class="{ 'lamp__dot--pulse': checking }" />
    <!-- **接上了就只剩那个点。**
         用户 2026-09-17：「引擎已经连接只显示前面的状态」。「引擎已连接」
         这四个字在顶栏上一直占着位置，而它说的是**常态**——常态不需要一行
         字，一个亮着的点就够，全文在 title 上。
         出事的两种照旧写出来：「引擎离线」要人看见，「检查中」要人别急。 -->
    <span v-if="!status || !status.online" class="lamp__text">
      {{ status ? '引擎离线' : '检查中' }}
    </span>
    <AppIcon v-if="status && !status.online" name="warn" :size="13" />
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
/* **这四个字不能折。** `.lamp` 的高是钉死的 26px，折成两行就是 38px，
   直接从药丸底下溢出去——而它是整屏唯一一处回答「引擎在不在」的地方。
   在这一章页上必折：那一页的顶栏比别的页多一个章节选择器，把这一格
   挤到放不下五个字（实测视口 1142px 就折了，还远不算窄屏）。
   窄到 640px 以下时下面那条 media 会把整段字藏掉，不靠折行让位。 */
.lamp__text {
  white-space: nowrap;
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
