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
import { useThinking } from '@/stores/thinking'

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
  /** 当前定时器的间隔，retime 拿它判要不要重开。 */
  let timerMs = 0
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

    // **别的镜头挂在别的阶段上，是正常的，不能擦。**
    //
    // 这儿原来是「一收到新阶段的消息，就把还挂在上一阶段的全删掉」，
    // 依据是"流水线严格分阶段：所有镜头先出首帧，再出成片"。
    // 2026-09-17 首帧和出片改成同时跑（pipeline/shot_flow.hpp）之后这条
    // 前提没了：两个阶段的消息交替着来，于是每一条都把对方的牌子擦一遍，
    // 墙上正在跑的那几格来回闪，一次只剩一格有状态。
    //
    // 同一镜换阶段仍然会被覆盖——下面 next.set(msg.shot_id, …) 本来就是
    // 整条换掉，用不着这个循环。这个循环唯一的作用是**跨镜头**擦，
    // 而那正是现在错的那一半。
    //
    // 擦不掉的那种漏报（某个阶段只报 progress 不报 shot_done，跑过的镜头
    // 永久挂在墙上，2026-09-10 配音和首帧就是这样）靠引擎那边保证：
    // 三个阶段现在都报 shot_done，frames.cpp / render.cpp / audio.cpp 各
    // 一条，改那儿的时候要记得这边没有兜底了。

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
        // 这一对数说的是哪个阶段：'prep'（搬权重、腾显存）、'sample'（采样）
        // 还是 'decode'（VAE 分块解码）。三件事量级差很远：准备可能 26/28 段、
        // 解码 78 块，而挂了 Turbo 的采样只有 6 步。不分的话牌子上写
        // "成片 26/28"，看着就是跑了 28 步；解码写成"准备"，看着像模型又在重载。
        shotPhase:
          typeof msg.shot_phase === 'string' ? msg.shot_phase : (prev?.shotPhase ?? 'sample'),
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
    timerMs = SLOW_POLL_MS
    timer = setInterval(async () => {
      await poll()
      if (missCount !== 0) return
      clearInterval(timer)
      timerMs = POLL_MS
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
  /**
   * WS 连上了就把轮询放慢，断了再拉回常速。
   *
   * **「连上 WS 之后这条路的开销可以忽略」——量过了，不成立。**
   * 2026-09-17 从引擎日志里数：53 分钟里 `/api/run` 被请求 **1757 次**
   * （每 1.8 秒一拍），每一次回的是整份状态、**含最多 200 条事件**。
   * 那一整轮 WS 一直连着，事件本来就是推过来的。
   *
   * 全量仍然要拉——outputs / queue_total 这些只有全量里有，推上来的是
   * 增量（start() 上那段注释说的就是这件事，那部分没变）。**变的只是
   * 频率**：连着的时候 8 秒一拍够补全了，断了立刻回到 1.2 秒。
   */
  function retime() {
    if (!polling.value) return
    const want = live.value ? SLOW_POLL_MS : POLL_MS
    if (timerMs === want) return
    timerMs = want
    if (timer) clearInterval(timer)
    timer = setInterval(poll, want)
  }

  function openSocket() {
    if (socket) return
    clearTimeout(retry)
    retry = null
    socket = openJobSocket('run', (msg) => {
      const was = live.value
      live.value = true
      applyMessage(msg)
      if (!was) retime()
    }, () => {
      live.value = false
      retime()
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
    timerMs = intervalMs
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

  /**
   * 人自己按的停。**按一下之后下一拍的 error 是"已手动停止"，不是出事了。**
   * 见下面 announceFatal——和镜头那边 `stoppedByHand` 同一个办法。
   */
  /**
   * **这儿是直接 import 另一个 store，和上面 `changji:error` 那条不一样。**
   *
   * 那一条走自定义事件是为了不跟 ui store 互相认识——报错这件事全库到处
   * 都要用，而 ToastStack 本来就订着那个口子。这儿不同：thinking 是这条
   * store 自己要往里记的一份数据（谁在想、想了什么、按哪个 id 停），
   * 只有这一处推、只有这一处销号；绕一层事件反而要再找个地方去订。
   *
   * 也不会转圈：`stores/thinking.js` 一个 store 都不 import。
   */
  const thinking = useThinking()
  /**
   * 这一趟挂在哪条频道上（就是 job id）。
   *
   * 顶栏那个「停下」按的是它（`/api/job/cancel` 认 stream），而思考条目也
   * 按它存——所以收尾时要拿它去销号，不然徽章会一直挂着「正在思考」。
   */
  let stream = ''
  function clearThinking() {
    if (!stream) return
    thinking.finish(stream)
    stream = ''
  }

  let stoppedByHand = false
  /**
   * 「这一趟的停是人自己按的」。`announceFatal` 靠它把那一条不该红的错吃掉。
   *
   * **停没发出去的话要收回来**（传 false）：留着的话，接下来那趟**真的**
   * 炸了的时候（盘满了、引擎半路重启）它会被当成"自己按的停"吃掉，屏幕上
   * 一个字都不会有——而那一刻活儿压根没停，还在写。
   */
  function markStopped(on = true) {
    stoppedByHand = on
  }

  /**
   * 整批炸了要说出来。
   *
   * **单章写砸不走这儿**：那种引擎记在 `episodes[]` 里（batch.cpp
   * 「一章写砸了不该让前面几章白写，记下来接着往下写」），故事页左栏那条
   * 会把它标出来。走这儿的是**整趟活儿**挂掉——`store.load_story()` 就读
   * 不出来、盘满了写不回去、引擎半路重启——引擎把它写进 job 级的 `error`。
   *
   * 而在这之前**一处都没人读它**。实测过：让这一趟回
   * `{running:false, error:"…磁盘满了…"}`，界面上一个字都没有——章节列表
   * 照常重读一遍、看着一切正常，而十六章一章都没写。人点了「展开」等了
   * 半天，得到的是"像是没反应"。
   *
   * 走 `changji:error` 这个自定义事件而不是直接叫 ui store，理由同
   * session.js 那处：不让这两个 store 互相认识。ToastStack 订着它。
   */
  /**
   * 上一拍落地时它是不是还跑着。**这面旗子要在 poll 落地那一刻翻，
   * 不能在发请求之前取一个快照。**
   *
   * 两拍会撞上，而且是常态而不是巧合：socket 推来终止消息时
   * `applyMessage` 会立刻叫一次 poll（episodes 和 error 只有全量里有），
   * 而 1.5 秒那个定时器同时也在拉——引擎翻 running 的那一瞬正好是两条
   * 路一起动的时候。取快照的写法下，两拍都会看到"之前跑着、现在停了"：
   *
   *   · 整批炸了：同一句红字弹两遍；
   *   · **人自己按的停：弹一遍红的**——第一拍把 stoppedByHand 吃掉了，
   *     第二拍看不见那面旗子，于是把引擎写在 error 里的「已手动停止」
   *     当成事故报出来，旁边还并排站着 stopWriting() 那句绿的「已停」。
   *
   * 在这儿翻就没有这回事：先落地的那一拍翻成 false，后到的那一拍看见的
   * 就不是"跑着→停了"。
   */
  let wasRunning = false

  function announceFatal() {
    const err = state.value?.error
    if (!err) return
    if (stoppedByHand) { stoppedByHand = false; return }  // 停是自己按的，已经绿字说过
    window.dispatchEvent(
      new CustomEvent('changji:error', { detail: `批量那一趟没跑完：${err}` }),
    )
  }

  async function poll() {
    try {
      state.value = await api.seriesStatus()
      missCount = 0
      const now = running.value
      if (wasRunning && !now) announceFatal()
      wasRunning = now
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
    // **批量这几条的思考流原来落在地上。**
    //
    // 引擎那头是特意挂的（batch.cpp：「思考流挂到这条 job 的频道上。批量
    // 这几条是全流水线上跑得最久的（一整季几十分钟），最需要"它到底在想
    // 还是卡死了"这个信号」），可这条 store 的 applyMessage 只认进度，
    // `job_thinking` 一路掉进下面那个合并分支里当成空进度。
    //
    // 丢的不只是显示。**顶栏那块「AI 作业中」是它唯一的停止按钮**——
    // 那个按钮挂在思考徽章上（ThinkingBadge：「放这块不是随便挑的：它是
    // 这几分钟里唯一一直在屏幕上的东西」），而徽章只在 thinking 里有条目
    // 时才出现。没人 push，徽章就不出现，于是从设定页点「批量补分镜」
    // 之后——那一页的提示恰恰写着「顶栏那块「AI 作业中」里看进度」——
    // 屏幕上一个能按的停都没有。
    if (msg.type === 'job_thinking') {
      if (msg.job_id) {
        stream = msg.job_id
        thinking.push(msg.job_id, msg.text ?? '')
      }
      return
    }
    if (msg.type === 'done' || msg.type === 'error') {
      clearThinking()
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
    // **思考条目也要销号。** 不销的话顶栏那块「正在思考」会一直挂着——
    // 而它上面还有一个按下去没有对象的「停下」。走到这儿的路有三条：
    // 推上来的终止消息、轮询看见 running 变假、以及人自己按停。
    clearThinking()
  }

  return {
    state, running, percent, polling, live,
    poll, start, stop, applyMessage, markStopped,
  }
})
