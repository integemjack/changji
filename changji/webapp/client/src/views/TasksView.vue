<script setup>
/**
 * 任务页面：**引擎现在在干什么、待会儿要干什么、刚才干完了什么。**
 *
 * 用户 2026-09-17：「增加任务页面显示正在做的（已经用时，结束图标按钮）、
 * 排队中的（预计什么时候开始，取消图标按钮）、已经做完的（耗时），如果是
 * 大模型有思考的还得显示思考点击展开思考内容」，随后「任务页面也要显示实际
 * 进度」「完善任务页面内容显示和美化 ui」。
 *
 * 账在引擎那头（`cpp/src/pipeline/task_board.hpp`），这一页只是画它。
 *
 * **不自己记账。** 页面不维护任何"我以为在跑的那几件"——那正是出图那一版
 * 犯过的错（队列活在标签页的闭包里，关掉就散、说不出排队数）。这儿每一拍
 * 整份重画。
 */
import { computed, onMounted, onUnmounted, ref } from 'vue'

import AppIcon from '@/components/AppIcon.vue'
import EmptyState from '@/components/EmptyState.vue'
import { api } from '@/api'
import { readLocal, writeLocal } from '@/composables/local-storage'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

const session = useSession()
const ui = useUi()

const board = ref({ running: [], queued: [], done: [] })
/**
 * 展开了思考的那几件：id → `{ text, end }`。
 *
 * `end` 是手上这份的**绝对结尾**，下一拍拿它当 `from` 只取新增——一件活的
 * 思考动辄十几万字，整份重取的话每一拍都要搬十几万字过去。
 */
const opened = ref({})
/**
 * 上一次展开的是哪几件。**刷新之后要接回来**：用户 2026-09-17「刷新后思考
 * 看不到了」——展开状态原来只活在这个标签页的闭包里，而这一族活一跑十几
 * 分钟，中间刷一下页面是常态，不是边角情况（`useRefStream` 开头那段说的是
 * 同一件事）。
 *
 * 只记 id，不记正文：正文在引擎那头，接回来照样取得到。
 */
const kOpenKey = 'changji.tasks.openThinking'
function rememberOpen() {
  writeLocal(kOpenKey, JSON.stringify(Object.keys(opened.value)))
}
/** 只看这一部电影的。**默认只看**：一台机器上常常开着好几部。 */
const mineOnly = ref(true)
/** 第一拍还没回来。空表和"真的没活"要分开说。 */
const loaded = ref(false)

let timer = null

/**
 * 这一族活叫什么、用哪个图标。
 *
 * 和顶栏那块牌子（JobBadge 的 KIND）是同一套说法——**同一件事在两处用两个
 * 词**是这个仓库栽过好几次的坑（CLAUDE.md 第七条）。
 */
const KIND = {
  run: { label: '出片', icon: 'film' },
  write: { label: '批量', icon: 'sparkle' },
  write_one: { label: '写这一章', icon: 'sparkle' },
  revise: { label: '改稿', icon: 'sparkle' },
  outline: { label: '出大纲', icon: 'sparkle' },
  analyze: { label: '读故事', icon: 'sparkle' },
  image: { label: '出图', icon: 'image' },
  video: { label: '出片', icon: 'film' },
  tts: { label: '配音', icon: 'play' },
  llm: { label: '写文', icon: 'sparkle' },
  say: { label: '朗读', icon: 'play' },
  premise: { label: '想梗概', icon: 'sparkle' },
  script: { label: '写剧本', icon: 'script' },
  trailer: { label: '剪预告', icon: 'film' },
  bible: { label: '定角色场景', icon: 'user' },
  plan: { label: '拆镜头', icon: 'board' },
}
const kindOf = (k) => KIND[k] ?? { label: k, icon: 'sparkle' }

/** 秒数写成人看的。**一分钟以内只报秒**：「0:47」读起来比「47 秒」慢。 */
function dur(s) {
  const n = Math.max(0, Math.round(Number(s) || 0))
  if (n < 60) return n + ' 秒'
  const m = Math.floor(n / 60)
  if (m < 60) return `${m} 分 ${n % 60} 秒`
  return `${Math.floor(m / 60)} 小时 ${m % 60} 分`
}

/**
 * 这一件画到百分之几。没有步数就回 null，界面画一条来回跑的条。
 *
 * **一格的不算进度。** 出片那条总任务报的是「第几章 / 一共几章」，一次只
 * 跑一章时就是 0/1——画出来是一条 0% 的条加一句「0% 0/1」，它既不说明在
 * 干什么也不说明还有多久，而这一行真正有用的是下面那句引擎现说的话。
 */
function pct(r) {
  if (!r.total || r.total <= 1) return null
  return Math.min(100, Math.round((r.current / r.total) * 100))
}

/**
 * 这一件大概还要等多久才轮到。
 *
 * **不报一个准点，报一个量级。** 前面排着几件、每件大概多久，两样都是估
 * 的；报「大约 3 分钟后」会被当成承诺，而它取决于对面机器忙不忙。所以只在
 * 引擎给得出 `eta`（同一族活最近几次的中位耗时）时才说一句。
 */
function waitHint(row, index) {
  if (!row.eta) return ''
  const lanes = board.value.running.length || 1
  const ahead = Math.floor(index / lanes)
  if (ahead <= 0) return '马上轮到'
  return `大约还要等 ${dur(row.eta * ahead)}`
}

/** 做完的那几行：一句话说清是什么下场。 */
/**
 * `pill`：那块牌子用哪一档颜色。
 *
 * **「停下了」不能和「失败」同色。** 引擎那头就是分开记的（TaskState），
 * 人按的停不是出错；一样标红的话，人会去找哪儿坏了。红只留给真砸了的。
 */
const STATE = {
  done: { label: '', cls: '', pill: '' },
  failed: { label: '失败', cls: 'is-failed', pill: 'pill--danger' },
  cancelled: { label: '停下了', cls: 'is-cancelled', pill: 'pill--neutral' },
}

/** 这一轮做完的活加起来花了多久、有几件没成。给"做完的"那个小结用。 */
const doneSummary = computed(() => {
  const rows = board.value.done
  const secs = rows.reduce((a, r) => a + (Number(r.seconds) || 0), 0)
  const bad = rows.filter((r) => r.state === 'failed').length
  const stopped = rows.filter((r) => r.state === 'cancelled').length
  return { secs, bad, stopped }
})

async function load() {
  try {
    const d = await api.tasks(mineOnly.value ? session.projectPath : '')
    board.value = {
      running: d?.running ?? [],
      queued: d?.queued ?? [],
      done: d?.done ?? [],
    }
    loaded.value = true
    // 已经展开的那几件，思考还在长——跟着刷。**只刷展开的那几件、而且只
    // 取新增**：一份思考十几万字，整份全刷等于每一拍把整本账拖一遍。
    for (const id of Object.keys(opened.value)) await loadThinking(id, true)
  } catch {
    // 拉不到就保持上一拍。引擎打个嗝不该让整页闪成空的。
  }
}

async function loadThinking(id, quiet) {
  const have = opened.value[id]
  try {
    const d = await api.taskThinking(id, have?.end ?? 0)
    const start = Number(d?.start ?? 0)
    const end = Number(d?.end ?? 0)
    const piece = d?.thinking ?? ''
    // `start` 大于我们手上那份的结尾 = 中间断了一截（思考太长，引擎那头从
    // 头截过）。这时候**丢掉手上那份重接**，不能把两段错接起来。
    const broke = have && start > have.end
    const text = have && !broke ? have.text + piece : piece
    opened.value = { ...opened.value, [id]: { text, end, broke: !!broke } }
  } catch (e) {
    if (!quiet) ui.error(`看不到这一件的思考：${e?.message || e}`)
  }
}

function toggleThinking(row) {
  if (opened.value[row.id] !== undefined) {
    const next = { ...opened.value }
    delete next[row.id]
    opened.value = next
    rememberOpen()
    return
  }
  loadThinking(row.id, false).then(rememberOpen)
}

/**
 * 思考那一块**跟着最新往下走**。
 *
 * 用户 2026-09-17：「在任务里思考没有实时显示最后内容」。它是从上往下长的，
 * 而人要看的是**它现在在想什么**——不跟的话每来一段都得自己滚到底。
 *
 * **人自己往上翻了就别抢**：离底部超过一屏就当他在回头看，停止跟随；
 * 滚回底部附近又接着跟。
 */
function follow(el) {
  if (!el) return
  const near = el.scrollHeight - el.scrollTop - el.clientHeight < 80
  if (near || el.dataset.first !== '1') {
    el.dataset.first = '1'
    el.scrollTop = el.scrollHeight
  }
}

async function stop(row) {
  try {
    await api.cancelTask(row.id)
    await load()   // 不等下一拍：按下去要当场有反应
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
  timer = setTimeout(
    async () => {
      await load()
      tick()
    },
    busy.value ? 1500 : 5000,
  )
}

onMounted(async () => {
  // 刷新之前展开着的那几件，接回来。
  try {
    for (const id of JSON.parse(readLocal(kOpenKey) || '[]')) {
      opened.value = { ...opened.value, [id]: { text: '', end: 0 } }
    }
  } catch {
    // 存的东西坏了就当没记过，不值得打扰用户。
  }
  await load()
  tick()
})
onUnmounted(() => clearTimeout(timer))
</script>

<template>
  <div class="page tasks">
    <header class="tasks__head">
      <h1 class="h2">任务</h1>
      <!-- 一眼看完的三个数。**摆在标题旁边**：底下三节各自还会重复一次，
           但人进这一页最先问的是"还有多少没干完"。 -->
      <span v-if="busy" class="tasks__sum tiny">
        <b>{{ board.running.length }}</b> 件在跑<template v-if="board.queued.length">
          · <b>{{ board.queued.length }}</b> 件排着</template>
      </span>
      <span v-else-if="loaded" class="tasks__sum tiny dim">闲着</span>
      <span class="spacer" />
      <label class="switch tiny" title="只看当前这一部电影的活">
        <input v-model="mineOnly" type="checkbox" @change="load" />
        <span>只看这一部</span>
      </label>
    </header>

    <!-- ── 正在做 ── -->
    <section class="card tasks__sec">
      <h2 class="tasks__h">
        <span class="dot dot--live" />正在做
        <!-- **计数做成牌子，不是标题后面跟一个数字。** 原来是紧贴着标题的
             一个 `tiny dim` 数，空的时候就是「做完的 0」——那个 0 像是标题
             的一部分，不像一个计数。围起来就读得出它是"几件"。 -->
        <span class="tasks__n numeric" :class="{ 'tasks__n--zero': !board.running.length }">
          {{ board.running.length }}
        </span>
      </h2>
      <!-- **空态用房子里那一个，别自己手搓一行灰字。** 整份界面十个视图
           都是 EmptyState（一个图标 + 一句"什么是空的" + 一句"下一步去哪"），
           只有这一页原来是贴在标题底下的一行 `tiny dim` —— 挨着标题、没有
           留白，读起来像标题掉下来的半句话，不像"这儿本来该有东西"。

           **过滤着的时候要说清"是没有，还是被这个勾挡住了"。** 一台机器上常
           开着好几部电影，跑着的那部不一定是当前这部——那时候这一页整片空白，
           而顶栏那块牌子还写着「任务 6」，两处对不上。那句话进 hint。 -->
      <EmptyState
        v-if="!board.running.length"
        icon="play"
        :title="!loaded ? '正在问引擎…' : '这会儿没有活在跑'"
        :hint="loaded && mineOnly ? '别的电影有没有，把「只看这一部」取消了就知道' : ''"
      />
      <ul v-else class="rows">
        <li v-for="r in board.running" :key="r.id" class="row row--live">
          <!-- 进度铺成整行的底色，和镜头墙一个规矩：既不占地方，也比一条
               细线看得清。**没有步数时画一条来回跑的**——读权重、等对面
               机器那十几秒一次回调都没有，画一条停在 0 的条像卡死了。 -->
          <span
            class="row__fill"
            :class="{ 'row__fill--idle': pct(r) === null }"
            :style="pct(r) !== null ? { width: pct(r) + '%' } : null"
          />
          <span class="chip tiny nowrap">
            <AppIcon :name="kindOf(r.kind).icon" :size="12" />
            {{ kindOf(r.kind).label }}
          </span>
          <span class="row__main">
            <span class="row__title truncate" :title="r.title">{{ r.title }}</span>
            <!-- 引擎现说的那句（「正在写 ch03（厂长的回执）」）。长跑那条
                 的名字是固定的，真正在干哪一步只有这一句说得出。 -->
            <span v-if="r.note" class="row__note tiny dim truncate" :title="r.note">
              {{ r.note }}
            </span>
          </span>
          <!-- **这一格没有数也要占着。** 读权重、等对面机器那种活报不出
               步数，`v-if` 掉的话这一行后面全部左移，于是同一屏里时长那一
               列落在四个不同的位置上，一列数没法竖着扫下来——而"哪件跑了
               最久"正是这一页要一眼看出的事。空着，位置留住。 -->
          <span class="tiny numeric nowrap row__pct">
            <template v-if="pct(r) !== null">
              {{ pct(r) }}%
              <span class="dim">{{ r.current }}/{{ r.total }}</span>
            </template>
          </span>
          <span class="tiny dim numeric nowrap row__time">{{ dur(r.seconds) }}</span>
          <!-- 尾部这一格同理：有没有「思考」、停没停，各行不一样，
               收进一个宽度钉死的格子里，右对齐。 -->
          <span class="row__act">
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
          </span>
          <pre
            v-if="opened[r.id] !== undefined"
            :ref="(el) => follow(el)"
            class="think"
          >{{ opened[r.id].text || '还没有想出字来' }}</pre>
        </li>
      </ul>
    </section>

    <!-- ── 排队中 ── -->
    <section v-if="board.queued.length || busy" class="card tasks__sec">
      <h2 class="tasks__h">
        <span class="dot dot--wait" />排队中
        <!-- **计数做成牌子，不是标题后面跟一个数字。** 原来是紧贴着标题的
             一个 `tiny dim` 数，空的时候就是「做完的 0」——那个 0 像是标题
             的一部分，不像一个计数。围起来就读得出它是"几件"。 -->
        <span class="tasks__n numeric" :class="{ 'tasks__n--zero': !board.queued.length }">
          {{ board.queued.length }}
        </span>
      </h2>
      <EmptyState v-if="!board.queued.length" icon="pause" title="没有排着的" />
      <ul v-else class="rows">
        <li v-for="(r, i) in board.queued" :key="r.id" class="row">
          <span class="chip tiny nowrap">
            <AppIcon :name="kindOf(r.kind).icon" :size="12" />
            {{ kindOf(r.kind).label }}
          </span>
          <span class="row__main">
            <span class="row__title truncate" :title="r.title">{{ r.title }}</span>
          </span>
          <span class="tiny dim nowrap">{{ waitHint(r, i) }}</span>
          <span v-if="r.waited > 5" class="tiny dim numeric nowrap row__wait">
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

    <!-- ── 做完的 ── -->
    <section class="card tasks__sec">
      <h2 class="tasks__h">
        <span class="dot" />做完的
        <!-- **计数做成牌子，不是标题后面跟一个数字。** 原来是紧贴着标题的
             一个 `tiny dim` 数，空的时候就是「做完的 0」——那个 0 像是标题
             的一部分，不像一个计数。围起来就读得出它是"几件"。 -->
        <span class="tasks__n numeric" :class="{ 'tasks__n--zero': !board.done.length }">
          {{ board.done.length }}
        </span>
        <!-- **失败那个数不该和别的字一样灰。** 原来四段全是 `tiny dim`
             拿「·」串成一句：「4 · 一共 26 分 16 秒 · 1 件失败 · 1 件停下」，
             而这一行里真正要人看见的就是"有几件砸了"。耗时照旧是灰的背景
             信息，失败和停下各给一块牌子——和行里那两块同色，一眼对得上。 -->
        <span v-if="board.done.length" class="tiny dim tasks__tot">
          一共 {{ dur(doneSummary.secs) }}
        </span>
        <span v-if="doneSummary.bad" class="pill pill--danger tiny nowrap">
          {{ doneSummary.bad }} 件失败
        </span>
        <span v-if="doneSummary.stopped" class="pill pill--neutral tiny nowrap">
          {{ doneSummary.stopped }} 件停下
        </span>
      </h2>
      <EmptyState
        v-if="!board.done.length"
        icon="check"
        title="还没有干完的活"
        hint="做完的会留在这儿，连带耗时和当时的思考"
      />
      <ul v-else class="rows">
        <li
          v-for="r in board.done"
          :key="r.id"
          class="row"
          :class="STATE[r.state]?.cls"
        >
          <span class="chip tiny nowrap">
            <AppIcon :name="kindOf(r.kind).icon" :size="12" />
            {{ kindOf(r.kind).label }}
          </span>
          <span class="row__main">
            <span class="row__title truncate" :title="r.title">{{ r.title }}</span>
            <!-- **停和砸分两种说法。** 「人按的停不是失败」，报成红的会让
                 人去找哪儿出错了——引擎那头也是这么分的（TaskState）。 -->
            <span v-if="r.error" class="row__note tiny truncate" :title="r.error">
              {{ r.error }}
            </span>
          </span>
            <!-- 同上：干净跑完的那几件没有牌子，但位置要留着，
               不然它们的时长和失败那几行对不齐。 -->
          <span class="row__state">
            <span
              v-if="STATE[r.state]?.label"
              class="pill tiny nowrap"
              :class="STATE[r.state].pill"
            >
              {{ STATE[r.state].label }}
            </span>
          </span>
          <span class="tiny dim numeric nowrap row__time">{{ dur(r.seconds) }}</span>
          <span class="row__act">
            <button
              v-if="r.thinking"
              class="btn btn--sm btn--ghost"
              type="button"
              title="点开看它当时想了什么"
              @click="toggleThinking(r)"
            >
              思考
            </button>
          </span>
          <pre
            v-if="opened[r.id] !== undefined"
            :ref="(el) => follow(el)"
            class="think"
          >{{ opened[r.id].text || '没有留下思考' }}</pre>
        </li>
      </ul>
    </section>
  </div>
</template>

<style scoped>
.tasks__head {
  display: flex;
  align-items: baseline;
  gap: var(--s3);
  margin-bottom: var(--s3);
}
.tasks__sum b {
  font-variant-numeric: tabular-nums;
  color: var(--text);
}
.tasks__sec + .tasks__sec {
  margin-top: var(--s3);
}
/* **标题要有内距。** `.card` 自己的 padding 是 0（行各自带 `7px 8px`，
   所以行看着是有边距的），而标题什么都没有——量过：卡片左缘 24、标题左缘
   25、行内容左缘 33。也就是标题贴着上边框 1px，还比它底下每一行都往左顶
   出去 8px，整个卡在角上。
   左右给 8px，和行内容对齐成一条竖线；上面 12px，让它和卡片边框脱开。 */
.tasks__h {
  display: flex;
  align-items: center;
  gap: var(--s2);
  margin: 0;
  padding: var(--s3) var(--s2) var(--s2);
  font-size: var(--fs-md);
  font-weight: 600;
}
/* 最后一行离下边框也只有 7px，跟着补一点。 */
.tasks__sec {
  padding-bottom: var(--s2);
}
/* 空态是 section 的直接子元素，它自己只有上下 padding——不补的话它比
   标题和行都往左顶 8px，三样东西三个左缘。 */
.tasks__sec :deep(.empty) {
  padding-inline: var(--s2);
}
/* 小节标题上那个计数。`.chip` 那一套的缩小版：围起来才读得出是"几件"。 */
.tasks__n {
  min-width: 20px;
  padding: 0 6px;
  border-radius: var(--r-pill);
  background: var(--surface-2);
  color: var(--text-2);
  font-size: var(--fs-xs);
  font-weight: 600;
  line-height: 18px;
  text-align: center;
}
/* 零件就更退一档：它是"这儿是空的"，不是一个要看的数。 */
.tasks__n--zero {
  background: transparent;
  color: var(--text-3, var(--text-2));
}
/* 「一共 X」是背景信息，不跟失败那块牌子抢。 */
.tasks__tot {
  font-weight: 400;
}
/* 三节各一个小圆点：在跑的会呼吸，排着的是空心，做完的是灰实心。
   标题文字都是一样的字号字重，**颜色和形状才是区分**——三行大标题
   堆在一起时，人先看到的是左边那一列点。 */
.dot {
  width: 7px;
  height: 7px;
  flex: none;
  border-radius: 50%;
  background: var(--text-3, var(--text-2));
}
.dot--live {
  background: var(--accent);
  animation: taskpulse 1.6s ease-in-out infinite;
}
.dot--wait {
  background: transparent;
  border: 1.5px solid var(--text-2);
}
@keyframes taskpulse {
  0%,
  100% {
    opacity: 1;
  }
  50% {
    opacity: 0.25;
  }
}
/* 动画对"减少动态效果"那档系统设置要让步。 */
@media (prefers-reduced-motion: reduce) {
  .dot--live,
  .row__fill--idle {
    animation: none;
  }
}

.rows {
  list-style: none;
  margin: 0;
  padding: 0;
}
.row {
  position: relative;
  display: flex;
  align-items: center;
  gap: var(--s2);
  padding: 7px var(--s2);
  border-radius: var(--r-sm);
  /* 思考展开之后要换行铺满，所以这一行是可换行的 flex。 */
  flex-wrap: wrap;
}
.row + .row {
  border-top: 1px solid var(--line);
}
/* **一行一格的节奏。** 带「正在写 ch05…」那句的行是两行高（48px），不带的
   只有 35px，两种挨在一起像没排齐。给个下限，单行的那几件也占住同一格。 */
.row {
  min-height: 42px;
}
.row:hover {
  background: var(--bg-2);
}
.row--live {
  /* 底色那条要压在内容底下，所以这一行自己开一个层叠上下文 */
  isolation: isolate;
}
.row__fill {
  position: absolute;
  inset: 0 auto 0 0;
  z-index: -1;
  border-radius: var(--r-sm);
  /* **前沿要化开。** 原来是一整块纯色，于是行中间横着一道笔直的竖边——
     读起来像"这一行被选中了一半"，而不是"跑到这儿了"。让最后 28px 渐隐，
     那道边就变成了进度该有的样子。整体也压淡了一档（12% → 9%）：它是底色，
     不该和 hover 抢，更不该盖过上面的字。 */
  background: linear-gradient(
    90deg,
    color-mix(in srgb, var(--accent) 9%, transparent) calc(100% - 28px),
    transparent
  );
  transition: width 0.4s ease;
}
.row__fill--idle {
  width: 100%;
  background: linear-gradient(
    90deg,
    transparent,
    color-mix(in srgb, var(--accent) 10%, transparent),
    transparent
  );
  background-size: 40% 100%;
  background-repeat: no-repeat;
  animation: taskslide 1.4s linear infinite;
}
@keyframes taskslide {
  from {
    background-position: -40% 0;
  }
  to {
    background-position: 140% 0;
  }
}
.chip {
  display: inline-flex;
  align-items: center;
  gap: 3px;
  flex: none;
  width: 84px;
  padding: 1px 6px;
  border-radius: var(--r-sm);
  background: var(--surface-2);
  color: var(--text-2);
}
/* **类别那一格宽度钉死**：十几行堆在一起时，名字左边缘对齐比省几像素
   重要得多——不对齐的话每一行都要重新找从哪儿开始读。 */
.row__main {
  display: flex;
  flex-direction: column;
  flex: 1 1 220px;
  min-width: 0;
  line-height: 1.35;
}
.row__note {
  color: var(--text-2);
}
/* **右边这两列和左边的 .chip 是同一件事**：上面那段说"名字左边缘对齐比省
   几像素重要"，而右边原来没人管——量过一屏：时长那一列的左缘在 528/565/
   578/579 四个位置上游走，差 50px。每一行的数都落在不同地方，一列数就没法
   竖着扫下来，而"哪件跑了最久"正是这一页要一眼看出的事。
   钉死宽度 + 右对齐 + tabular-nums，三样缺一不可。 */
.row__pct {
  flex: none;
  width: 86px;
  text-align: right;
  color: var(--accent);
}
.row__time {
  flex: none;
  width: 66px;
  text-align: right;
}
/* 排队那一节的「排了 X」多两个字，66px 会把它裁了，单开一格。 */
.row__wait {
  flex: none;
  width: 92px;
  text-align: right;
}
/* 尾部那一格：有没有「思考」、停没停，各行不一样。钉死宽度、靠右排，
   前面那几列才不会被它顶得一行一个位置。 */
.row__act {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: var(--s1);
  flex: none;
  width: 96px;
}
/* 「失败 / 停下了」那块牌子。干净跑完的没有牌子，但格子留着。 */
.row__state {
  display: flex;
  justify-content: flex-end;
  flex: none;
  width: 56px;
}
/* **错的那一行要读得出来。** 原来用的是 --warn（#ffcc4d，黄），白底上
   几乎看不清；而且黄说的是"提醒"，一件跑砸了的活该说"错"。--danger 是红。 */
.is-failed .row__note {
  color: var(--danger, var(--text-2));
}
.is-failed .row__title,
.is-cancelled .row__title {
  color: var(--text-2);
}
.think {
  flex: 1 0 100%;
  max-height: 40vh;
  overflow: auto;
  margin: var(--s2) 0 var(--s1);
  padding: var(--s2);
  border-radius: var(--r-sm);
  background: var(--bg-2);
  color: var(--text-2);
  font-size: var(--fs-xs);
  line-height: 1.6;
  /* 思考是一大段没有换行的字，不裹住的话整页会被它撑宽。 */
  white-space: pre-wrap;
  word-break: break-word;
}
</style>
