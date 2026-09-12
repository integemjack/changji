<script setup>
/**
 * 「机器 × 能力」那张表。
 *
 * 一行一台机器（**本机也是一行，不是特例**），一列一个能力，格子三态：
 *
 *   灰   干不了——缺模型、没编进去、没有 ffmpeg。悬停看原因
 *   空心 能干，但你在配置里关掉了
 *   实心 参与自动调度
 *
 * **三态是引擎算好的，界面不自己推。** 推的话迟早和调度器的判断对不上，
 * 而那种对不上表现为"表上说能派，跑起来说没有可用节点"。
 *
 * 现在是只读的：要关掉某台的某个能力，改配置里那台的 `off`。
 * 点格子开关要等写回配置那一步。
 */
import { onMounted, onUnmounted, ref } from 'vue'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'

const data = ref(null)
const error = ref('')
const loading = ref(false)
let timer = null

async function load() {
  loading.value = true
  try {
    data.value = await api.nodes()
    error.value = ''
  } catch (err) {
    error.value = err.message
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  load()
  // 十五秒一次。**别更勤**：每次都要去问别的机器，而那几台正在出片。
  timer = setInterval(load, 15000)
})
onUnmounted(() => clearInterval(timer))

/** 格子的样子。三态之外，离线那台整行压暗。 */
function cellClass(cap, node) {
  if (!cap.able) return 'cell cell--cant'
  if (cap.off) return 'cell cell--off'
  if (!node.online) return 'cell cell--cant'
  return 'cell cell--on'
}

function cellTitle(cap, node) {
  if (!cap.able) return `${cap.label}：干不了。${cap.why || ''}`
  if (cap.off) return `${cap.label}：这台能干，但配置里把它关了（off）`
  if (!node.online) return `${cap.label}：这台连不上`
  return `${cap.label}：参与自动调度`
}
</script>

<template>
  <div class="matrix">
    <div class="matrix__head">
      <h3 class="matrix__t">这几台机器能产什么</h3>
      <span class="spacer" />
      <button
        class="iconbtn"
        type="button"
        :disabled="loading"
        title="重新问一遍（每台最多等 3 秒）"
        @click="load"
      >
        <AppIcon name="refresh" :size="14" />
      </button>
    </div>

    <p v-if="error" class="alert alert--bad">
      <AppIcon name="warn" :size="15" />
      {{ error }}
    </p>

    <table v-if="data" class="matrix__grid">
      <thead>
        <tr>
          <th class="col-name">机器</th>
          <th v-for="c in data.nodes[0]?.capabilities ?? []" :key="c.cap">
            {{ c.label }}
          </th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="n in data.nodes" :key="n.url" :class="{ off: !n.online }">
          <td class="col-name">
            <span class="nm">{{ n.name }}</span>
            <span v-if="n.local" class="pill pill--neutral tiny">本机</span>
            <span v-else-if="!n.online" class="pill pill--warn tiny">连不上</span>
            <span v-else-if="n.busy" class="pill pill--ok tiny">忙</span>
            <span class="url mono tiny">{{ n.url }}</span>
            <span v-if="n.error" class="err tiny">{{ n.error }}</span>
          </td>
          <td v-for="c in n.capabilities" :key="c.cap" class="col-cap">
            <span :class="cellClass(c, n)" :title="cellTitle(c, n)" />
          </td>
        </tr>
      </tbody>
    </table>

    <ul v-if="data" class="sum">
      <li v-for="s in data.summary" :key="s.cap" :class="{ bad: s.count === 0 }">
        <b>{{ s.label }}</b>
        <span v-if="s.count > 0">{{ s.count }} 台可用</span>
        <span v-else class="why">{{ s.why }}</span>
      </li>
    </ul>

    <p class="tiny dim">
      能不能干是那台自己量出来的，这儿只能关不能开。要关掉某一样，在配置里
      那台的 <code>off</code> 里加上它。
    </p>
  </div>
</template>

<style scoped>
.matrix {
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.matrix__head {
  display: flex;
  align-items: center;
  gap: 8px;
}
.matrix__t {
  font-size: 14px;
  margin: 0;
}
.matrix__grid {
  border-collapse: collapse;
  width: 100%;
  font-size: 13px;
}
.matrix__grid th {
  text-align: center;
  font-weight: 500;
  padding: 4px 6px;
  opacity: 0.7;
}
.matrix__grid th.col-name,
.matrix__grid td.col-name {
  text-align: left;
  width: 40%;
}
.matrix__grid td {
  padding: 6px;
  border-top: 1px solid var(--line, #e5e5e5);
  vertical-align: middle;
}
.matrix__grid tr.off {
  opacity: 0.55;
}
.col-cap {
  text-align: center;
}
.nm {
  font-weight: 500;
  margin-right: 6px;
}
.url {
  opacity: 0.5;
  margin-left: 6px;
}
.err {
  display: block;
  color: var(--warn, #b45309);
  margin-top: 2px;
}
.cell {
  display: inline-block;
  width: 12px;
  height: 12px;
  border-radius: 50%;
}
/* 干不了：一个空框，连轮廓都淡 */
.cell--cant {
  border: 1px dashed var(--line, #ccc);
  opacity: 0.5;
}
/* 能干但关着：空心 */
.cell--off {
  border: 1.5px solid var(--fg, #555);
  opacity: 0.6;
}
/* 参与调度：实心 */
.cell--on {
  background: var(--ok, #16a34a);
}
.sum {
  list-style: none;
  padding: 0;
  margin: 0;
  display: flex;
  flex-wrap: wrap;
  gap: 4px 14px;
  font-size: 12px;
}
.sum li {
  opacity: 0.8;
}
.sum li b {
  margin-right: 4px;
}
.sum li.bad {
  opacity: 1;
  color: var(--warn, #b45309);
  flex-basis: 100%;
}
.why {
  opacity: 0.9;
}
</style>
