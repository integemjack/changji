/**
 * 发一个"当场回 202、结果从 WebSocket 回来"的请求，等到它真干完。
 *
 * **为什么不直接 await 那个 HTTP 请求。** 引擎那边一条 I/O 线程管着一批
 * 连接，请求在它上面占多久，落在同一条线程上的连接就干等多久——写一章
 * 一两分钟，出一张参考图几十秒。实测那期间别的请求会卡满二十多秒，而
 * 顶栏那块表更惨：它是长连接，认准了一条线程，一旦落在被占住那条上，
 * 会冻到这件活干完。
 *
 * 所以接口只回一句"开始了"，结果走那条通道：
 *
 *     job_progress   干到哪儿了
 *     job_thinking   模型"先想再写"的那一段（**每一步都有**）
 *     job_preview    采样到一半的那张小图（出图才有）
 *     job_done       干完了，result 就是同步那条会回的那份 body
 *     job_error      砸了
 *
 * **socket 连不上的时候**（代理把 Upgrade 掐了、页面刚打开还没握完手）
 * 退到轮询信箱那条，同一批消息改从 HTTP 取——进度、思考、顶栏那个「停下」
 * 都还在。为什么非补不可、以及原来那条退路少了什么，见 useJobFeed。
 *
 * **真正的同步只剩最后一层兜底**：连信箱都开不出来才不带 async 发过去。
 * 慢，但拿得到结果——没有回传通道的话异步那条根本没地方把结果送回来。
 */
import { openJobFeed } from '@/composables/useJobFeed'
import { useThinking } from '@/stores/thinking'

/**
 * @param {(extra: object) => Promise<any>} send
 *        真正发请求的那下。`extra` 是要并进请求体的 `{stream, async}`；
 *        两条路都走不通时它是 `{}`。
 * @param {object}   [opts]
 * @param {string}   [opts.prefix='job']  stream id 的前缀，只为了日志好认
 * @param {(cur:number,total:number,msg:string)=>void} [opts.onProgress]
 * @param {string}   [opts.label]
 *        给人看的活名（"写大纲"这种），显示在思考浮层里。不给就只显示内容。
 * @param {(dataUrl:string, step:number)=>void} [opts.onPreview]
 *        采样到一半那张小图。**一张几十 KB，别存**——下一步马上又有一张，
 *        攒起来只会把这条通道变成主要流量。⚠️ 轮询那条路上没有它
 *        （信箱不留底），进度条照旧走。
 * @returns {Promise<any|null>} 干完的那份结果；砸了或者断了回 null
 * @throws 把 send 抛出来的东西原样抛出去（400/404 这些还是同步回的）
 */
export async function runAsyncJob(
  send,
  { prefix = 'job', label = '', onProgress, onPreview } = {},
) {
  const streamId = prefix + '-' + Math.random().toString(36).slice(2, 10)
  // **思考流统一在这儿接，不让每个视图各接一遍。** 会思考的步骤有十几个，
  // 散在五六个视图里；漏掉的那几个的表现是"这一步没有思考显示"，不报错。
  const thinking = useThinking()

  let settle = null
  const finished = new Promise((r) => {
    settle = r
  })

  const feed = await openJobFeed(
    streamId,
    (msg) => {
      if (msg.type === 'job_thinking') {
        thinking.push(streamId, msg.text ?? '')
      } else if (msg.type === 'job_progress') {
        onProgress?.(msg.current ?? 0, msg.total ?? 0, msg.message ?? '')
      } else if (msg.type === 'job_preview') {
        onPreview?.(msg.image ?? '', msg.current ?? 0)
      } else if (msg.type === 'job_done') {
        settle({ ok: true, result: msg.result })
      } else if (msg.type === 'job_error') {
        settle({ ok: false, message: msg.message })
      }
    },
    // 这条路断了。**一定要把等的人放出来**，否则这一格会永远显示"画着…"，
    // 而那比报个错难受得多。
    (message) => settle({ ok: false, message }),
  )
  const live = feed.mode !== 'none'

  if (live) thinking.start(streamId, label)
  try {
    const started = await send(live ? { stream: streamId, async: true } : {})
    // 同步兜底那条（两条回传路都没有）直接就是结果了。
    if (!started || !started.started) return started
    const fin = await finished
    if (!fin.ok) throw new Error(fin.message || '这件事没干成')
    return fin.result
  } finally {
    // **成了、砸了、抛了都要清。** 漏一条的话顶栏会永远显示"正在思考"，
    // 而那比不显示难受得多。
    thinking.finish(streamId)
    feed.close()
  }
}
