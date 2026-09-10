/**
 * 一集的镜头表，以及它正在被怎么处理。
 *
 * **为什么抽出来。** 分镜页和制作页读的是同一份 `api.shots()`，各自画了一遍
 * 卡片、缩略图、状态、空状态——两千行里有两三百行是近似重复的。更糟的是
 * 它们会分叉：制作页学会了逐镜进度和排队之后，分镜页还停在只显示状态标签，
 * 于是同一镜在两页上说法不一样。
 *
 * 2026-09-10 两页合成一页（`ShotsView`）之后这里就是唯一的那份。
 * 留成 composable 而不是写进那个组件，是为了「成片」页那类只读的地方
 * 也能直接拿来用，不必再抄一遍拉取和轮询。
 *
 * 管三件事：
 *   1. 镜头表本身（拉、跑起来之后定期重拉）
 *   2. 每一镜此刻的进度（阶段、第几步、百分比）——只有 WebSocket 有
 *   3. 重出队列（谁在跑、谁排着、点一下该干什么）
 *
 * 编排那一半（改台词、调顺序、批量锁定）不在这儿：那是页面自己的事，
 * 而且只有一个页面用。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import { api } from '@/api'
import { STAGE_LABELS, statusOf } from '@/api/labels'
import { useRun } from '@/stores/run'
import { useSession } from '@/stores/session'
import { useUi } from '@/stores/ui'

/**
 * 出片分两段，**分开重出**。
 *
 * 一段是首帧（图像模型，约一分钟），一段是成片（视频模型，挂着 Turbo
 * 约两分钟）。要改的往往只有一段：
 *
 *   构图不对、人物站错位置 → 重出首帧（然后多半也要重出成片）
 *   构图是对的、只是动得不好 → **只重出成片**，留着那张首帧
 *
 * 后一种要是连首帧一起重跑，多花一分钟不说，还换来一张不一样的首帧——
 * 本来满意的那张就没了，等于把已经对的东西推倒重来。
 */
export const STEPS = [
  { id: 'frames', icon: 'image', label: '首帧', hint: '只重出这一镜的首帧图' },
  { id: 'final', icon: 'film', label: '成片', hint: '留着首帧，只重出视频' },
]

export function useShots() {
  const session = useSession()
  const ui = useUi()
  const runStore = useRun()

  const shots = ref([])
  const loading = ref(false)

  /**
   * 播放器和缩略图的换代号。跑完一轮加一，拼进 `src` 里。
   *
   * **重出一镜之后文件路径一个字都没变**（还是 shots/final/ep01_sh002.mp4），
   * 浏览器于是把缓存里那份旧的接着放——用户点了「重新生成」、等了两分钟、
   * 看到的还是原来那段，而且没有任何东西提示他看的是旧的。
   *
   * 只在跑完时加一，不是每次刷新都加：跑的过程中加会把正在看的那一镜
   * 从头打断。`preload="none"` 让这次换代几乎不花钱。
   */
  const bust = ref(0)

  async function load() {
    if (!session.projectPath || !session.episodeId) {
      shots.value = []
      return
    }
    loading.value = true
    try {
      const data = await api.shots(session.projectPath, session.episodeId)
      shots.value = data.shots ?? []
    } catch (err) {
      // 还没出分镜时引擎会 404。这不是错，是流程还没走到。
      if (err.status !== 404) ui.error(err.message)
      shots.value = []
    } finally {
      loading.value = false
    }
  }

  watch(() => [session.projectPath, session.episodeId], load, { immediate: true })

  // ---- 每一镜此刻在干什么 ----

  /** 正在跑的那几镜，按 shot_id 索引。 */
  const inflightBy = computed(() => {
    const m = {}
    for (const x of runStore.inflight) m[x.shot_id] = x
    return m
  })

  /**
   * 已经交给引擎、但它还没报出第一条进度的那几镜。
   *
   * **点下去到第一条进度之间能隔一分钟**——那段时间引擎在载模型，
   * 一个字都不会报。这期间牌子上显示的还是上一轮的"成片完成"：
   * 用户点了按钮，画面一动不动，只能再点一次。
   */
  const sent = ref(new Set())

  /**
   * 还没交给引擎、在这儿排着的。**`shot_id` → 要重出哪几段**。
   *
   * **队列在前端，不在引擎。** `POST /api/run` 在有任务跑着的时候回 409
   * （"已经在跑 ep01 了"），而那条 409 是和 Python 逐字节对拍的，动不得。
   * 所以跑着的时候点别的镜头不发请求，先记在这儿，这一轮完了再提交。
   *
   * 这样"点一个别的都点不了"就没有了——用户可以一路点过去，
   * 挑出十几个要重出的，然后走开。
   *
   * 值是集合而不是单个字符串：同一镜可以同时排着"重出首帧"和"重出成片"，
   * 后点的不该把先点的顶掉。
   */
  const waiting = ref(new Map())

  /** 这一镜正在被处理：引擎在跑、已提交、或者在前端排着。 */
  function busy(shotId) {
    return (
      Boolean(inflightBy.value[shotId]) ||
      sent.value.has(shotId) ||
      waiting.value.has(shotId)
    )
  }

  /** 引擎正在跑这一镜（或者刚交出去还没回音）。那时候只给一个暂停。 */
  function shotRunning(shotId) {
    return Boolean(inflightBy.value[shotId]) || sent.value.has(shotId)
  }

  /** 这一镜的这一段，是不是在队列里排着。 */
  function isWaiting(shotId, step) {
    return Boolean(waiting.value.get(shotId)?.has(step))
  }

  // 引擎开始报这一镜了，"排队中"就该让位给真进度。**两个集合都要清**：
  // 只清 sent 的话，一个既在 waiting 里又被引擎跑着的镜头会同时显示
  // "首帧"（状态取 inflight）和"不重出这一镜了"（按钮取 waiting），
  // 同一格上两个互相矛盾的说法。
  watch(inflightBy, (now) => {
    const ids = Object.keys(now)
    if (!ids.length) return
    if (sent.value.size) {
      const next = new Set(sent.value)
      for (const id of ids) next.delete(id)
      if (next.size !== sent.value.size) sent.value = next
    }
    if (waiting.value.size) {
      const next = new Map(waiting.value)
      for (const id of ids) next.delete(id)
      if (next.size !== waiting.value.size) waiting.value = next
    }
  })

  /**
   * 这一镜跑到百分之几。**没在跑、或者引擎没给这一镜的步数就返回 null**，
   * 由界面决定画走马灯还是画具体进度。
   *
   * 别用 inflight 里的 step/total：那是整集的位置（第 21 镜 / 共 22 镜），
   * 拿它画单镜的条，正在跑的那一镜一出现就是 95%，跑完还是 95%。
   */
  function pct(shotId) {
    const x = inflightBy.value[shotId]
    if (!x || typeof x.shotStep !== 'number' || !x.shotSteps) return null
    return Math.min(100, Math.round((x.shotStep / x.shotSteps) * 100))
  }

  /**
   * 这一镜在干什么，到第几步了。
   *
   * 只有阶段名（"首帧"）的话，一条几十秒不动的进度条和卡死了看着一样。
   * 带上步数就有了在走的证据。
   *
   * **准备和采样要分开说**：准备（搬权重、VAE 分块解码）可能是 282/351 段，
   * 而挂了 Turbo 的采样只有 6 步。都写成"成片 282/351"的话，看着就是跑了
   * 几百步——用户会以为 Turbo 没生效，已经问过一次了。
   */
  function shotState(shot) {
    const x = inflightBy.value[shot.shot_id]
    if (x) {
      const stage = STAGE_LABELS[x.stage] || x.stage || ''
      if (typeof x.shotStep === 'number' && x.shotSteps) {
        if (x.shotPrep) return `${stage}·准备 ${x.shotStep}/${x.shotSteps}`
        return `${stage} ${x.shotStep}/${x.shotSteps} 步`
      }
      return stage || '跑着'
    }
    if (busy(shot.shot_id)) return '排队中'

    // **牌子上说的该是"这一格能看到什么"，不是流水线跑到哪一步了。**
    //
    // 一个既没首帧也没片子的镜头，格子里是空的，而状态是 audio_done，
    // 于是底下写着「配音完成」——技术上没错（配音跑过了、时长锁了），
    // 但在一面看画面的墙上，"完成"两个字读起来就是"这一镜做好了"。
    // 用户报的原话是"什么都没有的还是显示成配音完成"。
    //
    // 出问题的状态（未过闸、降级）**照原样显示**：那是必须看见的东西，
    // 不能被一句"还没出画面"盖掉。
    const st = statusOf(shot.status)
    if (blank(shot) && st.tone !== 'warn') return '还没出画面'
    return st.label
  }

  /** 这一格什么都没有：没首帧也没片子，看过去就是一块空的。 */
  function blank(shot) {
    return !shot.frame_path && !shot.video_path
  }

  /**
   * 牌子边框和时间轴那一格的颜色。
   *
   * 同 shotState 一个道理：`audio_done` 的 tone 是 info（"进行中"的蓝），
   * 而那一格里什么都没有。蓝色配一块空白，扫过去会当成"这几镜在跑"。
   * 什么都没出的一律按"未开工"的灰算，除非它是个要看见的问题。
   */
  function shotTone(shot) {
    const st = statusOf(shot.status)
    if (blank(shot) && st.tone !== 'warn') return 'neutral'
    return st.tone
  }

  // ---- 开跑、停下、排队 ----

  /**
   * 开跑。
   *
   * `ids` 为空是整集全流程。非空是只跑那几镜；再给 `stages` 就只跑那几段
   * （`['frames']` 只出首帧，`['final']` 只出视频，不给就是配音、首帧、
   * 成片全走一遍）。
   */
  async function start(ids = [], stages = null, force = null) {
    const one = ids.length > 0
    // **点下去立刻点亮，别等引擎。**
    //
    // 引擎要载模型，第一条进度可能是一分钟以后的事。这一分钟里牌子上
    // 显示的还是上一轮的状态，和"没点上"看起来一模一样。
    //
    // 整集出片时点亮的是**引擎接下来会挨个跑的那些**——也就是还没到终态
    // 的。以前这儿只处理了单镜那条（`if (one)`），点「出片」整面墙一动
    // 不动，用户报的就是这个。
    const lit = one
      ? ids
      : shots.value
          .filter((s) => !['final_done', 'locked', 'fallback'].includes(s.status))
          .map((s) => s.shot_id)
    // **同步先记上**，在 await 之前。紧接着再点别的镜头时，判据
    // （`sent` 非空）才来得及生效，否则那一下会走提交那条路撞 409。
    const before = sent.value
    sent.value = new Set([...sent.value, ...lit])
    try {
      await api.run({
        project: session.projectPath,
        episode_id: session.episodeId,
        // 重跑单镜时**一定要带 force**：那一镜已经是完成状态，
        // 不带的话它不在待办里，跑完什么都没变而且不报错。
        // 整集那条默认不 force（接着没跑完的往下跑）；「全部重出」
        // 会显式传真，否则那个按钮点了什么都不会发生。
        force: force === null ? one : force,
        ...(one ? { shot_ids: ids } : {}),
        ...(stages ? { stages } : {}),
      })
    } catch (err) {
      sent.value = before   // 没起来就别亮着
      // 409（已经在跑）由调用方接住改成排队，别在这儿弹红框。
      return { ok: false, error: err }
    }
    runStore.start()
    return { ok: true }
  }

  async function stop() {
    try {
      await api.stopRun()
      ui.ok('已停，跑完的镜头留着')
    } catch (err) {
      ui.error(err.message)
    }
    runStore.poll()
  }

  /**
   * 把攒着的交给引擎。空的就什么都不做。
   *
   * **按段分组，一次只交一组**：`/api/run` 的 `stages` 是整个请求共用的，
   * 一次请求没法让 A 镜只出首帧、B 镜只出成片。而它在有任务跑着时回 409，
   * 所以也不能连发两个。交完一组，剩下的留到这一组跑完再交——
   * 最多两组，自己会排干。
   *
   * **首帧那一组先走**：真有一镜两段都排着的话，顺序本来就该是先首帧
   * 再成片（成片拿首帧当起始图）。反过来那一镜的成片用的是旧首帧。
   */
  function flushWaiting() {
    if (!waiting.value.size) return
    for (const step of STEPS) {
      const ids = [...waiting.value.entries()]
        .filter(([, steps]) => steps.has(step.id))
        .map(([id]) => id)
      if (!ids.length) continue

      const next = new Map()
      for (const [id, steps] of waiting.value) {
        const rest = new Set([...steps].filter((x) => x !== step.id))
        if (rest.size) next.set(id, rest)
      }
      waiting.value = next
      start(ids, [step.id])
      return
    }
  }

  function enqueue(id, step) {
    const next = new Map(waiting.value)
    next.set(id, new Set([...(next.get(id) ?? []), step]))
    waiting.value = next
  }

  /**
   * 点了某一镜的「首帧」或「成片」。**四件事**，看此刻是什么状态：
   *
   *   这一段在排队    → 取消，从队列里拿掉，不发任何请求
   *   这一镜正在跑    → 停下这一轮（引擎只有整轮的停，没有单镜的停）
   *   有别的在跑      → 排进队列，这一轮完了再交
   *   什么都没在跑    → 立刻交出去，只跑这一段
   */
  async function shotAction(shot, step) {
    const id = shot.shot_id
    // **先判"引擎正在跑它"。** 反过来的话，一个刚排进队列、紧接着就被
    // 引擎接手的镜头会一直按"排队中"处理——按钮画的是取消，
    // 而它其实已经在跑了，取消什么都不会发生。
    if (shotRunning(id)) {
      await stop()
      return
    }
    if (isWaiting(id, step)) {
      const next = new Map(waiting.value)
      const rest = new Set([...next.get(id)].filter((x) => x !== step))
      if (rest.size) next.set(id, rest)
      else next.delete(id)
      waiting.value = next
      return
    }

    // **这一段必须在 await 之前跑完。** 连着点两下的话，第二下发生在
    // 第一下的请求还没回来的时候，那时 runStore.running 还是假（要等下一次
    // 轮询，最长 1.2 秒），于是第二镜也走了提交那条路——而那条路上引擎会
    // 回 409，`start` 返回失败，那一格什么都不显示。用户报过这个。
    //
    // 判据换成"这一轮已经交出去过东西了"（sent 非空），并且**同步**先记上。
    if (runStore.running || sent.value.size > 0) {
      enqueue(id, step)
      return
    }
    sent.value = new Set([...sent.value, id])
    const r = await start([id], [step])
    if (!r.ok) {
      // 没提交上（引擎正忙、或者别的浏览器抢先了）。**别丢掉**，
      // 挪进队列等这一轮完——丢掉的话用户点过的那一下就白点了。
      sent.value = new Set([...sent.value].filter((x) => x !== id))
      enqueue(id, step)
    }
  }

  /** 「首帧」或「成片」那个按钮该画成什么。 */
  function stepBtn(shot, step) {
    if (isWaiting(shot.shot_id, step.id)) {
      return { icon: 'close', title: `不重出${step.label}了` }
    }
    if (runStore.running || sent.value.size > 0) {
      return { icon: step.icon, title: `排进队列，这一轮跑完重出${step.label}` }
    }
    return { icon: step.icon, title: step.hint }
  }

  // ---- 跑起来之后定期重拉 ----
  //
  // 镜头墙是唯一能看见「片子长什么样」的地方。不刷的话它停在开跑那一刻，
  // 几十分钟里画面一动不动——用户没法判断出来的东西对不对，
  // 只能等全跑完才发现方向就错了。六秒一次：首帧出一张要几十秒，跟得上。
  let timer = null
  function watchShots(on) {
    if (on && !timer) timer = setInterval(load, 6000)
    if (!on && timer) {
      clearInterval(timer)
      timer = null
    }
  }
  watch(() => runStore.running, (now) => watchShots(now))
  watch(
    () => runStore.running,
    (now, before) => {
      if (!(before && !now)) return
      load()
      // 这一轮交出去的那几个跑完了（或者失败了），不该再挂着"排队中"。
      sent.value = new Set()
      // 刚跑完，磁盘上那几个 mp4 换过了但路径没变。见 bust 的注释。
      bust.value += 1
      session.refresh()
      if (runStore.state?.error) ui.error(runStore.state.error)
      else ui.ok('这一轮跑完了')
      // 跑的过程中攒下的那几镜，现在交出去。
      flushWaiting()
    },
  )

  // **进页面就把轮询和 WebSocket 开起来。**
  //
  // 不开的话，跑着的时候刷新一下页面（或者从别处点进来），整面墙一动不动：
  // 引擎在跑，而这一页既没轮询也没连上 WebSocket，什么都不知道。
  // 原来这一句在 ProductionView 的 onMounted 里，两页合并时漏掉了。
  onMounted(() => runStore.start())
  onUnmounted(() => {
    runStore.stop()
    watchShots(false)
  })

  return {
    shots, loading, load, bust,
    inflightBy, pct, shotState, busy, shotRunning, isWaiting,
    start, stop, shotAction, stepBtn, shotTone,
    running: computed(() => runStore.running),
  }
}
