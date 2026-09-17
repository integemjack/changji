<script setup>
/**
 * 顶栏那块「正在思考」。鼠标放上去能看见它在想什么。
 *
 * 用户 2026-09-14：「不管写文章还是什么都要显示出在思考的 UI，鼠标放上去
 * 还能看到内容」。
 *
 * **为什么非有不可。** 现在的模型都要"先想再写"，而且关不掉（智谱
 * glm-5.3 发关闭值直接回 400 说"该模型始终思考"）。思考那一段少则十几秒、
 * 多则十几分钟，期间正文一个字都不出来——用户面对的是一个一动不动的
 * 进度条，分不清是在想还是卡死了。实测 glm-5.3 写一份大纲，光思考就超过
 * 十分钟。
 *
 * **划过就开，点一下钉住。** 两种都要：
 *
 *   划过就开   它只在真的在想的时候才出现，而出现的那几分钟里用户本来就在
 *              等它——把手放上去想瞄一眼，再要求点一下是多一道手续。
 *   点了钉住   只有划过的话，**手一移开就没了**，想把那段思考读完、
 *              或者选中复制一段，根本做不到（用户 2026-09-14 直接
 *              报的就是「点击展开」）。钉住之后鼠标可以离开，
 *              再点一次、或者点到别处才收。
 *
 * **内容是滚动的，自己贴着底。** 思考是一直往下长的，不自动滚的话浮层
 * 打开后停在开头，越看越旧。
 *
 * **不做"展开全文"。** 存的本来就只有最后几千字（见 stores/thinking），
 * 摆一个按钮点开却发现前面没了，比不摆更糟。
 *
 * **「停下」放在这儿。** 用户 2026-09-14：「增加可以取消按钮」。
 * 放这块不是随便挑的：它是这几分钟里**唯一**一直在屏幕上的东西，而想停的
 * 念头恰恰是在这几分钟里冒出来的（看了两眼思考，发现它想岔了）。摆回各个
 * 视图的话，十几个会思考的步骤要各摆一个，而且用户得先找到那一页。
 */
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'

import { api } from '@/api'
import AppIcon from '@/components/AppIcon.vue'
import { useThinking } from '@/stores/thinking'
import { useUi } from '@/stores/ui'

const thinking = useThinking()
const ui = useUi()

/**
 * 跟引擎那本任务账对拍子。**刷新之后这块徽标全靠它**——socket 那份按点击
 * 生成的 stream id 存，刷新就没了（见 stores/thinking 里 fromBoard 那段）。
 *
 * socket 上有东西的时候 `syncFromServer` 自己会空转，所以这一拍不贵；
 * 拍子按"在不在想"快慢分，和这套代码里别处那几条轮询一个规矩。
 */
let beat = null
function tick() {
  clearTimeout(beat)
  beat = setTimeout(
    async () => {
      await thinking.syncFromServer()
      tick()
    },
    thinking.busy ? 2000 : 6000,
  )
}
/** 鼠标在上面。 */
const hovering = ref(false)
/** 点过了，钉住不收。 */
const pinned = ref(false)
const open = computed(() => pinned.value || hovering.value)
const body = ref(null)
const chip = ref(null)
const root = ref(null)

/**
 * 浮层挂在**视口**上，不是挂在徽标上。
 *
 * 挂在徽标上（absolute + right:0）的第一版当场露馅：这块徽标在顶栏中间
 * 偏左，而浮层有 520px 宽，往左伸就被视口左边切掉了大半——实际点出来
 * 看到的是半截字。顶栏上东西的排布还会随窗口宽度变，写死靠左或靠右都
 * 迟早撞上同一件事。
 *
 * 所以开的时候量一次徽标在哪儿，定住 top，再按空间够不够决定往左还是
 * 往右伸，并且两边都留 12px 不贴边。
 */
const pos = ref({ top: 0, left: 0 })
const MARGIN = 12
const WIDTH = 520

function place() {
  const el = chip.value
  if (!el) return
  const r = el.getBoundingClientRect()
  const w = Math.min(WIDTH, window.innerWidth - MARGIN * 2)
  // 优先和徽标右边对齐；伸出去了就贴着视口边收回来
  let left = r.right - w
  if (left < MARGIN) left = MARGIN
  if (left + w > window.innerWidth - MARGIN) left = window.innerWidth - MARGIN - w
  const next = { top: Math.round(r.bottom + 6), left: Math.round(left), width: w }
  // 没挪就别写：下面 replace() 是一秒一次的，原样写回去会让这块浮层
  // （里头是一段一直在长、还自动贴底的文字）每秒白重排一次。
  const cur = pos.value
  if (cur.top === next.top && cur.left === next.left && cur.width === next.width) return
  pos.value = next
}

watch(open, (v) => {
  if (v) place()
})

/**
 * **量一次不够，徽标自己会动。**
 *
 * `place()` 原来只在浮层打开那一下跑一次，而它量的是徽标当时在哪儿。
 * 顶栏那一排是 `display: flex` 加一个 `spacer` 顶到右边的——也就是说
 * 这块徽标的右边缘由**它右边那几个东西的宽度**决定，而那几个一直在变：
 *
 *   · SysMeter 两秒推一条，「8%」变「100%」就宽出两个字符；
 *   · JobBadge 一开跑就出现、跑完就消失，而"正在思考"这几分钟里恰恰
 *     是别的活最容易起落的时候；
 *   · 窗口一改大小，整排跟着挪。
 *
 * 浮层是 `position: fixed` 加写死的 top/left/width，于是它会一点点和
 * 徽标错开；窗口缩窄时更糟——left 是按旧宽度算的，520px 的浮层会挂到
 * 视口外面去，还可能把整页顶出一条横向滚动条。而这块浮层的存在时间正是
 * 十几分钟那一档（注释开头就写着"实测 glm-5.3 写一份大纲，光思考就超过
 * 十分钟"），错开是必然发生的，不是边角情形。
 *
 * 开着的时候跟着重量一次就行：下面那个「想了多久」的秒表本来就一秒一跳，
 * 顺手带上；窗口改大小再单挂一条。关着的时候一次都不量。
 */
function replace() {
  if (open.value) place()
}
onMounted(() => window.addEventListener('resize', replace))
onUnmounted(() => window.removeEventListener('resize', replace))

/** 按过停的那几条，按钮变成"停着…"不让再按。 */
const stopping = ref(new Set())

/**
 * 停下这一件。
 *
 * **不在这儿把它从列表里划掉。** 取消是"请它停"，真停下来要等那一步自己
 * 走到下一个检查点（最长一次网络往返）。划早了的话顶栏先消失、几秒后那件活
 * 才真结束，中间那几秒用户以为停了、其实还在烧 token。等 job_error 回来，
 * runAsyncJob 的 finally 会清掉它。
 */
async function stop(id) {
  if (!id || stopping.value.has(id)) return
  stopping.value = new Set([...stopping.value, id])
  try {
    const r = await api.cancelJob(id)
    // stopped=false 是"按下去那一刻刚好干完"，不是错，别弹框
    if (!r?.stopped) ui.info?.('这一步已经结束了')
  } catch (e) {
    ui.error?.(e.message || '没停下来')
    const next = new Set(stopping.value)
    next.delete(id)
    stopping.value = next
  }
}

/** 点一下：钉住 / 取消钉住。 */
function toggle() {
  pinned.value = !pinned.value
  if (pinned.value) place()
}

/**
 * 钉住之后点到别处就收。
 *
 * **挂在 document 上，不是靠 blur。** 浮层里的字是要能选中复制的，
 * 而选文字会让按钮失焦——靠 blur 收的话，一拖选浮层就没了。
 */
function onDocClick(e) {
  if (!pinned.value) return
  if (root.value?.contains(e.target)) return
  pinned.value = false
}
// **捕获阶段。** 页面上别处有十几处 `@click.stop`（项目库那几行、卡片、
// 抽屉、菜单），挂在冒泡阶段的话点到它们这张浮层收不掉——而它有 520px
// 宽、盖在页面上，思考一段可能十几分钟。顶栏隔壁那块（JobBadge）为同一
// 件事改过，理由那儿写着：捕获阶段先于它们拿到事件，且只读不拦。
// 点徽标本身不受影响：那时候 e.target 在 root 里，上面那句就返回了。
onMounted(() => {
  document.addEventListener('click', onDocClick, true)
  // 进页面先对一次账：刷新之后 socket 那份是空的，这一下就是唯一的来源。
  thinking.syncFromServer()
  tick()
})
onUnmounted(() => {
  document.removeEventListener('click', onDocClick, true)
  clearInterval(timer)
  clearTimeout(beat)
})

/** 正在想的那几条。一般只有一条，批量跑的时候会有好几条。 */
const items = computed(() => thinking.items)

/** 浮层里显示哪一条：最近开始的那一条。 */
const current = computed(() => items.value[0] ?? null)

/** 想了多久了。**要显示这个**——十分钟不动的时候，这个数是"它还活着"的唯一证据。 */
const elapsed = ref('')
let timer = null
watch(
  () => thinking.busy,
  (busy) => {
    clearInterval(timer)
    if (!busy) {
      elapsed.value = ''
      // 想完了就收起来，别留一张空浮层——钉住的也收，它已经没内容了
      pinned.value = false
      hovering.value = false
      return
    }
    const tick = () => {
      // 秒表跳一下顺手把浮层的位置重量一次，理由见 replace() 上面那段
      replace()
      const at = current.value?.at
      if (!at) return
      const s = Math.max(0, Math.round((Date.now() - at) / 1000))
      elapsed.value = s < 60 ? `${s} 秒` : `${Math.floor(s / 60)} 分 ${s % 60} 秒`
    }
    tick()
    timer = setInterval(tick, 1000)
  },
  { immediate: true },
)

// 新的一段进来就贴着底。**只在浮层开着时做**——关着的时候 body 是 null，
// 而且没人看。
watch(
  () => current.value?.text,
  async () => {
    if (!open.value) return
    await nextTick()
    const el = body.value
    if (el) el.scrollTop = el.scrollHeight
  },
)
</script>

<template>
  <div
    v-if="thinking.busy"
    class="think"
    ref="root"
    @mouseenter="hovering = true"
    @mouseleave="hovering = false"
  >
    <button
      ref="chip"
      class="think__chip"
      :class="{ 'is-pinned': pinned }"
      type="button"
      :aria-expanded="open"
      @click.stop="toggle"
    >
      <AppIcon name="sparkle" :size="14" />
      <span class="think__label">
        正在思考<template v-if="current?.label"> · {{ current.label }}</template>
      </span>
      <span v-if="elapsed" class="think__time numeric">{{ elapsed }}</span>
      <span v-if="items.length > 1" class="think__n">{{ items.length }}</span>
    </button>

    <div
      v-if="open"
      class="think__pop"
      :style="{ top: pos.top + 'px', left: pos.left + 'px', width: pos.width + 'px' }"
    >
      <div class="think__head">
        <span class="strong">{{ current?.label || '模型正在想' }}</span>
        <span class="tiny dim">{{ elapsed }}</span>
        <span class="spacer" />
        <button
          class="btn btn--ghost btn--sm"
          type="button"
          :disabled="stopping.has(current?.id)"
          @click.stop="stop(current?.id)"
        >
          {{ stopping.has(current?.id) ? '停着…' : '停下' }}
        </button>
      </div>
      <pre v-if="current?.text" ref="body" class="think__body">{{ current.text }}</pre>
      <p v-else class="think__empty tiny dim">
        刚开始想，还没吐出内容。有的服务会想完才一次性给。
      </p>
      <p v-if="items.length > 1" class="tiny dim think__more">
        另外还有 {{ items.length - 1 }} 件也在想
      </p>
    </div>
  </div>
</template>

<style scoped>
.think {
  position: relative;
  display: inline-flex;
}

.think__chip {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 3px 9px;
  border: 1px solid var(--line);
  border-radius: 999px;
  background: var(--surface);
  color: var(--text);
  font-size: 12px;
  cursor: pointer;
}

/* 钉住了要看得出来，否则"为什么移开还在"没人解释得通 */
.think__chip.is-pinned {
  border-color: var(--accent, var(--text));
}

/* 呼吸一下，说明它还活着。思考十分钟不出字的时候，这是唯一的动静。 */
.think__chip :deep(svg) {
  animation: think-pulse 1.6s ease-in-out infinite;
}

@keyframes think-pulse {
  0%,
  100% {
    opacity: 0.45;
  }
  50% {
    opacity: 1;
  }
}

.think__label {
  white-space: nowrap;
}

.think__time {
  /* 没有 --text-dim 这个变量（是 --text-2 / --text-3）。写错的变量整条
     声明作废，这几处一直跟着父元素的颜色走——该轻的不轻。 */
  color: var(--text-3);
}

.think__n {
  min-width: 16px;
  padding: 0 4px;
  border-radius: 999px;
  background: var(--accent-soft, var(--line));
  text-align: center;
}

.think__pop {
  /* fixed：顶栏可能开了 overflow，absolute 的浮层会被它裁掉一角 */
  position: fixed;
  z-index: 50;
  padding: 10px 12px;
  border: 1px solid var(--line);
  border-radius: 10px;
  background: var(--surface);
  box-shadow: 0 10px 30px rgb(0 0 0 / 22%);
}

.think__head {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 6px;
}

.think__head .spacer {
  flex: 1;
}

.think__body {
  max-height: 320px;
  margin: 0;
  overflow-y: auto;
  font-size: 12px;
  line-height: 1.65;
  color: var(--text-2);
  white-space: pre-wrap;
  word-break: break-word;
}

.think__empty,
.think__more {
  margin: 6px 0 0;
}
</style>
