/**
 * 一件后台活的消息怎么送到这一页来：**socket 优先，连不上就轮询信箱。**
 *
 * 引擎那边一件活的消息（`job_progress` / `job_thinking` / `job_done` /
 * `job_error`，还有各步自己的 `outline_progress`、`story_token`）都播在
 * 一条随机流号上，正常走 WebSocket。
 *
 * **连不上是真实存在的**：公司代理把 Upgrade 掐了、页面刚打开还没握完手。
 * 原来这时候的退路是"不带 async 发过去，让 HTTP 一直等到干完"——拿得到
 * 结果，但那几分钟里**进度、思考、停下三样一起没有**：
 *
 *     进度   job_progress 播出去没人听
 *     思考   同上，而这恰恰是最需要看见的那一步（十几分钟不出一个字）
 *     停下   顶栏那块徽标是 thinking.start 才出现的，没有它就没有按钮；
 *            而且同步那条路上引擎压根不挂 JobScope，按了也找不到这件活
 *
 * 三样一起没有，所以单补一个取消没用：没有按钮可按。
 *
 * 现在退到第二条路：先开个信箱（`POST /api/job/watch`），照样带 async 把
 * 请求发出去，然后一秒不到拉一次 `GET /api/job/events` 把同一批消息取回来。
 * 消化那一下（`consume`）两条路共用同一份，所以界面看到的东西是一样的——
 * 唯一少的是采样预览图，信箱不存它（一张几十 KB，见 job_stream.hpp）。
 *
 * **调用方的形状不变**：不管哪条路，都是"消息进 consume，结果自己等那个
 * promise"。`mode` 只在一处有用——它是假的时候不能带 async 发请求，
 * 因为那时候结果真的没地方送回来。
 */
import { api } from '@/api'
import { openJobSocket } from '@/composables/useJobSocket'

/** 轮询隔多久拉一次。思考是一段段推的，隔太久就不像"在想"了。 */
const POLL_MS = 800

/** 等 socket 握手最多等多久。本机的话几毫秒就完了。 */
const OPEN_MS = 2000

/** 收尾那条消息。看见它就别再拉了——信箱那边已经销号。 */
const TERMINAL = new Set(['job_done', 'job_error'])

/** 断了时的默认说法。 */
const LOST = '和引擎的连接断了，这件事干没干完不好说'

/**
 * 接上这条 stream。
 *
 * @param {string} streamId 流号，和请求体里那个 `stream` 是同一串字
 * @param {(msg: object) => void} consume 来了一条消息。**已经过滤过 job_id**
 * @param {(message: string) => void} [onLost]
 *        这条路断了、而结果还没到。调用方一定要在这儿把等的人放出来，
 *        否则那一格会永远显示"干着…"，而那比报个错难受得多。
 * @param {object} [opts]
 * @param {string} [opts.lost]
 *        断了时那句话。各页说的不一样（"这份大纲写没写完不好说" /
 *        "这一段改没改完不好说"），而人看见的就是这句，值得让调用方自己定。
 * @returns {Promise<{mode:'socket'|'poll'|'none', close:()=>void}>}
 */
export async function openJobFeed(streamId, consume, onLost, { lost = LOST } = {}) {
  let sock = null
  let opened = false
  const take = (msg) => {
    // 同一条连接上别的活也会推消息过来，认准自己那个。
    if (msg?.job_id !== streamId) return
    consume(msg)
  }

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      take,
      () => {
        // 还没开起来就断的那种（构造函数直接抛、代理当场拒），这一声
        // 在下面会被 `opened` 挡掉——那时候要走的是轮询，不是收场。
        if (opened) onLost?.(lost)
        resolve()
      },
      () => {
        opened = true
        resolve()
      },
    )
    setTimeout(resolve, OPEN_MS)
  })

  if (opened) {
    return { mode: 'socket', close: () => sock?.close() }
  }

  // **先把这条 socket 彻底断掉。** 它迟一点可能才连上、或者才报错，而从
  // 这儿起结果只认轮询；留着它的话那一声会把结论带偏。close() 顺手把
  // onclose/onerror 摘了，后面不会再叫。
  sock?.close()
  sock = null

  let watching = false
  try {
    watching = !!(await api.watchJob(streamId))?.watching
  } catch {
    watching = false
  }
  // 连信箱都开不出来（引擎太老、网断了一瞬）：没有第二条路了，
  // 调用方会退回真同步——慢，但拿得到结果。
  if (!watching) return { mode: 'none', close: () => {} }

  let closed = false
  const loop = async () => {
    let since = 0
    while (!closed) {
      let r = null
      try {
        r = await api.jobEvents(streamId, since)
      } catch (e) {
        if (!closed) onLost?.(e?.message || lost)
        return
      }
      if (closed) return
      // 信箱没了：没开过，或者太久没人来取被扫掉了。**别傻等。**
      if (!r || r.exists === false) {
        onLost?.(lost)
        return
      }
      since = r.next ?? since
      let over = false
      for (const msg of r.events ?? []) {
        take(msg)
        if (TERMINAL.has(msg?.type)) over = true
      }
      if (over) return
      // 引擎说完了、可我们没看见收尾那条：别再转圈了。
      if (r.done) {
        onLost?.('这件事的结果没收到')
        return
      }
      await new Promise((r2) => setTimeout(r2, POLL_MS))
    }
  }
  // **不 await。** 这一趟要跑到活干完为止，而调用方现在就得去发那个请求。
  loop()
  return {
    mode: 'poll',
    close: () => {
      closed = true
    },
  }
}
