/**
 * WebSocket 中转层的测试。
 *
 * 这一层错了不会报错，只会"进度条不动"——而那和引擎慢、任务卡住
 * 在界面上是同一个样子，查起来要从头翻。
 */

import { describe, expect, it, vi } from 'vitest'

import { WS_PATH, engineWsUrl } from './ws.js'

describe('engineWsUrl', () => {
  it('http 换成 ws，并接上引擎的 /ws', () => {
    expect(engineWsUrl('http://127.0.0.1:8080')).toBe('ws://127.0.0.1:8080/ws')
  })

  it('https 换成 wss', () => {
    expect(engineWsUrl('https://engine.example.com')).toBe(
      'wss://engine.example.com/ws',
    )
  })

  it('结尾的斜杠要去掉，不然拼出来是 //ws', () => {
    // 引擎地址是用户在设置页手填的，带不带斜杠都有人写。
    expect(engineWsUrl('http://127.0.0.1:8080/')).toBe('ws://127.0.0.1:8080/ws')
    expect(engineWsUrl('http://127.0.0.1:8080///')).toBe(
      'ws://127.0.0.1:8080/ws',
    )
  })

  it('没配地址或者协议不认识时返回 null，由调用方关掉连接', () => {
    // 返回一个拼错的地址比返回 null 糟：前端会连上去然后一直等，
    // 而 null 会让这一层立刻关掉，前端马上退回轮询。
    expect(engineWsUrl('')).toBeNull()
    expect(engineWsUrl(null)).toBeNull()
    expect(engineWsUrl(undefined)).toBeNull()
    expect(engineWsUrl('127.0.0.1:8080')).toBeNull()
    expect(engineWsUrl('ftp://x')).toBeNull()
  })

  it('路径和 /api/* 同源，前端不用管跨域', () => {
    expect(WS_PATH).toBe('/api/ws')
    expect(WS_PATH.startsWith('/api/')).toBe(true)
  })
})

/**
 * 中转本身：浏览器 → 这一层 → 引擎，两个方向都要通。
 *
 * 起一个假引擎（真的 WebSocket 服务端）和一个真的 http 服务，
 * 把中转挂上去，然后用真的 WebSocket 客户端连过来。
 * 纯函数测不到的正是这一段——**消息有没有真的搬过去**。
 */
describe('中转', () => {
  it('订阅上行、消息下行，两个方向都通', async () => {
    const { createServer } = await import('node:http')
    const { WebSocketServer, WebSocket } = await import('ws')
    const { attachWs } = await import('./ws.js')

    // 假引擎：记下收到的上行，然后推一条进度回去
    const engine = new WebSocketServer({ port: 0 })
    const enginePort = await new Promise((r) =>
      engine.on('listening', () => r(engine.address().port)))
    const uplink = []
    engine.on('connection', (sock) => {
      sock.send(JSON.stringify({ type: 'hello', service: 'changji' }))
      sock.on('message', (d) => {
        uplink.push(d.toString())
        sock.send(JSON.stringify({
          type: 'progress', job_id: 'run-1', stage: 'audio',
          step: 2, total: 5, message: '第二句',
        }))
      })
    })

    process.env.CHANGJI_ENGINE_URL = `http://127.0.0.1:${enginePort}`
    const http = createServer()
    attachWs(http)
    const port = await new Promise((r) =>
      http.listen(0, '127.0.0.1', () => r(http.address().port)))

    const got = []
    const client = new WebSocket(`ws://127.0.0.1:${port}${WS_PATH}`)
    await new Promise((r) => client.on('open', r))
    client.on('message', (d) => got.push(JSON.parse(d.toString())))
    client.send(JSON.stringify({ type: 'subscribe', job_id: 'run' }))

    // 等到进度那条到了为止
    await vi.waitFor(() => {
      expect(got.some((m) => m.type === 'progress')).toBe(true)
    }, { timeout: 3000 })

    // 上行真的到了引擎，而且**一个字没改**
    expect(uplink).toEqual([JSON.stringify({ type: 'subscribe', job_id: 'run' })])
    // 下行也一样，包括连上时那条问候
    expect(got[0]).toEqual({ type: 'hello', service: 'changji' })
    const prog = got.find((m) => m.type === 'progress')
    expect(prog.step).toBe(2)
    expect(prog.job_id).toBe('run-1')

    client.close()
    await new Promise((r) => http.close(r))
    await new Promise((r) => engine.close(r))
    delete process.env.CHANGJI_ENGINE_URL
  })

  it('引擎断了要把浏览器那条也关掉——前端靠这个退回轮询', async () => {
    const { createServer } = await import('node:http')
    const { WebSocketServer, WebSocket } = await import('ws')
    const { attachWs } = await import('./ws.js')

    const engine = new WebSocketServer({ port: 0 })
    const enginePort = await new Promise((r) =>
      engine.on('listening', () => r(engine.address().port)))
    engine.on('connection', (sock) => setTimeout(() => sock.close(), 20))

    process.env.CHANGJI_ENGINE_URL = `http://127.0.0.1:${enginePort}`
    const http = createServer()
    attachWs(http)
    const port = await new Promise((r) =>
      http.listen(0, '127.0.0.1', () => r(http.address().port)))

    const client = new WebSocket(`ws://127.0.0.1:${port}${WS_PATH}`)
    await new Promise((r) => client.on('open', r))
    // 不关的话前端会一直等一个永远不来的消息，界面停在"生成中"
    await new Promise((r) => client.on('close', r))

    await new Promise((r) => http.close(r))
    await new Promise((r) => engine.close(r))
    delete process.env.CHANGJI_ENGINE_URL
  }, 10000)
})
