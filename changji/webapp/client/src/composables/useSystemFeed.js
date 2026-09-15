/**
 * 引擎两秒一推的那份系统表。顶栏那三个小表（SysMeter）和「AI 作业中」
 * （JobBadge）读的是同一份——负载读数在前面几栏，此刻在跑和排队的那几件
 * 在 `jobs` 里。引擎那头也是同一个函数拼的（server.cpp：「**和推过去的
 * 那份一样**」）。
 *
 * **模块级一份，不是每个组件一份。** 理由同 useRefStream：两个组件各订一条
 * 的话，引擎两秒一次那份表要发两遍，而两处的连接管理还是逐字重复的两份
 * ——它们自己的注释就写着「两个组件是同一份写法，也就有同一个毛病」，
 * 上一次那个毛病（讣告串台、重连越积越多）确实是分别修了两遍。
 *
 * **连不上 WebSocket 时退回轮询。** 这是它和那两份老写法真正不同的地方：
 * 原来这两块是**纯 socket**，代理把 Upgrade 掐了就是永远空白——顶栏三个
 * 小表整个不出现，「AI 作业中」也一次都不出现。而后者是**从设定页点完
 * 「批量补分镜」之后唯一看得见的出口**（那一页自己的提示就写着去那儿看
 * 进度）。`/api/system` 回的就是同一份 body，拉回来照样能画。
 *
 * 只在 socket 没有的时候拉：连上之后这条就停，不为同一个节奏做两遍功。
 */
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import { api } from '@/api'
import { openJobSocket } from '@/composables/useJobSocket'

/** 最近那一条。null = **现在没数据**，不是"零负载"。 */
const stat = ref(null)

/**
 * 多久没收到就当它死了。
 *
 * **不能只靠 onclose。** 2026-09-11 用户报「GPU 一直 0、显存一直 22.1，
 * 有任务也不动」——服务重启（或者隧道断一下）之后，浏览器这头常常收不到
 * FIN，socket 在 readyState 上还是 OPEN，`onclose` 一辈子不触发。于是那几个
 * 数冻在最后一条消息上，而"一个不动的数比没有更误导"。
 *
 * 服务端两秒一推，八秒还没动静就是不对了（丢三条）。留余量是因为标签页
 * 切到后台时浏览器会压计时器，压得太紧会来回重连。
 */
const STALE_MS = 8000
/** 没有 socket 时多久拉一次。跟推送同一个节奏。 */
const PULL_MS = 2000

let sock = null
let retry = null
let watchdog = null
let poller = null
let users = 0
let lastAt = 0
/**
 * 这一条连接是第几条。**断开回调要认自己那一条**。
 *
 * 看门狗发现八秒没动静 → `dead.close()` → 紧接着 `connect()` 连上新的，
 * 而 `close()` 的 onclose 是**异步**到的：它带的是**上一条**的讣告，却会
 * 把刚建好那条的状态一并清掉，还排一个五秒后的重连——五秒后又连一条，
 * 而刚才那条既没关也没人记着。每触发一次看门狗就多留一条。
 * 用一个递增的号认人：讣告上的号和当前的对不上，就是上一条的，不理。
 */
let gen = 0

async function pull() {
  try {
    const body = await api.system()
    lastAt = Date.now()
    stat.value = body
  } catch {
    // 拉不到也别留旧数：那几个数字会一动不动地说谎。
    stat.value = null
  }
}

function startPulling() {
  if (poller) return
  pull()
  poller = setInterval(pull, PULL_MS)
}

function stopPulling() {
  clearInterval(poller)
  poller = null
}

function connect() {
  if (!users) return
  clearTimeout(retry)
  retry = null
  const myGen = ++gen
  lastAt = Date.now()
  sock = openJobSocket(
    'system',
    (msg) => {
      if (msg.type !== 'system') return
      lastAt = Date.now()
      stat.value = msg
      // 推上来了就别再拉了
      stopPulling()
    },
    () => {
      if (myGen !== gen) return // 上一条的讣告，别动现在这条
      sock = null
      clearTimeout(retry)
      retry = setTimeout(connect, 5000)
      // **不清 stat，改成拉。** 拉得到的话这一块照常显示，而且是新数；
      // 拉不到才空掉（见 pull 的 catch）。
      startPulling()
    },
  )
}

/** 半开的连接自己不会说话，所以由这头来问。 */
function sweep() {
  if (!users || !sock) return
  if (Date.now() - lastAt < STALE_MS) return
  const dead = sock
  sock = null
  dead.close()
  startPulling()   // 重连那几秒里别空着
  connect()
}

/**
 * 订上这份表。组件卸载时自动退订，最后一个人走了就把 socket 收掉。
 *
 * @returns {{stat: import('vue').Ref<object|null>}}
 */
export function useSystemFeed() {
  onMounted(() => {
    users += 1
    if (users > 1) return
    connect()
    watchdog = setInterval(sweep, 3000)
  })
  onUnmounted(() => {
    users = Math.max(0, users - 1)
    if (users) return
    clearInterval(watchdog)
    watchdog = null
    clearTimeout(retry)
    retry = null
    stopPulling()
    gen += 1 // 之后到的讣告都不算数
    sock?.close()
    sock = null
    stat.value = null
  })
  return { stat }
}

/**
 * 长跑任务此刻在不在跑。`null` = 还不知道（表没回来、拉不到）。
 *
 * **给"跑完了重拉一次"那几条下降沿用的。** 它们原来盯的是
 * `useRun.running` / `useWriter.running`，而那两个 store **只有特定几页
 * 在驱动**：`useRun` 只有镜头格里的 `useShots` 在轮询，`useWriter` 只有
 * 故事页和设定页那一格。于是"跑完了"这件事恰恰在**数字摆在眼前的那几页**
 * 上一次都不会发生——项目库的卡片、这一集那排标签、设定页那行标签，
 * 三处都栽在同一条上。这份表在每一页上都两秒一拍地拉，没有这个问题。
 *
 * **默认只认 run / write 两种**（就是那两个作业槽的语义）。出参考图那种
 * 短活也在这份表里，一键出图一跑就是十几条，跟着它重拉等于把一整页的
 * 请求重发十几遍。
 *
 * @param {string[]} [kinds] 认哪几种。成片那一页只认 `run`：片子只有它出。
 */
export function useLongRunning(kinds = ['run', 'write']) {
  const { stat } = useSystemFeed()
  return computed(() => {
    const jobs = stat.value?.jobs
    if (!jobs) return null
    return jobs.some((j) => kinds.includes(j.kind))
  })
}

/**
 * 引擎回来之后，把那一页上"读砸了"的那一趟重来一次。
 *
 * **引擎重启是这套东西里最常见的一种断。** 仓库里三处注释都写着「引擎重启
 * 时会连着失败几次」，而那几处说的都是**轮询**那一层（它们自己会慢下来再
 * 接着试）。真正卡住的是**页面那一趟读**：进页面时引擎正好没起来，那一页
 * 就停在「读不到这一集的分镜 · 网络请求发不出去」上，而引擎两秒之后就回来了
 * ——顶栏那盏灯自己变回「引擎已连接」，系统表和「AI 作业中」也自己回来了，
 * 唯独那一页的正文一直是那句报错。人只能刷新，或者切一下集号 / 标签把它
 * 骗回来。实测过，那一屏在引擎回来之后又停了十几秒一动不动。
 *
 * 判据就用这份表在不在：它两秒一拍，拉不到时 `stat` 是 null（见 pull 的
 * catch），回来那一拍就非空了。
 *
 * **只在那一页真的挂着错的时候重来。** 不然每开一页都要白读一趟——
 * 第一次拿到表也是一次 null → 非空的跳变。
 *
 * @param {() => unknown} hasError 这一页现在是不是停在报错上
 * @param {() => void}    retry    重来那一趟
 */
export function useRetryWhenBack(hasError, retry) {
  const { stat } = useSystemFeed()
  watch(
    () => stat.value != null,
    (up) => {
      if (up && hasError()) retry()
    },
  )
}
