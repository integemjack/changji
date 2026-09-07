/**
 * Python 引擎的客户端。
 *
 * 真正的流水线在 Python 那边跑，Node 这层只做转发和聚合。
 * 不在 Node 里重写一遍分镜逻辑，两套实现迟早会漂。
 */

import { Readable } from 'node:stream'
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
    throw new EngineError(data?.detail || data?.message || `引擎报错 ${res.status}`, res.status)
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
  const qs = req.originalUrl.includes('?')
    ? req.originalUrl.slice(req.originalUrl.indexOf('?'))
    : ''
  const target = engineUrl(req.path, qs)

  const init = { method: req.method, headers: {}, redirect: 'manual' }
  // multipart 上传（参考图）原样透传，不能在这里 JSON 化
  const ct = req.headers['content-type']
  if (ct) init.headers['content-type'] = ct
  if (req.method !== 'GET' && req.method !== 'HEAD') {
    init.body = req.rawBody ?? req
    init.duplex = 'half'
  }

  let upstream
  try {
    upstream = await fetch(target, init)
  } catch (err) {
    res.status(503).json({
      detail: `连不上引擎 ${cfg.engineBaseUrl}：${err.message}`,
      engineOffline: true,
    })
    return
  }

  res.status(upstream.status)
  for (const [key, value] of upstream.headers) {
    // 让 Node 自己算长度和编码，照抄上游的容易和实际发出的对不上
    if (['content-encoding', 'content-length', 'transfer-encoding', 'connection'].includes(key)) {
      continue
    }
    res.setHeader(key, value)
  }
  if (!upstream.body) {
    res.end()
    return
  }
  Readable.fromWeb(upstream.body).pipe(res)
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
