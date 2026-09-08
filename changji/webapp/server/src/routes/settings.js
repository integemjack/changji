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

/**
 * 把设置页提交上来的东西整理成一个可以直接写盘的 patch。
 *
 * 返回 `{ patch }` 或者 `{ error }`。**单独抽出来是为了能测**——
 * 这几条规则原来长在路由处理函数里，而路由要起一个 express 才跑得起来。
 *
 * 只认这两个键：其余的都归引擎管，设置页透传过去改（见文件头）。
 * 传了别的键就当没传——不报错，因为前端会把整份配置回传，
 * 里面本来就带着引擎那一层的字段。
 */
export function planConfigPatch(body) {
  const patch = {}
  if (typeof body?.engineBaseUrl === 'string') {
    const url = body.engineBaseUrl.trim().replace(/\/+$/, '')
    if (!/^https?:\/\//.test(url)) {
      return { error: '引擎地址要以 http:// 或 https:// 开头' }
    }
    patch.engineBaseUrl = url
  }
  if (body?.engineTimeoutMs !== undefined) {
    const ms = Number(body.engineTimeoutMs)
    if (!Number.isFinite(ms) || ms < 1000) {
      return { error: '超时至少要 1000 毫秒' }
    }
    patch.engineTimeoutMs = Math.round(ms)
  }
  if (!Object.keys(patch).length) return { error: '没有要改的项' }
  return { patch }
}

settingsRouter.post('/config', (req, res) => {
  const { patch, error } = planConfigPatch(req.body)
  if (error) {
    res.status(400).json({ detail: error })
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
