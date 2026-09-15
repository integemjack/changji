<script setup>
/**
 * 「机器 × 能力」那张表。
 *
 * 一行一台机器（**本机也是一行，不是特例**），一列一个能力，格子三态：
 *
 *   灰   干不了——缺模型、没编进去、没有 ffmpeg。悬停看原因
 *   空心 能干，但关着
 *   实心 参与自动调度
 *
 * **三态是引擎算好的，界面不自己推。** 推的话迟早和调度器的判断对不上，
 * 而那种对不上表现为"表上说能派，跑起来说没有可用节点"。
 *
 * 格子能点：点一下关掉／打开。存在 `<项目库>/nodes.json`，不碰
 * config.toml——配置里那份 `off` 是**部署时定的**，界面上显示成锁着的
 * （`locked`），要改得去动配置文件。两处取并集，任一处关了就是关了。
 *
 * 展开一行能看那台的模型：缺哪几组、一键装成和本机同一套、下到哪儿了。
 * **同一套是要紧的**：种子跨机一致这件事挡不住模型不同——种子相同、
 * 模型不同，出来的就是两张脸，而那表现为一集里画风在某几镜跳一下。
 */
import { onMounted, onUnmounted, ref } from 'vue'
import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'

const data = ref(null)
const error = ref('')
const loading = ref(false)
/** 正在提交的那个格子，`url|cap`。同一时刻只让点一个。 */
const pending = ref('')

/** 展开的那一行（节点地址），空 = 都收着。 */
const opened = ref('')
/** 展开那台的模型状态，key 是节点地址。 */
const setup = ref({})
/** 那台的下载进度，key 是节点地址。 */
const progress = ref({})
const busyNode = ref('')
let timer = null
let pollTimer = null

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
onUnmounted(() => {
  clearInterval(timer)
  clearInterval(pollTimer)
})

/**
 * 点一个格子。
 *
 * **回来的就是整张新表**，直接换掉——自己在前端推一遍"点了之后该长什么样"
 * 的话，迟早和引擎算的不一致，而那种不一致表现为"点完看着关了，跑起来
 * 还是派给它"。
 */
async function toggle(node, cap) {
  if (cap.locked || !cap.able) return
  if (pending.value) return
  pending.value = `${node.url}|${cap.cap}`
  try {
    data.value = await api.setNodeOff(node.url, cap.cap, !cap.off)
    error.value = ''
  } catch (err) {
    error.value = err.message
  } finally {
    pending.value = ''
  }
}

/** 展开／收起一行，顺手把那台的模型状态拉回来。 */
async function openRow(node) {
  if (opened.value === node.url) {
    opened.value = ''
    return
  }
  opened.value = node.url
  if (!setup.value[node.url]) await loadSetup(node.url)
  pollProgress(node.url)
}

async function loadSetup(url) {
  busyNode.value = url
  try {
    setup.value = { ...setup.value, [url]: await api.nodeSetup(url) }
    error.value = ''
  } catch (err) {
    error.value = err.message
  } finally {
    busyNode.value = ''
  }
}

/**
 * 让这台装成和本机同一套。
 *
 * **本机选的那一套就是标准**：拿它的 selected 原样发过去。让用户在这儿
 * 再挑一遍的话，两台挑得不一样就是画风跳，而那要到成片才看得出来。
 */
async function matchLocal(url) {
  const mine = setup.value.local ?? (await api.nodeSetup('local'))
  setup.value = { ...setup.value, local: mine }
  const selections = mine?.selected ?? {}
  if (!Object.keys(selections).length) {
    error.value = '本机自己还没选定模型，先把本机那套配好'
    return
  }
  busyNode.value = url
  try {
    await api.nodeSetupDownload(url, selections)
    error.value = ''
    pollProgress(url)
  } catch (err) {
    error.value = err.message
  } finally {
    busyNode.value = ''
  }
}

async function cancel(url) {
  try {
    await api.nodeSetupCancel(url)
    error.value = ''
    // **停完要再问一遍**，别让那一块继续写着「正在下 3 个文件」。
    // 轮询这会儿可能已经不在了——问不到三次就会放手（见 pollProgress）
    // ——那样的话点完停下什么都不会变。重新起一趟，它会拿到 cancelled
    // 然后自己收尾。
    pollProgress(url)
  } catch (err) {
    error.value = err.message
  }
}

/**
 * 连着问不到几次才算真断了。**一次不算**：那头在下几十 GB 的模型，
 * 一趟几分钟到几十分钟，中间引擎重启一下、网络抖一下都很正常。
 */
const kProgressMisses = 3

/** 下载进度。两秒一次，下完就停——**别一直问**，那几台正在出片。 */
function pollProgress(url) {
  clearInterval(pollTimer)
  let misses = 0
  const tick = async () => {
    if (opened.value !== url) {
      clearInterval(pollTimer)
      return
    }
    try {
      const p = await api.nodeSetupProgress(url)
      progress.value = { ...progress.value, [url]: p }
      misses = 0
      if (p?.state !== 'running') {
        clearInterval(pollTimer)
        // 下完了模型就变了，那台能干什么也跟着变
        if (p?.state === 'done') {
          await loadSetup(url)
          await load()
        }
      }
    } catch (err) {
      // **一次问不到不等于下载停了。** 这儿原来是一次失败就
      // `clearInterval` 而且一个字不说——之后那一格永远停在最后一次
      // 的进度上（「正在下 3 个文件 41%」），不动、不报错，看着像卡死，
      // 而那头多半还在好好地下。
      misses += 1
      if (misses < kProgressMisses) return
      clearInterval(pollTimer)
      error.value = `问不到这台的下载进度了：${err.message}。那头可能还在下，收起这一行再展开一次就重新问`
    }
  }
  tick()
  pollTimer = setInterval(tick, 2000)
}

function gb(bytes) {
  if (!bytes) return ''
  return `${(bytes / 1024 / 1024 / 1024).toFixed(1)} GB`
}

/** 格子的样子。三态之外，离线那台整行压暗。 */
function cellClass(cap, node) {
  if (!cap.able) return 'cell cell--cant'
  if (cap.off) return 'cell cell--off'
  if (!node.online) return 'cell cell--cant'
  return 'cell cell--on'
}

function cellTitle(cap, node) {
  if (!cap.able) return `${cap.label}：干不了。${cap.why || ''}`
  if (cap.locked) return `${cap.label}：配置文件里关掉的，要改得去动 [[peer.nodes]] 的 off`
  if (cap.off) return `${cap.label}：关着。点一下打开`
  if (!node.online) return `${cap.label}：这台连不上`
  return `${cap.label}：参与自动调度。点一下关掉`
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

    <!-- **「还没问完」和「没有别的机器」是两回事。** 这一问要挨个去连，
         每台最多等 3 秒，几台加起来十几秒是常事；这段时间里原来整块只剩
         一个标题和底下那句说明，看着就像"就本机一台、没别的"。而真到了
         那种时候，表里至少还有本机那一行。 -->
    <p v-if="loading && !data" class="tiny dim">问着…（每台最多等 3 秒）</p>

    <table v-if="data" class="matrix__grid">
      <thead>
        <tr>
          <th class="col-name">机器</th>
          <th v-for="c in data.nodes[0]?.capabilities ?? []" :key="c.cap">
            {{ c.label }}
          </th>
          <th class="col-act" />
        </tr>
      </thead>
      <tbody>
        <template v-for="n in data.nodes" :key="n.url">
          <tr :class="{ off: !n.online }">
            <td class="col-name">
              <span class="nm">{{ n.name }}</span>
              <!-- 「本机」是身份，「连不上／忙」是状态，**两件事各走各的**。
                   原来三个串在一条 v-if/v-else-if 上，本机那一行永远停在第
                   一个分支——于是本机的「忙」一次都没亮过，而本机恰恰是最
                   常在跑的那一台（`local_exec().busy()`，引擎每次都算了给
                   过来）。 -->
              <span v-if="n.local" class="pill pill--neutral tiny">本机</span>
              <span v-if="!n.online" class="pill pill--warn tiny">连不上</span>
              <span v-else-if="n.busy" class="pill pill--ok tiny">忙</span>
              <span class="url mono tiny">{{ n.url }}</span>
              <span v-if="n.error" class="err tiny">{{ n.error }}</span>
            </td>
            <td v-for="c in n.capabilities" :key="c.cap" class="col-cap">
              <button
                type="button"
                class="cellbtn"
                :class="{ 'cellbtn--locked': c.locked || !c.able }"
                :disabled="!c.able || c.locked || pending !== ''"
                :title="cellTitle(c, n)"
                @click="toggle(n, c)"
              >
                <span :class="cellClass(c, n)" />
              </button>
            </td>
            <td class="col-act">
              <button
                class="btn btn--ghost btn--sm"
                type="button"
                :disabled="!n.online"
                :title="n.online ? '看这台装了哪些模型' : '连不上，看不了'"
                @click="openRow(n)"
              >
                {{ opened === n.url ? '收起' : '模型' }}
              </button>
            </td>
          </tr>

          <tr v-if="opened === n.url" class="drawer">
            <td :colspan="(n.capabilities?.length ?? 5) + 2">
              <div v-if="busyNode === n.url" class="tiny dim">问着…</div>
              <div v-else-if="setup[n.url]" class="setup">
                <div class="setup__groups">
                  <span
                    v-for="g in setup[n.url].groups ?? []"
                    :key="g.key"
                    class="pill tiny"
                    :class="g.satisfied ? 'pill--ok' : 'pill--warn'"
                    :title="g.purpose"
                  >
                    {{ g.title }}{{ g.satisfied ? '' : ' 缺' }}
                  </span>
                  <span class="tiny dim">
                    盘上还剩 {{ gb(setup[n.url].diskFreeBytes) }}
                  </span>
                </div>

                <div v-if="progress[n.url]?.state === 'running'" class="dl">
                  <span class="tiny">
                    正在下
                    {{ (progress[n.url].items ?? []).filter((i) => i.state === 'running').length }}
                    个文件
                  </span>
                  <button class="btn btn--ghost btn--sm" type="button" @click="cancel(n.url)">
                    停下
                  </button>
                  <ul class="dl__items">
                    <li
                      v-for="it in (progress[n.url].items ?? []).filter((i) => i.state === 'running')"
                      :key="it.name"
                      class="tiny mono"
                    >
                      {{ it.name }}
                      {{ it.total ? Math.round((it.downloaded / it.total) * 100) : 0 }}%
                    </li>
                  </ul>
                </div>

                <div v-else class="setup__acts">
                  <button
                    v-if="!n.local"
                    class="btn btn--sm"
                    type="button"
                    :disabled="busyNode === n.url"
                    title="拿本机选定的那一套原样装过去。同一套模型是画风一致的前提"
                    @click="matchLocal(n.url)"
                  >
                    装成和本机同一套
                  </button>
                  <span v-if="progress[n.url]?.state === 'failed'" class="tiny warn-text">
                    上次下载失败：{{ progress[n.url].error }}
                  </span>
                  <span v-else-if="progress[n.url]?.state === 'done'" class="tiny">
                    下完了
                  </span>
                </div>
              </div>
            </td>
          </tr>
        </template>
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
      点格子关掉或打开。<b>能不能干是那台自己量出来的</b>，灰的点不动——
      那要去装模型或者换一份编进了 sd.cpp 的二进制。配置文件里关掉的
      （<code>[[peer.nodes]]</code> 的 <code>off</code>）也点不动。
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
  width: 36%;
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
.col-act {
  text-align: right;
  white-space: nowrap;
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
.cellbtn {
  background: none;
  border: 0;
  padding: 4px;
  cursor: pointer;
  line-height: 0;
}
.cellbtn--locked,
.cellbtn:disabled {
  cursor: not-allowed;
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
.drawer td {
  background: var(--bg-soft, rgba(127, 127, 127, 0.06));
}
.setup {
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.setup__groups {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
}
.setup__acts {
  display: flex;
  align-items: center;
  gap: 10px;
}
.dl {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
}
.dl__items {
  list-style: none;
  margin: 0;
  padding: 0;
  flex-basis: 100%;
  opacity: 0.8;
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
