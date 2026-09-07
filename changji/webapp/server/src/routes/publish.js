/**
 * 上传至平台。
 *
 * 各家平台的开放接口都要资质和密钥，这里不假装能替用户登录。
 * 实际做的事是：把成片和一份填好的元数据（标题、简介、话题）投递到
 * 目标目录，或者 POST 给用户自己配的 webhook，由那边的脚本或第三方
 * 工具接手。这样在没有任何平台密钥的机器上也能跑通全流程。
 */

import fs from 'node:fs'
import path from 'node:path'
import { Router } from 'express'
import { loadConfig, saveConfig } from '../config.js'
import { callEngine } from '../engine.js'

export const publishRouter = Router()

// 各家的竖屏要求和时长上限。发布页拿它做发片前的自检，
// 免得投出去才被平台退回来。
const PLATFORMS = [
  { id: 'douyin', name: '抖音', ratio: '9:16', maxDurationS: 900 },
  { id: 'kuaishou', name: '快手', ratio: '9:16', maxDurationS: 600 },
  { id: 'xiaohongshu', name: '小红书', ratio: '9:16', maxDurationS: 900 },
  { id: 'shipinhao', name: '视频号', ratio: '9:16', maxDurationS: 1800 },
  { id: 'bilibili', name: '哔哩哔哩', ratio: '16:9', maxDurationS: 28800 },
  { id: 'youtube', name: 'YouTube', ratio: '9:16', maxDurationS: 3600 },
  { id: 'custom', name: '自定义', ratio: '', maxDurationS: 0 },
]

publishRouter.get('/platforms', (_req, res) => {
  res.json({ platforms: PLATFORMS })
})

publishRouter.get('/targets', (_req, res) => {
  const cfg = loadConfig()
  res.json({ targets: cfg.publishTargets ?? [] })
})

publishRouter.post('/targets', (req, res) => {
  const { id, name, platform, exportDir, webhookUrl, note } = req.body ?? {}
  if (!name || !platform) {
    res.status(400).json({ detail: '得填名字和平台' })
    return
  }
  if (!exportDir && !webhookUrl) {
    res.status(400).json({ detail: '投递目录和 webhook 至少要填一个' })
    return
  }
  if (webhookUrl && !/^https?:\/\//.test(webhookUrl)) {
    res.status(400).json({ detail: 'webhook 要以 http:// 或 https:// 开头' })
    return
  }
  const cfg = loadConfig()
  const targets = [...(cfg.publishTargets ?? [])]
  const target = {
    id: id || 't_' + Date.now().toString(36),
    name: String(name).slice(0, 60),
    platform,
    exportDir: exportDir || '',
    webhookUrl: webhookUrl || '',
    note: String(note || '').slice(0, 200),
  }
  const at = targets.findIndex((t) => t.id === target.id)
  if (at >= 0) targets[at] = target
  else targets.push(target)
  saveConfig({ publishTargets: targets })
  res.json({ target, targets })
})

publishRouter.delete('/targets/:id', (req, res) => {
  const cfg = loadConfig()
  const targets = (cfg.publishTargets ?? []).filter((t) => t.id !== req.params.id)
  saveConfig({ publishTargets: targets })
  res.json({ targets })
})

publishRouter.get('/records', (req, res) => {
  const cfg = loadConfig()
  const all = cfg.publishRecords ?? []
  const project = req.query.project
  res.json({ records: project ? all.filter((r) => r.project === project) : all })
})

/**
 * 投递一条成片。
 *
 * 先问引擎要项目根，再自己拼路径读文件。让前端传绝对路径过来是不行的，
 * 那等于把整块磁盘开给浏览器。
 */
publishRouter.post('/deliver', async (req, res) => {
  const { project, rel, targetId, title, description, tags, episodeId } = req.body ?? {}
  if (!project || !rel || !targetId) {
    res.status(400).json({ detail: '缺项目、成片或投递目标' })
    return
  }
  const cfg = loadConfig()
  const target = (cfg.publishTargets ?? []).find((t) => t.id === targetId)
  if (!target) {
    res.status(404).json({ detail: '没有这个投递目标' })
    return
  }

  let root
  try {
    const info = await callEngine('/api/project', {
      search: '?path=' + encodeURIComponent(project),
      timeoutMs: 15000,
    })
    root = path.resolve(info.root)
  } catch (err) {
    res.status(err.status ?? 502).json({ detail: err.message })
    return
  }

  const source = path.resolve(root, rel)
  if (source !== root && !source.startsWith(root + path.sep)) {
    res.status(403).json({ detail: '只能投递项目目录内的文件' })
    return
  }
  if (!fs.existsSync(source)) {
    res.status(404).json({ detail: '成片不在了：' + rel })
    return
  }

  const meta = {
    project,
    episodeId: episodeId || '',
    platform: target.platform,
    title: String(title || '').slice(0, 120),
    description: String(description || '').slice(0, 2000),
    tags: Array.isArray(tags) ? tags.slice(0, 20).map((t) => String(t).slice(0, 30)) : [],
    source: rel,
    bytes: fs.statSync(source).size,
    deliveredAt: new Date().toISOString(),
  }

  const record = {
    id: 'p_' + Date.now().toString(36),
    ...meta,
    targetId: target.id,
    targetName: target.name,
    status: 'pending',
    detail: '',
  }

  try {
    if (target.exportDir) {
      fs.mkdirSync(target.exportDir, { recursive: true })
      const stem = (meta.episodeId || 'final') + '_' + Date.now().toString(36)
      const dest = path.join(target.exportDir, stem + path.extname(source))
      fs.copyFileSync(source, dest)
      fs.writeFileSync(
        path.join(target.exportDir, stem + '.json'),
        JSON.stringify(meta, null, 2),
        'utf8',
      )
      record.exportedTo = dest
    }
    if (target.webhookUrl) {
      const hook = await fetch(target.webhookUrl, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(meta),
        signal: AbortSignal.timeout(20000),
      })
      record.webhookStatus = hook.status
      if (!hook.ok) throw new Error('webhook 返回 ' + hook.status)
    }
    record.status = 'done'
    record.detail = target.exportDir ? '已投递到 ' + record.exportedTo : 'webhook 已收下'
  } catch (err) {
    record.status = 'failed'
    record.detail = err.message
  }

  const records = [record, ...(cfg.publishRecords ?? [])].slice(0, 200)
  saveConfig({ publishRecords: records })
  res.status(record.status === 'failed' ? 502 : 200).json({ record })
})
