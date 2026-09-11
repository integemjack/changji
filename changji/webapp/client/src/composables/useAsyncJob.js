/**
 * 发一个"当场回 202、结果从 WebSocket 回来"的请求，等到它真干完。
 *
 * **为什么不直接 await 那个 HTTP 请求。** 引擎那边一条 I/O 线程管着一批
 * 连接，请求在它上面占多久，落在同一条线程上的连接就干等多久——写一章
 * 一两分钟，出一张参考图几十秒。实测那期间别的请求会卡满二十多秒，而
 * 顶栏那块表更惨：它是长连接，认准了一条线程，一旦落在被占住那条上，
 * 会冻到这件活干完。
 *
 * 所以接口只回一句"开始了"，结果走这条 socket：
 *
 *     job_progress   干到哪儿了
 *     job_preview    采样到一半的那张小图（出图才有）
 *     job_done       干完了，result 就是同步那条会回的那份 body
 *     job_error      砸了
 *
 * **退不回去的那条路要留着**：socket 连不上（代理掐了 WebSocket、页面刚
 * 打开还没连上）就不带 async，让 HTTP 一直等到干完。慢，但拿得到结果——
 * 没有 socket 的话异步那条根本没地方把结果送回来。
 */
import { openJobSocket } from '@/composables/useJobSocket'

/**
 * @param {(extra: object) => Promise<any>} send
 *        真正发请求的那下。`extra` 是要并进请求体的 `{stream, async}`；
 *        socket 没连上时它是 `{}`。
 * @param {object}   [opts]
 * @param {string}   [opts.prefix='job']  stream id 的前缀，只为了日志好认
 * @param {(cur:number,total:number,msg:string)=>void} [opts.onProgress]
 * @param {(dataUrl:string, step:number)=>void} [opts.onPreview]
 *        采样到一半那张小图。**一张几十 KB，别存**——下一步马上又有一张，
 *        攒起来只会把这条通道变成主要流量。
 * @returns {Promise<any|null>} 干完的那份结果；砸了或者断了回 null
 * @throws 把 send 抛出来的东西原样抛出去（400/404 这些还是同步回的）
 */
export async function runAsyncJob(send, { prefix = 'job', onProgress, onPreview } = {}) {
  const streamId = prefix + '-' + Math.random().toString(36).slice(2, 10)

  let settle = null
  const finished = new Promise((r) => {
    settle = r
  })

  let sock = null
  let opened = false

  await new Promise((resolve) => {
    sock = openJobSocket(
      streamId,
      (msg) => {
        if (msg.job_id !== streamId) return
        if (msg.type === 'job_progress') {
          onProgress?.(msg.current ?? 0, msg.total ?? 0, msg.message ?? '')
        } else if (msg.type === 'job_preview') {
          onPreview?.(msg.image ?? '', msg.current ?? 0)
        } else if (msg.type === 'job_done') {
          settle({ ok: true, result: msg.result })
        } else if (msg.type === 'job_error') {
          settle({ ok: false, message: msg.message })
        }
      },
      () => {
        // 连接没了。**一定要把等的人放出来**，否则这一格会永远显示"画着…"，
        // 而那比报个错难受得多。
        settle({ ok: false, message: '和引擎的连接断了，这件事干没干完不好说' })
        resolve()
      },
      () => {
        opened = true
        resolve()
      },
    )
    // 连不上也别卡在这儿：两秒之后就当没有 socket，走同步那条。
    setTimeout(resolve, 2000)
  })

  try {
    const started = await send(opened ? { stream: streamId, async: true } : {})
    // 同步那条（socket 没开）直接就是结果了。
    if (!started || !started.started) return started
    const fin = await finished
    if (!fin.ok) throw new Error(fin.message || '这件事没干成')
    return fin.result
  } finally {
    sock?.close()
  }
}
