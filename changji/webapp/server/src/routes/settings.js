/**
 * 设置页要用的聚合接口。
 *
 * 设置分两层：引擎地址归 Node 管（不然连不上引擎就什么都改不了），
 * ComfyUI 地址、大模型地址、画质、闸门这些归引擎管。
 * 这里把两层拼成一份，界面上看到的就是一整页。
 */

import { Router } from 'express'
import { envLocked, loadConfig, saveConfig, configPath } from '../config.js'
import { callEngine, engineStatus, EngineError } from '../engine.js'

export const settingsRouter = Router()

settingsRouter.get('/status', async (_req, res) => {
  res.json(await engineStatus())
})

function nodeConfigView() {
  const cfg = loadConfig()
  return {
    engineBaseUrl: cfg.engineBaseUrl,
    engineTimeoutMs: cfg.engineTimeoutMs,
    configFile: configPath(),
    envLocked: envLocked(),
  }
}

settingsRouter.get('/config', (_req, res) => {
  res.json(nodeConfigView())
})

settingsRouter.post('/config', (req, res) => {
  const patch = {}
  if (typeof req.body?.engineBaseUrl === 'string') {
    const url = req.body.engineBaseUrl.trim().replace(/\/+$/, '')
    if (!/^https?:\/\//.test(url)) {
      res.status(400).json({ detail: '引擎地址要以 http:// 或 https:// 开头' })
      return
    }
    patch.engineBaseUrl = url
  }
  if (req.body?.engineTimeoutMs !== undefined) {
    const ms = Number(req.body.engineTimeoutMs)
    if (!Number.isFinite(ms) || ms < 1000) {
      res.status(400).json({ detail: '超时至少要 1000 毫秒' })
      return
    }
    patch.engineTimeoutMs = Math.round(ms)
  }
  if (!Object.keys(patch).length) {
    res.status(400).json({ detail: '没有要改的项' })
    return
  }
  saveConfig(patch)
  res.json(nodeConfigView())
})

/**
 * 设置页开页时要的全部数据，一次问完。
 *
 * 拆成四个请求的话，引擎离线时界面会连着弹四个错，
 * 而用户需要看到的只有一句「引擎连不上」。
 */
settingsRouter.get('/overview', async (_req, res) => {
  const status = await engineStatus()
  const out = {
    node: nodeConfigView(),
    engine: status,
    connections: null,
    settings: null,
    hardware: null,
    doctor: null,
    errors: {},
  }
  if (!status.online) {
    res.json(out)
    return
  }
  const jobs = [
    ['connections', '/api/connections'],
    ['settings', '/api/settings'],
    ['hardware', '/api/hardware'],
    ['doctor', '/api/doctor'],
  ]
  await Promise.all(
    jobs.map(async ([key, pathname]) => {
      try {
        out[key] = await callEngine(pathname, { timeoutMs: 30000 })
      } catch (err) {
        out.errors[key] = err instanceof EngineError ? err.message : String(err)
      }
    }),
  )
  res.json(out)
})
