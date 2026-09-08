/**
 * 把引擎的 WebSocket 中转给浏览器。
 *
 * 为什么要中转，而不是让浏览器直连引擎：**引擎地址是可配的**
 * （`engineBaseUrl`，默认 127.0.0.1:8080，设置页能改，也能用环境变量指到
 * 另一台机器）。让浏览器直连就得把这个地址发到前端去，而前端拿到的
 * 地址一旦填错，用户连改回来的界面都打不开——这正是当初加这一层的理由，
 * 见 index.js 开头那段。
 *
 * 中转只做搬运，不解释消息内容。引擎推什么，浏览器收到什么。
 *
 * ⚠️ **按类别订阅，不是按 job_id。** 引擎那边的 job_id 是随机生成的，
 * 而且**不从任何接口暴露出去**（`POST /api/run` 回 {started, queue}，
 * `GET /api/run` 那十几个字段里也没有）。所以订阅只能用任务类别
 * （"run" / "write"），引擎侧 `Hub::subscribe` 认这两种。
 * 这一点是 2026-09-08 做端到端验证时才发现的：在那之前
 * `/ws` 一条任务消息都送不出去，因为没有客户端拿得到 job_id。
 */

import { WebSocketServer, WebSocket } from 'ws'

import { loadConfig } from './config.js'

/** 浏览器连过来的路径。和 /api/* 一样走同源，不用管跨域。 */
export const WS_PATH = '/api/ws'

/**
 * 把 http(s):// 换成 ws(s)://，并接上引擎的 /ws。
 *
 * 引擎地址结尾可能带斜杠（用户手填的），不去掉的话拼出来是 //ws。
 */
export function engineWsUrl(baseUrl) {
  if (!baseUrl) return null
  const trimmed = baseUrl.replace(/\/+$/, '')
  if (trimmed.startsWith('https://')) return `wss://${trimmed.slice(8)}/ws`
  if (trimmed.startsWith('http://')) return `ws://${trimmed.slice(7)}/ws`
  return null
}

/**
 * 挂到已经在监听的 http 服务上。
 *
 * 用 noServer 模式手动处理 upgrade：`ws` 自己的 path 匹配会把**所有**
 * upgrade 都接管掉，而这个服务上还有 Vite 的 HMR（开发模式下）。
 */
export function attachWs(server) {
  const wss = new WebSocketServer({ noServer: true })

  server.on('upgrade', (req, socket, head) => {
    let pathname
    try {
      pathname = new URL(req.url, 'http://localhost').pathname
    } catch {
      socket.destroy()
      return
    }
    if (pathname !== WS_PATH) return // 别人的 upgrade，不碰
    wss.handleUpgrade(req, socket, head, (client) => {
      wss.emit('connection', client, req)
    })
  })

  wss.on('connection', (client) => {
    const target = engineWsUrl(loadConfig().engineBaseUrl)
    if (!target) {
      // 关掉而不是干等：前端看到关闭就退回轮询，那条路不依赖引擎地址
      // 对不对（它打的是 /api/run，由这一层代理，错了会给出人话）。
      client.close(1011, '还没设置引擎地址')
      return
    }

    let upstream
    try {
      upstream = new WebSocket(target)
    } catch {
      client.close(1011, '连不上引擎')
      return
    }

    // 引擎还没连上时浏览器可能已经发了订阅，先攒着。
    // 丢掉的话前端要么收不到消息，要么得自己写重发逻辑。
    const pending = []
    let upstreamOpen = false

    upstream.on('open', () => {
      upstreamOpen = true
      for (const m of pending) upstream.send(m)
      pending.length = 0
    })
    upstream.on('message', (data) => {
      if (client.readyState === WebSocket.OPEN) client.send(data.toString())
    })
    upstream.on('close', () => {
      if (client.readyState === WebSocket.OPEN) client.close(1011, '引擎断开')
    })
    upstream.on('error', () => {
      if (client.readyState === WebSocket.OPEN) client.close(1011, '连不上引擎')
    })

    client.on('message', (data) => {
      const text = data.toString()
      if (upstreamOpen && upstream.readyState === WebSocket.OPEN) {
        upstream.send(text)
      } else {
        pending.push(text)
      }
    })
    client.on('close', () => {
      // **浏览器走了要把上游一起关掉**，否则每刷新一次页面就在引擎那边
      // 留一条连接，而引擎的 Hub 按连接记订阅，泄漏的连接会一直收广播。
      if (upstream.readyState === WebSocket.OPEN ||
          upstream.readyState === WebSocket.CONNECTING) {
        upstream.close()
      }
    })
    client.on('error', () => {
      if (upstream.readyState === WebSocket.OPEN) upstream.close()
    })
  })

  return wss
}
