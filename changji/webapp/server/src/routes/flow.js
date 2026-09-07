/**
 * 引导流程的进度判定。
 *
 * 八步走：项目 → 剧本大纲 → 角色 → 场景 → 分镜 → 制作 → 成片 → 上传。
 * 每一步是不是做完了，判定规则只写在这一处。写在前端的话，
 * 侧边栏、顶部进度条、下一步按钮三个地方各判一遍，迟早对不上。
 */

import { Router } from 'express'
import { loadConfig } from '../config.js'
import { callEngine } from '../engine.js'

export const flowRouter = Router()

export const STEPS = [
  { key: 'project', title: '项目', hint: '选一个项目，或者新建一个' },
  { key: 'script', title: '剧本大纲', hint: '给一句梗概，让大模型写出整集剧本' },
  { key: 'characters', title: '角色', hint: '定下每个角色的长相和音色' },
  { key: 'scenes', title: '场景', hint: '定下每个场景的空间和光线' },
  { key: 'storyboard', title: '分镜', hint: '把剧本拆成一个个镜头' },
  { key: 'production', title: '制作', hint: '配音、首帧、草稿、成片，跑流水线' },
  { key: 'film', title: '成片', hint: '看装配好的整集' },
  { key: 'publish', title: '上传至平台', hint: '带上标题和话题投递出去' },
]

/**
 * 一集在流水线上走到哪了。
 *
 * 判定看的是数据本身而不是「用户点没点过下一步」。中途关掉浏览器
 * 第二天回来，进度得还在。
 */
function assessEpisode(project, shots, episode, publishedIds) {
  const done = {}
  done.project = Boolean(project?.project_id)
  done.script = Boolean(episode && String(episode.synopsis || '').trim())
  done.characters = (project?.characters?.length ?? 0) > 0
  done.scenes = (project?.locations?.length ?? 0) > 0
  done.storyboard = shots.length > 0

  const finalStates = new Set(['final_done', 'locked', 'fallback'])
  const producedShots = shots.filter((s) => finalStates.has(s.status)).length
  done.production = shots.length > 0 && producedShots === shots.length
  done.film = false // 由调用方按 outputs 回填
  done.publish = Boolean(episode && publishedIds.has(episode.episode_id))

  return {
    done,
    counters: {
      shots: shots.length,
      produced: producedShots,
      characters: project?.characters?.length ?? 0,
      locations: project?.locations?.length ?? 0,
      lipsync: shots.filter((s) => s.needs_lipsync).length,
      plannedDurationS: shots.reduce((a, s) => a + (s.duration_s || 0), 0),
    },
  }
}

flowRouter.get('/', async (req, res) => {
  const projectPath = req.query.path
  if (!projectPath) {
    res.json({
      steps: STEPS,
      done: Object.fromEntries(STEPS.map((s) => [s.key, false])),
      counters: {},
      project: null,
      episode: null,
    })
    return
  }

  const search = '?path=' + encodeURIComponent(projectPath)
  let project
  try {
    project = await callEngine('/api/project', { search, timeoutMs: 20000 })
  } catch (err) {
    res.status(err.status ?? 502).json({ detail: err.message })
    return
  }

  const episodeId = req.query.episode_id || project.episodes?.[0]?.episode_id || ''
  const episode = project.episodes?.find((e) => e.episode_id === episodeId) ?? null

  let shots = []
  if (episodeId) {
    try {
      const data = await callEngine('/api/shots', {
        search: search + '&episode_id=' + encodeURIComponent(episodeId),
        timeoutMs: 20000,
      })
      shots = data.shots ?? []
    } catch {
      // 分镜还没出的时候引擎会 404，这不是错误，是流程还没走到
      shots = []
    }
  }

  let outputs = []
  try {
    const data = await callEngine('/api/outputs', { search, timeoutMs: 15000 })
    outputs = data.files ?? []
  } catch {
    outputs = []
  }

  const cfg = loadConfig()
  const publishedIds = new Set(
    (cfg.publishRecords ?? [])
      .filter((r) => r.project === projectPath && r.status === 'done')
      .map((r) => r.episodeId),
  )

  const assessed = assessEpisode(project, shots, episode, publishedIds)
  // 成片文件名里带集号。没挑集时只要出过片就算走到这一步了。
  assessed.done.film = outputs.some(
    (o) => !episodeId || String(o.name || '').includes(episodeId),
  )

  res.json({
    steps: STEPS,
    done: assessed.done,
    counters: { ...assessed.counters, outputs: outputs.length },
    project,
    episodeId,
    episode,
    outputs,
  })
})
