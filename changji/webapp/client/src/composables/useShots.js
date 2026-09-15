/**
 * 一集的镜头表，以及它正在被怎么处理。
 *
 * **为什么抽出来。** 分镜页和制作页读的是同一份 `api.shots()`，各自画了一遍
 * 卡片、缩略图、状态、空状态——两千行里有两三百行是近似重复的。更糟的是
 * 它们会分叉：制作页学会了逐镜进度和排队之后，分镜页还停在只显示状态标签，
 * 于是同一镜在两页上说法不一样。
 *
 * 2026-09-10 两页合成一页（今天是「这一集」下面的 `EpShots`）之后，
 * 这里就是唯一的那份。
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
import { useLongRunning, useRetryWhenBack } from '@/composables/useSystemFeed'
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
  // **顺序就是流水线的顺序**：配音定时长，时长定帧数，帧数定画面。
  //
  // 配音这一格 2026-09-13 补的。以前墙上只有首帧和成片两个按钮，配音
  // 只能跟着「只出首帧」捎带跑，单独重跑一镜的配音**在界面上没有入口**
  // ——而改完角色音色之后要做的恰恰就是这件事。用户报的原话是
  // 「配音在界面上都不显示」。
  //
  // 单镜重跑本来就带 force（见下面 start 里那段），所以不用改引擎：
  // 配音那一段的入口状态是 PLANNED，force 之下不看状态。
  { id: 'audio', icon: 'play', label: '配音', hint: '只重出这一镜的配音；改完音色用它' },
  { id: 'frames', icon: 'image', label: '首帧', hint: '只重出这一镜的首帧图' },
  { id: 'final', icon: 'film', label: '成片', hint: '留着首帧，只重出视频' },
]

/**
 * 重出队列**放在模块作用域，不放在组件里**。
 *
 * 组件里的话，切到别的页面那一刻整个 useShots 就析构了，队列跟着没——
 * 切回来时排着的那几镜全变回普通状态，用户报的原话是
 * "从别的页面切回来排队中的不会再显示排队中了"。
 *
 * 而队列的寿命本来就该比一个页面长：点几个要重出的、去改改剧本、
 * 回来接着排，这是很正常的用法。
 *
 * 换项目或换集时清空（见下面那个 watch）——那时候这些 shot_id 已经
 * 不属于当前这一集了，留着只会在别的集上点亮几个不相干的格子。
 */
const sent = ref(new Set())
const waiting = ref(new Map())
/** 播放器和缩略图的换代号。同样跨页面留着，否则切回来又会看到缓存里的旧片子。 */
const bust = ref(0)
/** 上一次这几个集合属于哪一集。换了就清空。 */
let ownedBy = ''

export function useShots() {
  const session = useSession()
  const ui = useUi()
  const runStore = useRun()
  // 批量补分镜（/api/plan/all）跑在"写"那个槽上，不是"出片"那个。
  // 不订它的话，那一轮给这一集补完分镜，墙上还是空的——人要切一次 tab
  // 或者刷新才看得到，而那一轮正是他在这一页上点着「去补」起的。

  const shots = ref([])
  /** 这一趟读砸了的话，那句话。空串 = 没砸（包括"还没出分镜"那个 404）。 */
  const loadError = ref('')
  /**
   * 这一集真正会出多长（秒），由引擎给。
   *
   * **不要在前端把每镜的 `duration_s` 加起来。** 那是名义值，而模型只能按
   * 格子出帧（Wan 4n+1、MiniMax-H3 17k+5），名义 4 秒出来是 4.458 秒。
   * 格子规则跟着模型和显存变，只有引擎知道（`stages::VideoLimits`），
   * 在这儿复刻一份就是第三份副本，换模型就全错。
   *
   * null = 引擎没回这个字段（老版本），由调用方决定怎么兜底。
   */
  const episodeDuration = ref(null)
  const loading = ref(false)

  async function load() {
    if (!session.projectPath || !session.episodeId) {
      shots.value = []
      episodeDuration.value = null
      // **早返回也要把转圈关掉。** 上一趟要是被顶掉了，它的 finally 里那句
      // `if (mine()) loading.value = false` 不会动 loading——而这一趟又从这儿
      // 就回去了，于是转圈永远停在真。界面上的表现是空状态和列表都不画
      // （两个都判着 `!loading`），一片空白。
      loading.value = false
      return
    }
    // **这一趟是给哪一集读的。**
    //
    // 顶栏连着换两集，两趟请求都在路上，回来的顺序不保证——慢的那趟后
    // 落地，ep02 的标题下面摆的就是 ep01 的镜头墙。而这一页上每个格子的
    // 按钮都按 `shot_id` 发请求：点「重出」送的是 ep01 的镜头号配 ep02 的
    // 集号，轻则 404，重则那一集里正好有同号的镜头，重出的是另一集的东西。
    const want = `${session.projectPath}::${session.episodeId}`
    const mine = () => want === `${session.projectPath}::${session.episodeId}`
    loading.value = true
    try {
      const data = await api.shots(session.projectPath, session.episodeId)
      if (!mine()) return
      shots.value = data.shots ?? []
      episodeDuration.value = data.duration_s ?? null
      loadError.value = ''
    } catch (err) {
      if (!mine()) return
      // ⚠️ **404 不是"还没出分镜"。**
      //
      // 这儿原来写着「还没出分镜时引擎会 404。这不是错，是流程还没走到」
      // 并据此把 404 整个吞掉。查了引擎那一侧（readonly.cpp 的 `get_shots`）
      // ——**还没出分镜回的是 200 加一个空数组**；那里唯一的 404 是
      // `没有剧集 <id>`，也就是这个集号根本不存在。
      //
      // 于是吞掉 404 等于：集号对不上（localStorage 里记着一个已经被删的
      // 集、或者别的标签页刚删掉它）时，界面和"这一集还没分镜"长得一模一样，
      // 一声不吭，底下还摆着「AI 出分镜」——点下去是给一个不存在的剧集排
      // 分镜，再撞一次错。实跑对照过：200 空表和 404 两种情形，屏幕上一个
      // 字都不差。
      //
      // 现在 404 和别的错一样处理：上面那句提示 + 下面的 loadError。
      ui.error(err.message)
      // **读砸了和"还没出"是两件事，界面上要分得开。**
      //
      // 下面这句清空是必须的：换集时这一趟要是砸了，手里那份还是**上一集**
      // 的镜头墙，而每个格子的按钮都按 shot_id 发请求。但清空之后，光看
      // `!shots.length` 的话两件事长得一模一样，于是读砸了也摆出「还没有
      // 分镜 · AI 出分镜」——那一屏说的是"这一集还没分镜"，而这会儿到底有
      // 没有根本不知道。按下去就是拿一份新的盖掉可能还在的那份。
      //
      // 和 StoryView 上那段是同一条道理（「读不出来的时候不能摆"开始写"
      // 那一屏」），AssetCharacters / AssetLocations 也各有一份。
      loadError.value = err.message
      shots.value = []
      episodeDuration.value = null
    } finally {
      // 过期那一趟的 finally 会在新那趟还读着的时候把转圈关掉
      if (mine()) loading.value = false
    }
  }

  watch(
    () => [session.projectPath, session.episodeId],
    () => {
      // 换项目或换集：队列里那些 shot_id 不属于这一集了，留着只会在
      // 别的集上点亮几个不相干的格子。见 sent / waiting 的注释。
      const key = `${session.projectPath}::${session.episodeId}`
      if (key !== ownedBy) {
        ownedBy = key
        sent.value = new Set()
        waiting.value = new Map()
      }
      load()
    },
    { immediate: true },
  )

  // ---- 每一镜此刻在干什么 ----

  /** 正在跑的那几镜，按 shot_id 索引。 */
  const inflightBy = computed(() => {
    const m = {}
    for (const x of runStore.inflight) m[x.shot_id] = x
    return m
  })

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
   *
   * **准备段返回 null，走马灯。** 那一段的分母不是一个，它会一轮一轮地
   * 重来：出片一镜实测 0→28 跑满三遍（扩散模型、编码器、VAE 各一遍），
   * 采样完了 VAE 分块解码又是一轮 0→351。拿它画条的话，进度条在一镜里
   * **倒退四次**——比不动更像出了事。
   *
   * 下面 shotState 那段注释里早就把准备和采样分开说了（文字上写"准备
   * 8/28"和"成片 3/6 步"），条没跟上而已。走马灯在这儿是诚实的：那一段
   * 确实不知道还要多久。
   */
  function pct(shotId) {
    const x = inflightBy.value[shotId]
    if (!x || typeof x.shotStep !== 'number' || !x.shotSteps) return null
    if (x.shotPrep) return null
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
      // 有准备标志但没步数：正在腾显存 / 从磁盘读模型，还没进采样。
      // 这一段几十秒起，只写阶段名的话和卡死了看着一样。
      if (x.shotPrep) return `${stage}·正在准备模型`
      return stage || '跑着'
    }
    // **排的队盖不住已经出来的东西。**
    //
    // 整集出片时排队的是所有没到终态的镜头，其中不少已经有首帧了——
    // 它们在等的是出视频那一段。可牌子上一律写「排队中」的话，
    // 一张已经出好的首帧就被这三个字盖住了，用户看到的是
    // "首帧完成了还不显示图片和首帧完成"（2026-09-10 报的）。
    //
    // 有东西可看就说它是什么；"在排队"这件事由边框和那条走马灯说，
    // 不占这一行的字。什么都没有的才写「排队中」——那时候确实没别的可说。
    if (busy(shot.shot_id) && blank(shot)) return '排队中'

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
  /**
   * 整集跑一轮时，引擎会碰到哪几镜。
   *
   * **和点亮牌子那件事必须对得上。** 多点亮一格，那一格就挂着「排队中」
   * 直到整轮结束，而引擎根本没打算碰它。
   *
   * `force` 之下 `pick` 不看状态，全都要跑。否则按各阶段的入口状态算：
   * 只出首帧 → 还没有首帧的；只出成片 → 还没有视频的；全流程 → 没到终态的。
   */
  function willRun(stages, force) {
    const all = shots.value
    if (force) return all
    const only = new Set(stages ?? [])
    if (only.size && !only.has('final')) {
      if (only.has('frames')) return all.filter((s) => !s.frame_path)
      return all   // audio 之类：这一层不细分，宁可多亮不要漏
    }
    if (only.has('final')) return all.filter((s) => !s.video_path)
    return all.filter(
      (s) => !['final_done', 'locked', 'fallback'].includes(s.status),
    )
  }

  /**
   * `/api/run` 这一发还在路上。
   *
   * **界面上那三个按钮本来就想拿它变灰**（`:disabled="...isBusy('start')"`），
   * 但 useShots 整个不走 useAction——没有任何人登记过 `start` 这个 key，
   * 那三处判断永远是假。连点两下的后果：第二发撞 409，弹一句「已经在跑了」；
   * 整集那条还会连着弹两个 confirm。
   */
  const starting = ref(false)

  async function start(ids = [], stages = null, force = null) {
    if (starting.value) return { ok: false, error: null }
    starting.value = true
    try {
      return await startInner(ids, stages, force)
    } finally {
      starting.value = false
    }
  }

  async function startInner(ids, stages, force) {
    const one = ids.length > 0
    // **点下去立刻点亮，别等引擎。**
    //
    // 引擎要载模型，第一条进度可能是一分钟以后的事。这一分钟里牌子上
    // 显示的还是上一轮的状态，和"没点上"看起来一模一样。
    //
    // 整集出片时点亮的是**引擎接下来会挨个跑的那些**——也就是还没到终态
    // 的。以前这儿只处理了单镜那条（`if (one)`），点「出片」整面墙一动
    // 不动，用户报的就是这个。
    // 点亮哪些，要和引擎**实际会跑**哪些对上。多点亮一格的后果是
    // 那一格挂着「排队中」直到整轮结束，而引擎根本没打算碰它。
    const lit = one ? ids : willRun(stages, force).map((s) => s.shot_id)

    // **同步先记上**，在 await 之前。紧接着再点别的镜头时，判据
    // （`sent` 非空）才来得及生效，否则那一下会走提交那条路撞 409。
    const before = sent.value
    sent.value = new Set([...sent.value, ...lit])
    let res
    try {
      res = await api.run({
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
    /**
     * **引擎说它有键没认出来的时候，得说一声。**
     *
     * `/api/run` 刻意不 forbid 多余的键（前端和引擎的版本不一定同步升，
     * 多一个键就 422 会让整个功能挂掉），改成"照收不误、但把不认识的列
     * 回来"。引擎那段注释写着这条存在的理由，是一次真事故：把 `shot_ids`
     * 误写成 `only_shots`，引擎一声不吭当成"没指定镜头"，于是「重出这一镜」
     * 变成整集重渲 18 镜、跑了二十分钟。
     *
     * 它接着写「写错字段名的当场就知道」——**而只有有人看才知道**。
     * 这一发的返回原来是整个扔掉的（`await api.run(...)` 连接都不接），
     * 于是那条通道从头到尾没人听：唯一的客户端就是这儿。
     *
     * 正常用法下它是静默的（这一版发的五个键引擎都认）。真响了就说明
     * 手里这份前端和引擎对不上——多半是浏览器攥着旧的 index.html，
     * router/chunk-error.js 兜的正是同一种情况——而这一轮真跑的，很可能
     * 和你按的不是一回事。
     */
    const ignored = res?.ignored_fields
    if (Array.isArray(ignored) && ignored.length) {
      ui.warn(
        `引擎不认识这几项、已经忽略：${ignored.join('、')}。` +
          '这一轮跑的可能和你按的不是一回事，刷新一次再试',
      )
    }
    runStore.start()
    return { ok: true }
  }

  /**
   * 人自己按的停。下降沿那条提示据此闭嘴——理由见那儿。
   *
   * 只在**确实有活在跑**的时候立这个旗：没在跑时按停不会有下降沿，旗留着
   * 会把下一轮真正跑完的那句提示吃掉。
   */
  let stoppedByHand = false

  async function stop() {
    // **要不要立那个旗，得在发请求之前看。** 等回来再看的话引擎可能已经停
    // 了，`running` 变成假，旗就立不上——于是底下那个下降沿照样弹一句
    // 「这一轮跑完了」，而这一轮是人按停的。
    const wasRunning = runStore.running
    const queued = waiting.value.size

    try {
      await api.stopRun()
    } catch (err) {
      // ⚠️ **没停成就什么都别动。** 这儿原来是先把 `waiting` 清空、先立
      // `stoppedByHand`，再发请求——请求砸了的话：
      //
      //   · 排着的那几镜**凭空没了**。它们只活在这个 Map 里，一个字都没
      //     发给引擎；而屏幕上只有一句原始报错，不会说"顺手把 5 镜也扔了"。
      //   · `stoppedByHand` 还立着，会把下一轮**真正跑完**的那句提示吃掉
      //     ——这正是这个旗上面那段注释在防的事。
      //
      // 而那一刻引擎压根没停，还在跑。
      ui.error(`没停下来：${err.message}`)
      runStore.poll()
      return
    }

    // **排着的那几镜也一起取消。**
    //
    // 出队是挂在 `running` 的下降沿上的（见下面那个 watch：这一轮完了就把
    // 攒下的交出去），而**按「停下」走的是同一个下降沿**。不清队列的话，
    // 人按停、这一轮一停，排着的那几镜立刻又交出去开跑——「停」停不干净，
    // 而且下一轮开跑时屏幕上还写着一句「这一轮跑完了」。
    //
    // 清掉而不是留着：这一页没有"接着跑排队的"那个入口，留着的话它们要等
    // 到某一轮**不相干的**运行结束时才突然开跑，那比现在更莫名其妙。
    if (wasRunning) stoppedByHand = true
    waiting.value = new Map()
    ui.ok(queued ? `已停，跑完的镜头留着；排着的 ${queued} 镜也取消了` : '已停，跑完的镜头留着')
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
      // **没发出去就放回队列。**
      //
      // 这一组是在上面那几行里先从 waiting 里摘掉的（要先摘，不然下一次
      // flush 会把同一批再交一遍）。而 start 有两条不发的路：引擎那边已经
      // 有别的活在跑（409），或者上一发还在路上（starting）。原来两条都是
      // 直接丢——人点的「重出」连同格子上那个「排队中」一起悄悄没了，
      // 不报错，也不会再试。
      //
      // 放回去而不是整份还原：这半秒里可能又有人点了别的镜头。
      start(ids, [step.id]).then((r) => {
        if (r?.ok) return
        const back = new Map(waiting.value)
        for (const id of ids) {
          const set = new Set(back.get(id) ?? [])
          set.add(step.id)
          back.set(id, set)
        }
        waiting.value = back
      })
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

  /**
   * 「配音」「首帧」「成片」那几个按钮各该画成什么。**四种状态**，和
   * `shotAction` 的四条路一一对着——那四条路的判断顺序就是这儿的顺序。
   *
   * ⚠️ **正在跑的那一镜，这颗按钮是「停」。** `shotAction` 第一条路就是
   * `stop()`（引擎只有整轮的停，没有单镜的），而这儿原来没有对应的分支：
   * 抽屉开在正在渲染的那一镜上时，按钮写着「重出成片」、提示写着「排进队列，
   * 这一轮跑完重出」——按下去却是停掉整轮。说的和做的不是一回事，而代价是
   * 一轮几十分钟到几小时的渲染。
   */
  function stepBtn(shot, step) {
    // ⚠️ **「正在跑」要排在「排队中」前面，和 shotAction 一模一样。**
    //
    // 一镜可以同时是这两种：这一轮跑着别的镜头时点了它的「重出成片」
    // （进 waiting），跑着跑着引擎自己走到了它（进 inflight）。那时候两条
    // 分支都成立，而 `shotAction` 第一条判的是"正在跑"——按下去是 `stop()`，
    // 停的是整轮。这儿原来先判排队，按钮上写着「取消排队」，按下去却把一轮
    // 几十分钟到几小时的渲染停掉了。
    if (shotRunning(shot.shot_id)) {
      return {
        icon: 'pause',
        label: '停下这一轮',
        title: '这一镜正在跑。引擎只能整轮地停，按下去这一轮都停',
      }
    }
    if (isWaiting(shot.shot_id, step.id)) {
      return { icon: 'close', label: '取消排队', title: `不重出${step.label}了` }
    }
    if (runStore.running || sent.value.size > 0) {
      return {
        icon: step.icon,
        label: `重出${step.label}`,
        title: `排进队列，这一轮跑完重出${step.label}`,
      }
    }
    return { icon: step.icon, label: `重出${step.label}`, title: step.hint }
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

  // **落定一镜就立刻重拉，别等定时器。**
  //
  // 上面那个六秒的定时器在标签页不在前台时会被 Chrome 压到一分钟一次
  // （实测两分半里只拉了两次），首帧出来了牌子上要等一分钟才变。
  // 而 WebSocket 不受这个限制，引擎每落定一镜发一条 shot_done——那条就是
  // 信号本身。多镜连着落定时合并成一次拉，免得一秒内打好几个请求。
  let settleTimer = null
  watch(() => runStore.settled, () => {
    if (settleTimer) clearTimeout(settleTimer)
    settleTimer = setTimeout(load, 250)
  })

  /**
   * 问引擎这一轮还有哪几镜没落定，把它们点亮。
   *
   * **「排队中」不能只活在浏览器内存里。** 刷新一下、换个标签页、换台设备，
   * 排着的全没了；正在跑的那一镜也要等到下一条进度才亮。引擎每个阶段
   * 开工时登记一批、每落定一镜划掉一个，`/bff/run/pending` 就是那份名单。
   * 已经在 inflight 里的不动——那是比「排队中」更具体的信息。
   */
  async function syncPending() {
    try {
      const d = await api.runPending()
      if (!d?.running || !d.shot_ids?.length) return
      const add = d.shot_ids.filter((id) => !inflightBy.value[id])
      if (add.length) sent.value = new Set([...sent.value, ...add])
    } catch {
      // 老引擎没有这条。那就退回浏览器自己记的那份。
    }
  }

  // **回到前台那一刻也拉一次。** 藏在后台时定时器基本停摆，
  // 切回来看到的还是走开那一刻的墙——先拉一遍再说。
  function onVisible() {
    if (document.visibilityState !== 'visible') return
    load()
    runStore.poll()
    syncPending()
  }
  // 引擎重启之后自己回来：这一页停在「读不到这一集的分镜」上时，
  // 那份表一回来就重读一趟。见 useRetryWhenBack。
  useRetryWhenBack(() => loadError.value, load)

  // 批量那条跑完也重拉一次。只订下降沿：跑的过程中它一章一集地写，
  // 而这一页要的是"这一集的分镜出来了没有"。
  //
  // `session.refresh()` 也要跟着叫，理由同下面出片那条：顶栏那个集号下拉
  // 上写着每一集多少镜、侧边那几个对勾也按分镜算，而批量补分镜正好把这两
  // 样都改了。不叫的话它们停在开跑之前，直到下一次换集或者干完点别的。
  // ⚠️ **这儿原来盯的是 `writeStore.running`，而这一页没有人驱动它。**
  // `useWriter` 的轮询只有故事页和设定页「分集」那一格会开；在那一格点完
  // 「批量补分镜」，人多半直接过来这一页等——而这一页上那个旗子从头到尾
  // 是假的，下降沿一次都不来。上面那段话说的正是这件事。
  // 换成那份系统表（每一页都在拉），只认「写」那个槽。见 useLongRunning。
  watch(useLongRunning(['write']), (now, before) => {
    if (!(before === true && now === false)) return
    load()
    session.refresh()
  })
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
      // **自己按的停，别再红一次。**
      //
      // 引擎把「已手动停止」写进 `state.error`（jobs.cpp 里 cancel 那段），
      // 而 stop() 已经用一句绿的说过了。不判的话按一次停弹两条，其中一条
      // 是红的——人会去找哪里出错了，而什么都没错。
      if (stoppedByHand) stoppedByHand = false
      else if (runStore.state?.error) ui.error(runStore.state.error)
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
  onMounted(async () => {
    document.addEventListener('visibilitychange', onVisible)
    runStore.start()
    // **回到页面时对一次账。**
    //
    // 走开的这段时间里那一轮可能已经跑完了，而"清空 sent、把 waiting 交出去"
    // 的那个 watch 只在页面开着的时候才会触发。不对账的话，切回来看到的是
    // 几格永远的「排队中」，而队列里排着的那几镜谁也不会去跑。
    await runStore.poll()
    if (runStore.running) {
      // 正跑着：把引擎还没落定的那几镜点亮，别等它们各自报出第一条进度。
      syncPending()
      return
    }
    if (sent.value.size) sent.value = new Set()
    flushWaiting()
  })
  onUnmounted(() => {
    document.removeEventListener('visibilitychange', onVisible)
    if (settleTimer) clearTimeout(settleTimer)
    runStore.stop()
    watchShots(false)
  })

  /** 这一镜此刻的预览图（采样中途），没有就是空串。见 run store 的 previewBy。 */
  const previewOf = (shotId) => runStore.previewOf(shotId)

  /**
   * 这一镜的图该用哪一代。
   *
   * 首帧和视频都写在固定路径上，重出是原地覆盖、地址不变，浏览器会一直
   * 拿缓存里那张。整轮跑完那一下 `bust` 会把全墙换一次代，但一轮几十分钟
   * ——中间每一镜跑完那一下，预览图消失、牌子当场"变回"老样子，而人正盯
   * 着看新的出得对不对。所以再叠上这一镜自己落定过几次（run store 的
   * `settledBy`），只换刚跑完的那一张。
   */
  const bustOf = (shotId) =>
    bust.value + (runStore.settledBy.get(shotId) ?? 0)

  return {
    shots, episodeDuration, loading, loadError, load, bust, bustOf, previewOf,
    inflightBy, pct, shotState, busy, shotRunning, isWaiting,
    start, starting, stop, shotAction, stepBtn, shotTone,
    running: computed(() => runStore.running),
  }
}
