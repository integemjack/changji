<script setup>
/**
 * 任务页面：**引擎现在在干什么、待会儿要干什么、刚才干完了什么。**
 *
 * 用户 2026-09-17：「增加任务页面显示正在做的（已经用时，结束图标按钮）、
 * 排队中的（预计什么时候开始，取消图标按钮）、已经做完的（耗时），如果是
 * 大模型有思考的还得显示思考点击展开思考内容」。
 *
 * 在这之前，"引擎在忙什么"只有顶栏那一行小字（JobBadge），而它回答不了
 * 另外三件事：排着还没开始的、这一件已经跑了多久、刚才那件花了多少时间。
 * 账在引擎那头（`cpp/src/pipeline/task_board.hpp`），这一页只是画它。
 *
 * **不自己记账。** 页面不维护任何"我以为在跑的那几件"——那正是出图那一版
 * 犯过的错（队列活在标签页的闭包里，关掉就散、说不出排队数）。这儿每一拍
 * 整份重画。
 */
import { computed, onMounted, onUnmounted, ref } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import { api } from '@/api'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()

const board = ref({ running: [], queued: [], done: [] })
/** 展开了思考的那几件：id → 正文。 */
const opened = ref({})
/** 只看这一部剧的。**默认只看**：一台机器上常常开着好几部。 */
const mineOnly = ref(true)

let timer = null

/**
 * 这一族活叫什么。和顶栏 JobBadge 里那张表是同一套说法——**同一件事在两处
 * 用两个词**是这个仓库栽过好几次的坑（CLAUDE.md 第七条）。
 */
const KIND = {
  run: '出片',
  write: '批量',
  write_one: '写这一章',
  revise: '改稿',
  outline: '出大纲',
  analyze: '读故事',
  image: '出图',
  video: '出片',
  tts: '配音',
  llm: '写文',
  say: '朗读',
  premise: '想梗概',
  script: '写剧本',
  trailer: '剪预告',
  bible: '定角色场景',
  plan: '拆镜头',
}

/** 秒数写成人看的。**一分钟以内只报秒**：「0:47」读起来比「47 秒」慢。 */
function dur(s) {
  const n = Math.max(0, Math.round(Number(s) || 0))
  if (n < 60) return n + ' 秒'
  const m = Math.floor(n / 60)
  const r = n % 60
  if (m < 60) return `${m} 分 ${r} 秒`
  return `${Math.floor(m / 60)} 小时 ${m % 60} 分`
}

/**
 * 这一件大概还要等多久才轮到。
 *
 * **不报一个准点，报一个量级。** 前面排着几件、每件大概多久，两样都是估
 * 的；报「大约 3 分钟后」会被当成承诺，而它取决于对面机器忙不忙。所以只在
 * 引擎给得出 `eta`（同一种活最近几次的中位耗时）时才说一句。
 */
function waitHint(row, index) {
  if (!row.eta) return ''
  const running = board.value.running.length || 1
  const ahead = Math.floor(index / running)
  if (ahead <= 0) return '马上轮到'
  return `大约还要等 ${dur(row.eta * ahead)}`
}

async function load() {
  try {
    const d = await api.tasks(mineOnly.value ? session.projectPath : '')
    board.value = {
      running: d?.running ?? [],
      queued: d?.queued ?? [],
      done: d?.done ?? [],
    }
    // 已经展开的那几件，思考还在长——跟着刷。**只刷展开的那几件**：
    // 一份思考几千字，全刷等于每两秒把整本账拖一遍。
    for (const id of Object.keys(opened.value)) await loadThinking(id, true)
  } catch {
    // 拉不到就保持上一拍。引擎打个嗝不该让整页闪成空的。
  }
}

async function loadThinking(id, quiet) {
  try {
    const d = await api.taskThinking(id)
    opened.value = { ...opened.value, [id]: d?.thinking ?? '' }
  } catch (e) {
    if (!quiet) ui.error(`看不到这一件的思考：${e?.message || e}`)
  }
}

function toggleThinking(row) {
  if (opened.value[row.id] !== undefined) {
    const next = { ...opened.value }
    delete next[row.id]
    opened.value = next
    return
  }
  loadThinking(row.id, false)
}

async function stop(row) {
  try {
    await api.cancelTask(row.id)
    // 不等下一拍：按下去要当场有反应。
    await load()
  } catch (e) {
    ui.error(e?.message || String(e))
  }
}

const busy = computed(() => board.value.running.length + board.value.queued.length)

/**
 * 拍子。**在跑就快、闲着就慢**，和这个仓库里别处那几条轮询一个规矩：
 * 没活的时候两秒一拍只是白烧电。
 */
function tick() {
  clearTimeout(timer)
  timer = setTimeout(async () => {
    await load()
    tick()
  }, busy.value ? 1500 : 5000)
}

onMounted(async () => {
  await load()
  tick()
})
onUnmounted(() => clearTimeout(timer))
</script>

<template>
  <div class="page tasks">
    <header class="tasks__head">
      <h1 class="h2">任务</h1>
      <span class="spacer" />
      <label class="switch tiny" title="只看当前这一部剧的活">
        <input v-model="mineOnly" type="checkbox" @change="load" />
        <span>只看这一部</span>
      </label>
    </header>

    <!-- 正在做的 -->
    <section class="card">
      <h2 class="h3">
        正在做
        <span class="tiny dim">{{ board.running.length }}</span>
      </h2>
      <p v-if="!board.running.length" class="tiny dim">这会儿没有活在跑。</p>
      <ul v-else class="rows">
        <li v-for="r in board.running" :key="r.id" class="row">
          <span class="pill pill--neutral tiny nowrap">{{ KIND[r.kind] ?? r.kind }}</span>
          <span class="row__title truncate" :title="r.title">{{ r.title }}</span>
          <!-- 采样那条进度。没有步数的时候（读权重、等对面机器）不画，
               免得一条不动的条看着像卡死。 -->
          <span v-if="r.total > 0" class="tiny dim numeric nowrap">
            {{ r.current }}/{{ r.total }}
          </span>
          <span class="tiny dim numeric nowrap">已经 {{ dur(r.seconds) }}</span>
          <button
            v-if="r.thinking"
            class="btn btn--sm btn--ghost"
            type="button"
            :title="`想了 ${r.thinking_chars} 字，点开看`"
            @click="toggleThinking(r)"
          >
            思考
          </button>
          <span v-if="r.cancelling" class="tiny dim nowrap">正在停…</span>
          <button
            v-else
            class="iconbtn"
            type="button"
            title="结束这一件（正在跑的那一步会停下）"
            @click="stop(r)"
          >
            <AppIcon name="stop" :size="14" />
          </button>
          <pre v-if="opened[r.id] !== undefined" class="think">{{
            opened[r.id] || '还没有想出字来'
          }}</pre>
        </li>
      </ul>
    </section>

    <!-- 排队中的 -->
    <section class="card">
      <h2 class="h3">
        排队中
        <span class="tiny dim">{{ board.queued.length }}</span>
      </h2>
      <p v-if="!board.queued.length" class="tiny dim">没有排着的。</p>
      <ul v-else class="rows">
        <li v-for="(r, i) in board.queued" :key="r.id" class="row">
          <span class="pill pill--neutral tiny nowrap">{{ KIND[r.kind] ?? r.kind }}</span>
          <span class="row__title truncate" :title="r.title">{{ r.title }}</span>
          <span class="tiny dim nowrap">{{ waitHint(r, i) }}</span>
          <span v-if="r.waited > 5" class="tiny dim numeric nowrap">
            排了 {{ dur(r.waited) }}
          </span>
          <!-- **按完叉它还会在名单上待一会儿。** 排着的是"有空位了才被领
               走"的，取消只把令牌立起来，真正划掉要等领它的那一路回头看
               一眼。不说一声的话看着像没按上。 -->
          <span v-if="r.cancelling" class="tiny dim nowrap">取消中…</span>
          <button
            v-else
            class="iconbtn"
            type="button"
            title="取消这一件"
            @click="stop(r)"
          >
            <AppIcon name="close" :size="14" />
          </button>
        </li>
      </ul>
    </section>

    <!-- 做完的 -->
    <section class="card">
      <h2 class="h3">
        做完的
        <span class="tiny dim">{{ board.done.length }}</span>
      </h2>
      <p v-if="!board.done.length" class="tiny dim">还没有干完的活。</p>
      <ul v-else class="rows">
        <li v-for="r in board.done" :key="r.id" class="row">
          <span class="pill pill--neutral tiny nowrap">{{ KIND[r.kind] ?? r.kind }}</span>
          <span class="row__title truncate" :title="r.title">{{ r.title }}</span>
          <!-- **停和砸分两种说法。** 「人按的停不是失败」，报成红的会让人
               去找哪儿出错了——引擎那头也是这么分的（TaskState）。 -->
          <span v-if="r.state === 'failed'" class="pill pill--warn tiny nowrap">失败</span>
          <span v-else-if="r.state === 'cancelled'" class="pill pill--neutral tiny nowrap">
            停下了
          </span>
          <span class="tiny dim numeric nowrap">{{ dur(r.seconds) }}</span>
          <button
            v-if="r.thinking"
            class="btn btn--sm btn--ghost"
            type="button"
            title="点开看它当时想了什么"
            @click="toggleThinking(r)"
          >
            思考
          </button>
          <span v-if="r.error" class="tiny warn truncate" :title="r.error">{{ r.error }}</span>
          <pre v-if="opened[r.id] !== undefined" class="think">{{
            opened[r.id] || '没有留下思考'
          }}</pre>
        </li>
      </ul>
    </section>
  </div>
</template>

<style scoped>
.tasks__head {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 12px;
}
.rows {
  list-style: none;
  margin: 0;
  padding: 0;
}
.row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 0;
  border-top: 1px solid var(--line);
  /* 思考展开之后要换行铺满，所以这一行是可换行的 flex。 */
  flex-wrap: wrap;
}
.row:first-child {
  border-top: 0;
}
.row__title {
  flex: 1 1 200px;
  min-width: 0;
}
.think {
  flex: 1 0 100%;
  max-height: 40vh;
  overflow: auto;
  margin: 4px 0 8px;
  padding: 8px;
  border-radius: var(--r-sm);
  background: var(--bg-2);
  color: var(--text-2);
  font-size: var(--fs-xs);
  /* 思考是一大段没有换行的字，不裹住的话整页会被它撑宽。 */
  white-space: pre-wrap;
  word-break: break-word;
}
</style>
