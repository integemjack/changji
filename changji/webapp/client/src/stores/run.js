/**
 * 流水线运行状态。
 *
 * **WebSocket 打底，轮询兜底。** 引擎把进度推到 /api/ws（同一个进程直接
 * 答，原来中间那层 Node 2026-09-12 删了），连不上或断了就退回每 1200ms
 * 轮询 /api/run。
 * 两条路并存不是保守：WebSocket 掉线的原因很多（代理、休眠、引擎重启），
 * 而这一屏是用户盯着等的，宁可慢一点也不能停在半路。
 *
 * **推上来的是增量，不是整份状态。** 引擎的消息只有
 * {type, job_id, stage, step, total, message, shot_id}，
 * 没有 outputs / events / queue_total 这些。所以：
 *
 *   - 开跑前先拉一次全量（poll）
 *   - 跑的过程中用推上来的增量更新进度
 *   - 收到 done / error 再拉一次全量，把产出和完整事件补齐
 *
 * 这也是引擎那边 ws.hpp 里写的用法：「重连后先拉一次全量再续订阅」。
 */

import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { api } from '@/api'
// **阶段名只有一份**（`@/api/labels`）。这儿原来自己抄了一份，两份已经
// 分叉了：gate 一边写「闸门」一边写「质量闸门」，assemble 一边「装配」
// 一边「装配成片」——同一个阶段在不同组件里显示成不同的名字。
import { STAGE_LABELS } from '@/api/labels'
import { openJobSocket } from '@/composables/useJobSocket'

/** 轮询兜底的间隔。和原来一样，用户对这个节奏已经有预期。 */
const POLL_MS = 1200
/** 连丢三拍之后的慢轮询间隔。见 useRun 里的 sleepAndRetry。 */
const SLOW_POLL_MS = 8000

export const useRun = defineStore('run', () => {
  const state = ref(null)
  const polling = ref(false)
  /** 真的连上了 WebSocket。界面暂时不显示它，排查时有用。 */
  const live = ref(false)
  let timer = null
  let socket = null
  let retry = null
  let missCount = 0

  const running = computed(() => Boolean(state.value?.running))
  const percent = computed(() => {
    const s = state.value
    if (!s || !s.total) return 0
    return Math.min(100, Math.round((s.current / s.total) * 100))
  })
  const stageLabel = computed(() => {
    const stage = state.value?.stage
    return stage ? (STAGE_LABELS[stage] ?? stage) : ''
  })
  const events = computed(() => state.value?.events ?? [])

  /**
   * 正在跑的镜头。多卡之后同一时刻有好几镜在出，这张表就是"哪几镜、
   * 各到第几步"。键是 shot_id，值是最后一条进度。
   *
   * 进：任何带 shot_id 的 progress。
   * 出：shot_done（跑完了）、warn / gate（这一轮结束了，重试会再进来）、
   *     error，以及整个任务结束。
   *
   * 靠的是引擎在 WebSocket 消息里带的 kind。老引擎不带这个字段的话
   * 全按 progress 算——表只进不出，但至少不会漏掉正在跑的。
   */
  const inflightMap = ref(new Map())
  const inflight = computed(() => [...inflightMap.value.values()])

  /**
   * 又有一镜落定了（跑完、失败、过闸没过）。**一个只增不减的计数**，
   * 界面 watch 它去重拉镜头表。
   *
   * 镜头墙原来靠六秒一次的定时器重拉。可 Chrome 对不在前台的标签页会把
   * setInterval 压到**一分钟一次**（实测：两分半里 /api/shots 只拉了两次），
   * 于是首帧出来了，牌子上要等一分钟才变。而 WebSocket **不受这个限制**，
   * 引擎每落定一镜正好发一条 shot_done——那就是"该重拉了"的信号本身，
   * 没道理收到了还去等定时器。
   */
  const settled = ref(0)

  /**
   * 每一镜各自落定过几次。**给缩略图换代号用。**
   *
   * 首帧和视频都写在固定路径上（`frames/<shot_id>.png`、
   * `shots/<shot_id>.mp4`），重出一遍是**原地覆盖**，地址一个字不变——
   * 浏览器于是照旧拿缓存里那张。表现很难看：force 重跑时，每一镜跑完那
   * 一下预览图消失、牌子当场"变回"老样子，而人正盯着看新的出得对不对。
   *
   * 整轮跑完那一下界面会把全部图换一次代（useShots 的 `bust`），但那是
   * 几十分钟之后的事。按镜记一个数，落定哪一镜就只换哪一镜——不然一镜
   * 落定要把满墙二十张图全重拉一遍。
   */
  const settledBy = ref(new Map())

  /**
   * 正在跑的镜头此刻长什么样：shot_id → `data:image/png;base64,…`。
   *
   * 引擎每一步把潜空间投影成一张小图推上来（88×160 那种），牌子上放大
   * 显示，随着步数推进由糊到清。**落定就删**：那时候真图已经落盘，
   * 重拉之后牌子上换成它，预览再留着只会盖住真图。
   */
  const previewBy = ref(new Map())
  const previewOf = (shotId) => previewBy.value.get(shotId) ?? ''

  function trackInflight(msg) {
    if (!msg.shot_id) return
    const kind = msg.kind ?? (msg.type === 'error' ? 'error' : 'progress')
    const next = new Map(inflightMap.value)

    // **换阶段了就把上一阶段留下的全清掉。**
    //
    // 流水线是严格分阶段的：所有镜头先配音，再所有镜头出首帧，再出成片。
    // 所以一收到新阶段的消息，上一阶段还挂在表里的那些必然已经跑完了。
    //
    // 这是一道兜底。正路是引擎每跑完一镜报一条 shot_done（三个阶段现在
    // 都报了）。但漏报的代价太大——2026-09-10 配音和首帧两个阶段都只报
    // progress 不报完成，于是跑过的镜头全部永久挂在表里，整面墙都写着
    // 「配音」，包括那些其实只是在等的。而这件事不报错，只是显示得不对。
    if (msg.stage) {
      for (const [id, x] of next) {
        if (x.stage && x.stage !== msg.stage) next.delete(id)
      }
    }

    if (kind === 'progress') {
      const prev = next.get(msg.shot_id)
      next.set(msg.shot_id, {
        shot_id: msg.shot_id,
        stage: msg.stage ?? prev?.stage ?? '',
        message: msg.message ?? prev?.message ?? '',
        step: typeof msg.step === 'number' ? msg.step : (prev?.step ?? 0),
        total: typeof msg.total === 'number' ? msg.total : (prev?.total ?? 0),
        // **这一镜自己**跑到第几步。和上面那对不是一回事：
        // step/total 是整集的位置（第 21 镜 / 共 22 镜），拿它画单镜的
        // 进度条，那一镜从头到尾都停在 95%——一条不动而且是错的进度条。
        //
        // 引擎不带这两个字段时留 null，牌子上就画一条走马灯而不是
        // 一个具体的百分比。老引擎、以及"准备中"那几条都是这种。
        shotStep:
          typeof msg.shot_step === 'number' ? msg.shot_step : (prev?.shotStep ?? null),
        shotSteps:
          typeof msg.shot_steps === 'number' ? msg.shot_steps : (prev?.shotSteps ?? null),
        // 这一对数说的是"准备"（搬权重、VAE 分块解码）还是真在采样。
        // 两件事量级差很远：准备可能 26/28 段，而挂了 Turbo 的采样只有 6 步。
        // 不分的话牌子上写"成片 26/28"，看着就是跑了 28 步。
        shotPrep:
          typeof msg.shot_prep === 'boolean' ? msg.shot_prep : (prev?.shotPrep ?? false),
        since: prev?.since ?? Date.now(),
      })
    } else {
      next.delete(msg.shot_id)
    }
    inflightMap.value = next
  }

  async function poll() {
    try {
      state.value = await api.runStatus()
      if (!state.value?.running) inflightMap.value = new Map()
      missCount = 0
    } catch {
      // 引擎重启时会连着失败几次。立刻报错太吵，连丢三次再说。
      missCount += 1
      if (missCount >= 3) sleepAndRetry()
    }
  }

  /**
   * 连丢三拍之后：**慢下来，但别睡死。**
   *
   * 这儿原来是 `stop()`——而 stop 把 polling 置假、定时器清掉、socket 也关掉，
   * **连 socket 那条 5 秒重连都跟着断了**（它判的就是 polling）。之后没有
   * 任何东西会把这条线叫醒：能叫醒它的只有用户再点一次开始，或者切走标签页
   * 再切回来（useShots 的 onVisible）。
   *
   * 于是引擎重启一下——出片跑到一半，镜头墙就冻在那一帧；而顶栏那块负载表、
   * 「AI 作业中」、参考图那条流各有各的 5 秒重连，都自己回来了。屏幕上一半
   * 活着一半死着，看着最像"这一页坏了"。
   *
   * 慢轮询而不是继续 1.2 秒一拍：引擎真下线时别把请求打满。拉回来一拍就换回
   * 常速，并把 socket 接上——poll 成功那一支会把 missCount 归零，这儿据此判。
   */
  function sleepAndRetry() {
    if (!polling.value) return
    if (timer) clearInterval(timer)
    timer = setInterval(async () => {
      await poll()
      if (missCount !== 0) return
      clearInterval(timer)
      timer = setInterval(poll, POLL_MS)
      openSocket()
    }, SLOW_POLL_MS)
  }

  /** 把推上来的一条增量并进当前状态。 */
  function applyMessage(msg) {
    if (!msg || typeof msg !== 'object') return
    if (msg.type === 'hello') return

    if (msg.type === 'done' || msg.type === 'error') {
      // 终止消息只说"完了"，产出和完整事件要再拉一次。
      // **不能只把 running 置 false 就完事**：产出列表是这一屏的结果。
      inflightMap.value = new Map()
      previewBy.value = new Map()
      poll()
      return
    }

    // 预览图：只更新那一格的画面，**不是一步**——不进事件流、不动进度。
    if (msg.kind === 'preview') {
      if (msg.shot_id && msg.preview) {
        const next = new Map(previewBy.value)
        next.set(msg.shot_id, msg.preview)
        previewBy.value = next
      }
      return
    }
    // 这一镜落定了（跑完、失败、过闸没过），预览让位给真图
    if (msg.shot_id && msg.kind && msg.kind !== 'progress' && previewBy.value.has(msg.shot_id)) {
      const next = new Map(previewBy.value)
      next.delete(msg.shot_id)
      previewBy.value = next
    }

    trackInflight(msg)
    // 带 shot_id 的非 progress 事件 = 这一镜落定了。见 settled。
    if (msg.shot_id && msg.kind && msg.kind !== 'progress') {
      settled.value += 1
      const next = new Map(settledBy.value)
      next.set(msg.shot_id, (next.get(msg.shot_id) ?? 0) + 1)
      settledBy.value = next
    }

    // 字段名不一样：推上来的叫 step，快照里叫 current。
    // 直接把 msg 铺进 state 的话，进度条会读到 undefined。
    const base = state.value ?? {}
    state.value = {
      ...base,
      running: true,
      stage: msg.stage ?? base.stage,
      current: typeof msg.step === 'number' ? msg.step : base.current,
      total: typeof msg.total === 'number' ? msg.total : base.total,
      message: msg.message ?? base.message,
      // 事件日志按镜头分组显示，推上来的这条也要进去，否则跑的过程中
      // 日志是空的，要等结束才一次性出现。
      //
      // **echo 的除外。** 引擎那边 set_queue / set_pending / set_episode_id
      // 走的是 mutate，推的是当前状态快照，而快照里的 message 是上一条真
      // 事件留下的——照单追加的话每一句话都会被重印一遍。2026-09-13 实机：
      // 装配完日志里「成片已生成」连着两行，第二行是队列计数的回声。
      // 状态照收（进度条要它），事件表不收。
      events: msg.echo
        ? (base.events ?? [])
        : [
            ...(base.events ?? []),
            {
              at: Date.now() / 1000,
              stage: msg.stage ?? '',
              kind: msg.kind ?? (msg.type === 'error' ? 'error' : 'progress'),
              message: msg.message ?? '',
              shot_id: msg.shot_id,
              current: msg.step ?? 0,
              total: msg.total ?? 0,
            },
          ].slice(-200),
    }
  }

  /**
   * 断了之后过几秒再连一次。
   *
   * **另外两条频道都这么做，只有这两条不做。** refs（参考图）和顶栏那条
   * 都排一个 5 秒后的重连；这两条断了就是断了——`openSocket` 只有 `start()`
   * 会调，而 `start()` 见 `polling` 已经是 true 就提前 return，于是**一次
   * 断线之后，这一整轮都退回轮询**。
   *
   * 代价不是"慢一点"：单镜那一块的进度、采样中途的预览小图、落定一镜就
   * 立刻重拉，**只从这条 socket 来**。轮询给的是总进度和阶段名。引擎那边
   * 记过这个症状——「不是"没有进度"，而是进度看着是对的、只有单镜那一块
   * 不动」（bff_routes.hpp）。写作那条同理：批量展开正文时一个个冒出来的
   * 字全在这条线上。
   *
   * ⚠️ **进来先掐掉排着的那次**，否则会像 refs 那条一样越积越多：排着
   * 重连的同时有人又调了 start()，两条都连上，消息收两遍。
   */
  function openSocket() {
    if (socket) return
    clearTimeout(retry)
    retry = null
    socket = openJobSocket('run', (msg) => {
      live.value = true
      applyMessage(msg)
    }, () => {
      live.value = false
      socket = null
      // 定时器一直开着，这几秒里退回轮询，不会断档。
      // 只在还想要进度的时候重连——stop() 之后不该自己爬起来。
      if (polling.value) retry = setTimeout(openSocket, 5000)
    })
  }

  function start(intervalMs = POLL_MS) {
    if (polling.value) return
    polling.value = true
    poll()
    // **定时器照常开着，即使 WebSocket 连上了。**
    // 它同时是兜底和补全：推上来的是增量，outputs / queue_total 这些
    // 只有全量里有。连上 WS 之后这条路的开销可以忽略。
    timer = setInterval(poll, intervalMs)
    openSocket()
  }

  function stop() {
    polling.value = false
    if (timer) clearInterval(timer)
    timer = null
    clearTimeout(retry)
    retry = null
    if (socket) {
      const sock = socket
      socket = null
      sock.close()
    }
    live.value = false
    // **正在跑的那张表要清掉。**
    //
    // 它是一份快照，而停下之后没有任何东西再更新它。留着的话，切到别的
    // 页面再切回来，那几镜的进度条**冻在离开那一刻的位置上**——而它们
    // 多半早就跑完了。用户报的原话是"切换之前完成的会卡在原来的位置上"。
    //
    // 回到页面时 start() 会重新连上，几秒内就按真实进度重新填满。
    inflightMap.value = new Map()
  }

  return {
    state, running, percent, stageLabel, events, inflight, polling, live, settled,
    settledBy,
    previewBy, previewOf,
    poll, start, stop, applyMessage,
  }
})


/** 写整季剧本 / 批量出分镜的进度。跟流水线是两条独立的线。 */
export const useWriter = defineStore('writer', () => {
  const state = ref(null)
  const polling = ref(false)
  let timer = null

  const running = computed(() => Boolean(state.value?.running))
  const percent = computed(() => {
    const s = state.value
    if (!s || !s.total) return 0
    return Math.min(100, Math.round((s.done / s.total) * 100))
  })

  const live = ref(false)
  let socket = null
  let retry = null
  let missCount = 0

  async function poll() {
    try {
      state.value = await api.seriesStatus()
      missCount = 0
      if (!state.value.running) stop()
    } catch {
      // **一次取不到不等于活儿结束了。** 原来这里是 catch 就 stop()，
      // 而轮询是这条状态线唯一的全量来源（推上来的只有增量）——停掉之后
      // 界面永远冻在最后拿到的那一帧。
      //
      // 2026-09-12 用户截图逮到的：底栏写着「AI 展开中 0/4 · 正在写 ch01」，
      // 而同一刻接口回的是 {"done":1, "message":"重写 ch02……"}。两个数
      // 一起停在 ch01 开跑那一瞬，就是轮询早就死了。左边栏反而是对的，
      // 因为它读的是另一条路。
      //
      // 分寸抄流水线那条 store：连丢几次再停。引擎重启时会连着失败几次，
      // 立刻停太急。
      missCount += 1
      if (missCount >= 5) sleepAndRetry()
    }
  }

  /**
   * 连丢五拍之后：**慢下来，但别睡死。** 理由和 useRun 里那一个一字不差
   * ——stop() 会把 socket 那条 5 秒重连一起掐掉（它判的是 polling），
   * 而之后能把这条线叫醒的只有"重新进一次故事页"。批量展开一跑一个多小时，
   * 引擎中途重启一下，底栏那个「AI 展开中 3/16」就再也不动了。
   */
  function sleepAndRetry() {
    if (!polling.value) return
    if (timer) clearInterval(timer)
    timer = setInterval(async () => {
      await poll()
      if (missCount !== 0) return
      clearInterval(timer)
      timer = setInterval(poll, 1500)
      openSocket()
    }, SLOW_POLL_MS)
  }

  /**
   * 把推上来的一条并进状态。
   *
   * **和流水线那边的映射不一样：这里推的 step 对应快照里的 done，
   * 不是 current。** 写作任务的快照只有 {running, done, total, message,
   * episodes, error}，进度条算的是 done/total。照抄流水线那边的映射
   * 会让进度条一直是 0。
   */
  function applyMessage(msg) {
    if (!msg || typeof msg !== 'object') return
    if (msg.type === 'hello') return
    if (msg.type === 'done' || msg.type === 'error') {
      poll() // episodes 和 error 只有全量里有
      return
    }
    const base = state.value ?? {}
    state.value = {
      ...base,
      running: true,
      done: typeof msg.step === 'number' ? msg.step : base.done,
      total: typeof msg.total === 'number' ? msg.total : base.total,
      message: msg.message ?? base.message,
    }
  }

  function start(intervalMs = 1500) {
    if (polling.value) return
    polling.value = true
    poll()
    // 轮询留着：推的是增量，episodes 那些只有全量里有；
    // 而且 WebSocket 连不上时它就是唯一的路。
    timer = setInterval(poll, intervalMs)
    openSocket()
  }

  /** 同 useRun 的那一个，理由见那儿。 */
  function openSocket() {
    if (socket) return
    clearTimeout(retry)
    retry = null
    socket = openJobSocket('write', (msg) => {
      live.value = true
      applyMessage(msg)
    }, () => {
      live.value = false
      socket = null
      if (polling.value) retry = setTimeout(openSocket, 5000)
    })
  }

  function stop() {
    polling.value = false
    if (timer) clearInterval(timer)
    timer = null
    clearTimeout(retry)
    retry = null
    if (socket) {
      const sock = socket
      socket = null
      sock.close()
    }
    live.value = false
  }

  return {
    state, running, percent, polling, live,
    poll, start, stop, applyMessage,
  }
})
