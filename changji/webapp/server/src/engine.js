/**
 * Python 引擎的客户端。
 *
 * 真正的流水线在 Python 那边跑，Node 这层只做转发和聚合。
 * 不在 Node 里重写一遍分镜逻辑，两套实现迟早会漂。
 */

import { Readable } from 'node:stream'
import { pipeline } from 'node:stream/promises'
import { loadConfig } from './config.js'

export class EngineError extends Error {
  constructor(message, status = 502) {
    super(message)
    this.status = status
  }
}

function engineUrl(pathname, search = '') {
  const { engineBaseUrl } = loadConfig()
  if (!engineBaseUrl) throw new EngineError('还没设置引擎地址，去设置页填一下', 503)
  return engineBaseUrl + pathname + (search || '')
}

/**
 * 把引擎的错误 body 翻成一句人话。
 *
 * **422 的 detail 是一个结构化数组**（对齐 FastAPI 的 pydantic 报错，
 * 引擎那边 readonly.hpp 里写着这一点），形如：
 *
 *     [{ loc: ['body', 'patch.tts_backend'], msg: 'Extra inputs are not permitted' }]
 *
 * 原来这里是 `data?.detail || ...` 直接扔给 `new EngineError(...)`，
 * 而 `new Error([{...}])` 的 message 会被字符串化成 **"[object Object]"**——
 * 用户在界面上看到的就是这七个字，等于什么都没说。
 * 而表单填错是最常撞到的一类错误，这条路恰恰最需要说清楚。
 *
 * 多条要全带上：一次提交填错三个字段，只报第一个的话，
 * 用户改完再提交又被打回来。
 */
function engineMessage(data, status) {
  const detail = data?.detail
  if (Array.isArray(detail)) {
    const lines = detail.map((item) => {
      if (typeof item === 'string') return item
      // loc 的第一段一般是 "body" / "query"，对用户没意义，去掉。
      const loc = Array.isArray(item?.loc)
        ? item.loc.filter((x) => x !== 'body' && x !== 'query').join('.')
        : ''
      const msg = item?.msg || item?.message || JSON.stringify(item)
      return loc ? `${loc}：${msg}` : msg
    })
    if (lines.length) return lines.join('\n')
  }
  if (typeof detail === 'string' && detail) return detail
  if (detail) return JSON.stringify(detail)
  if (typeof data?.message === 'string' && data.message) return data.message
  return `引擎报错 ${status}`
}

/** 发一个 JSON 请求，返回解析后的结果。 */
export async function callEngine(pathname, { method = 'GET', body, search, timeoutMs } = {}) {
  const cfg = loadConfig()
  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), timeoutMs ?? cfg.engineTimeoutMs)
  let res
  try {
    res = await fetch(engineUrl(pathname, search), {
      method,
      headers: body === undefined ? {} : { 'content-type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: controller.signal,
    })
  } catch (err) {
    if (err.name === 'AbortError') {
      throw new EngineError('引擎响应超时。这一步可能还在跑，稍后再看进度', 504)
    }
    throw new EngineError(`连不上引擎 ${cfg.engineBaseUrl}：${err.message}`, 503)
  } finally {
    clearTimeout(timer)
  }
  const text = await res.text()
  let data
  try {
    data = text ? JSON.parse(text) : {}
  } catch {
    throw new EngineError(`引擎返回的不是 JSON：${text.slice(0, 200)}`, 502)
  }
  if (!res.ok) {
    throw new EngineError(engineMessage(data, res.status), res.status)
  }
  return data
}

/**
 * 原样转发一个请求。
 *
 * 媒体文件走这条路，所以必须是流式的：一集成片几百兆，
 * 全读进内存再吐出去，Node 这边会先崩。
 */
export async function proxy(req, res) {
  const cfg = loadConfig()
  // originalUrl 才带着挂载前缀。用 req.path 的话 app.use('/api', …)
  // 会把 /api 吃掉，转出去就变成了 /projects，引擎一律 404。
  const at = req.originalUrl.indexOf('?')
  const pathname = at >= 0 ? req.originalUrl.slice(0, at) : req.originalUrl
  const qs = at >= 0 ? req.originalUrl.slice(at) : ''
  const target = engineUrl(pathname, qs)

  // 浏览器中途放弃这次请求时（拖进度条、缩略图还没加载完就滚走了），
  // 要把上游那一路也掐掉，否则引擎那边会继续吐一整个视频给没人要的连接。
  const abort = new AbortController()
  const onClientGone = () => abort.abort()
  req.on('aborted', onClientGone)
  res.on('close', () => {
    if (!res.writableEnded) abort.abort()
  })

  const init = { method: req.method, headers: {}, redirect: 'manual', signal: abort.signal }
  // multipart 上传（参考图）原样透传，不能在这里 JSON 化
  const ct = req.headers['content-type']
  if (ct) init.headers['content-type'] = ct
  // Range 必须转过去。不转的话引擎每次都回整个文件，浏览器认定这个源
  // 不支持随机访问，成片页拖进度条和按分镜跳转就全都跳回 0 秒。
  for (const name of ['range', 'if-range', 'if-none-match', 'if-modified-since']) {
    const value = req.headers[name]
    if (value) init.headers[name] = value
  }
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    init.body = req.rawBody ?? req
    init.duplex = 'half'
  }

  let upstream
  try {
    upstream = await fetch(target, init)
  } catch (err) {
    if (abort.signal.aborted) return // 浏览器自己不要了，没人在等这个响应
    res.status(503).json({
      detail: `连不上引擎 ${cfg.engineBaseUrl}：${err.message}`,
      engineOffline: true,
    })
    return
  }

  res.status(upstream.status)
  const encoded = Boolean(upstream.headers.get('content-encoding'))
  for (const [key, value] of upstream.headers) {
    // fetch 已经把压缩解开了，上游的 content-encoding 和 content-length
    // 跟实际发出去的对不上，照抄会让浏览器把响应判成损坏
    if (['content-encoding', 'transfer-encoding', 'connection'].includes(key)) continue
    // 没压缩时长度是准的，留着——视频要靠它算进度条
    if (key === 'content-length' && encoded) continue
    res.setHeader(key, value)
  }
  if (!upstream.body) {
    res.end()
    return
  }

  // 必须用 pipeline 而不是 pipe。
  //
  // pipe 不管上游报错：连接中途断了（引擎重启、浏览器拖进度条放弃了这次
  // 请求）会在这个 Readable 上抛一个没人接的 'error' 事件，Node 的默认行为
  // 是让整个进程崩掉——一次拖进度条就能把整个 Web 服务干掉。
  // 这条在本机实测发生过：读了 485KB 视频之后 other side closed，进程退出。
  try {
    await pipeline(Readable.fromWeb(upstream.body), res)
  } catch (err) {
    // 断流是常态不是故障：视频拖进度条、缩略图还没加载完就滚走了，
    // 都会走到这儿。记一行就够，不要惊动上层。
    if (!abort.signal.aborted && !res.writableEnded) {
      console.warn(`转发中断 ${pathname}：${err.message}`)
    }
  } finally {
    req.off('aborted', onClientGone)
  }
}

/** 引擎在不在。设置页和顶栏的状态灯用这个。 */
export async function engineStatus() {
  const cfg = loadConfig()
  const started = Date.now()
  try {
    const data = await callEngine('/api/health', { timeoutMs: 4000 })
    return {
      online: true,
      baseUrl: cfg.engineBaseUrl,
      latencyMs: Date.now() - started,
      service: data.service ?? 'changji',
    }
  } catch (err) {
    return {
      online: false,
      baseUrl: cfg.engineBaseUrl,
      latencyMs: Date.now() - started,
      error: err.message,
    }
  }
}
