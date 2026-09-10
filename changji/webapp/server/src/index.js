/**
 * 场记 Web 平台的 Node 服务。
 *
 * 两件事：把打包好的前端发出去，把 /api/* 原样转给 Python 引擎。
 * 另外自己管两块引擎管不了的东西——引擎地址本身，和上传平台的投递配置。
 *
 * 为什么中间要加这一层：引擎地址填错了的时候，用户仍然需要一个能打开
 * 的界面去改它。前端直连引擎的话，填错地址就等于把自己锁在门外。
 */

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import express from 'express'

import { proxy } from './engine.js'
import { settingsRouter } from './routes/settings.js'
import { publishRouter } from './routes/publish.js'
import { flowRouter } from './routes/flow.js'
import { setupRouter } from './routes/setup.js'
import { attachWs } from './ws.js'

const here = path.dirname(fileURLToPath(import.meta.url))
const clientDist = path.resolve(here, '../../client/dist')

const app = express()
app.disable('x-powered-by')

// 只给自己的接口解析 JSON。/api/* 要原样转发，
// 在这里把 body 读掉的话，转发时就只剩一个空流。
app.use('/bff', express.json({ limit: '4mb' }))

app.get('/bff/health', (_req, res) => {
  res.json({ ok: true, service: 'changji-webapp' })
})
app.use('/bff/settings', settingsRouter)
app.use('/bff/publish', publishRouter)
app.use('/bff/flow', flowRouter)
// 首次运行那一页。**只是转发**，判断全在引擎那边——缺不缺模型是引擎
// 所在那台机器的事实，Node 这边一样都不知道。
app.use('/bff/setup', setupRouter)

app.use('/api', proxy)

if (fs.existsSync(clientDist)) {
  app.use(express.static(clientDist, { index: false, maxAge: '1h' }))
  // 前端是单页应用，刷新任意路由都要回到 index.html
  app.get('*', (req, res, next) => {
    if (req.path.startsWith('/api') || req.path.startsWith('/bff')) return next()
    res.sendFile(path.join(clientDist, 'index.html'))
  })
} else {
  app.get('/', (_req, res) => {
    res
      .status(503)
      .type('text/plain; charset=utf-8')
      .send('前端还没打包。开发时跑 npm run dev，部署时先跑 npm run build。')
  })
}

app.use((err, _req, res, _next) => {
  const status = err.status ?? 500
  res.status(status).json({ detail: err.message || '服务出错了' })
})

const port = Number(process.env.PORT || 5173 + 1)
const host = process.env.HOST || '0.0.0.0'
const server = app.listen(port, host, () => {
  console.log(`场记 Web 平台已启动：http://127.0.0.1:${port}`)
})

// 进度走 WebSocket。**掉线不影响功能**：前端收到关闭就退回轮询
// /api/run，那条路一直在。
attachWs(server)
