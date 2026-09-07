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

/** 问引擎要项目根。前端传绝对路径过来是不行的，那等于把整块磁盘开给浏览器。 */
async function projectRoot(project) {
  const info = await callEngine('/api/project', {
    search: '?path=' + encodeURIComponent(project),
    timeoutMs: 15000,
  })
  return { root: path.resolve(info.root), info }
}

/**
 * 投一条。
 *
 * 不抛异常：单条失败要作为一条记录留下来，而不是把整批打断。
 * 一次投八集时第三集失败就中断，前两集投了、后五集没投，
 * 而用户看到的只有一句报错——那种状态没法收拾。
 */
async function deliverOne({ target, root, project, rel, episodeId, title, description, tags }) {
  const record = {
    id: 'p_' + Math.random().toString(36).slice(2, 8) + Date.now().toString(36),
    project,
    episodeId: episodeId || '',
    platform: target.platform,
    title: String(title || '').slice(0, 120),
    description: String(description || '').slice(0, 2000),
    tags: Array.isArray(tags) ? tags.slice(0, 20).map((t) => String(t).slice(0, 30)) : [],
    source: rel,
    bytes: 0,
    deliveredAt: new Date().toISOString(),
    targetId: target.id,
    targetName: target.name,
    status: 'failed',
    detail: '',
  }

  const source = path.resolve(root, rel)
  if (source !== root && !source.startsWith(root + path.sep)) {
    record.detail = '只能投递项目目录内的文件'
    return record
  }
  if (!fs.existsSync(source)) {
    record.detail = '成片不在了：' + rel
    return record
  }
  record.bytes = fs.statSync(source).size

  // 投出去的元数据不带内部字段，接手的脚本读的是这一份
  const meta = {
    project: record.project,
    episodeId: record.episodeId,
    platform: record.platform,
    title: record.title,
    description: record.description,
    tags: record.tags,
    source: record.source,
    bytes: record.bytes,
    deliveredAt: record.deliveredAt,
  }

  try {
    if (target.exportDir) {
      fs.mkdirSync(target.exportDir, { recursive: true })
      const stem = (record.episodeId || 'final') + '_' + Date.now().toString(36)
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
  return record
}

function remember(records) {
  const cfg = loadConfig()
  saveConfig({ publishRecords: [...records, ...(cfg.publishRecords ?? [])].slice(0, 200) })
}

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
    ;({ root } = await projectRoot(project))
  } catch (err) {
    res.status(err.status ?? 502).json({ detail: err.message })
    return
  }

  const record = await deliverOne({
    target, root, project, rel, episodeId, title, description, tags,
  })
  remember([record])
  res.status(record.status === 'failed' ? 502 : 200).json({ record })
})

/**
 * 一次投好几集。
 *
 * 量产的后半段。整季跑完是八集八条片，一条一条填标题投出去，
 * 投到第五条人就开始出错——投重、漏投、标题串集。
 *
 * 标题和简介默认从每一集自己的数据来（集名和梗概），不用逐条打。
 * 想统一格式就给 titleTemplate，里面的 {title} {episode} {index} 会被替换。
 */
publishRouter.post('/batch', async (req, res) => {
  const { project, targetId, rels, titleTemplate, tags, description } = req.body ?? {}
  if (!project || !targetId || !Array.isArray(rels) || !rels.length) {
    res.status(400).json({ detail: '缺项目、投递目标或成片列表' })
    return
  }
  if (rels.length > 50) {
    res.status(400).json({ detail: '一次最多投 50 条' })
    return
  }
  const cfg = loadConfig()
  const target = (cfg.publishTargets ?? []).find((t) => t.id === targetId)
  if (!target) {
    res.status(404).json({ detail: '没有这个投递目标' })
    return
  }

  let root
  let info
  try {
    ;({ root, info } = await projectRoot(project))
  } catch (err) {
    res.status(err.status ?? 502).json({ detail: err.message })
    return
  }

  const episodes = info.episodes ?? []
  const records = []
  for (const [i, rel] of rels.entries()) {
    // 成片文件名里带集号，据此把这一条认回它属于哪一集
    const name = path.basename(String(rel))
    const ep = episodes.find((e) => name.includes(e.episode_id))
    const base = ep?.title || name.replace(/\.[^.]+$/, '')
    const title = titleTemplate
      ? String(titleTemplate)
          .replaceAll('{title}', base)
          .replaceAll('{episode}', ep?.episode_id ?? '')
          .replaceAll('{index}', String(i + 1))
          .slice(0, 120)
      : base

    // 一条一条来，不并发：投递目录多半在同一块盘上，
    // 八个大文件同时拷只会互相抢 IO，还更难说清哪一条卡住了
    // eslint-disable-next-line no-await-in-loop
    records.push(await deliverOne({
      target,
      root,
      project,
      rel,
      episodeId: ep?.episode_id ?? '',
      title,
      description: description ?? ep?.synopsis ?? '',
      tags,
    }))
  }

  remember(records)
  const failed = records.filter((r) => r.status === 'failed').length
  res.json({ records, delivered: records.length - failed, failed })
})
