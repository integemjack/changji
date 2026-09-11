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

/**
 * 八步分两段。
 *
 * series 那三步做一次，整部剧共用；episode 那五步对着当前这一集走。
 * 前端的侧边导航靠 phase 分段，这里也用它来决定一步该按全剧判定
 * 还是按当前集判定。
 */
export const STEPS = [
  { key: 'project', phase: 'series', title: '项目', hint: '选一个项目，或者新建一个' },
  { key: 'story', phase: 'series', title: '故事', hint: '讲什么、分几章、按每集时长切成几集' },
  { key: 'assets', phase: 'series', title: '设定', hint: '给故事里的人和地方定妆，全剧共用一套' },
  // 分镜和制作 2026-09-10 合成一步「镜头」，见 flow.cpp 里同一处的注释
  { key: 'episode', phase: 'episode', title: '这一集', hint: '剧本、镜头、成片、发布，都在这一集上' },
]

/**
 * 每一步做完了没有。
 *
 * 判定看的是数据本身而不是「用户点没点过下一步」。中途关掉浏览器
 * 第二天回来，进度得还在。
 */
/**
 * 导出**只为了能测**：这套判定规则是只写在一处的（见文件头），
 * 而只写在一处的东西一旦错了，侧边栏、进度条、下一步按钮会一起错，
 * 三个地方看起来还一致，反而更难发现是判定本身的问题。
 */
export function assess(project, shots, episode, publishedIds) {
  const done = {}
  done.project = Boolean(project?.project_id)

  // 剧本大纲是全剧的事：有梗概，并且至少一集写出了内容。
  // 只看当前这一集的话，新建第五集时前四步的对勾会集体消失。
  const written = (project?.episodes ?? []).filter((e) => e.shots > 0 || e.synopsis)
  done.script =
    Boolean(String(project?.premise || '').trim()) && written.length > 0

  done.characters = (project?.characters?.length ?? 0) > 0

  // 场景库是全剧共用的，但这一步问的是「这一集够不够」：
  // 分镜还没出的时候只要库里有场景就算数；出了分镜之后，
  // 这一集引用到的场景必须都在库里，否则跑到一半会报「场景未注册」。
  const known = new Set((project?.locations ?? []).map((l) => l.location_id))
  // 老分镜常把场景 id 填在 scene_id 里、location_id 留空。那种镜头渲染时
  // 拿不到场景描述，界面上要能看出来，所以这里两个字段都认。
  const used = new Set()
  let unlinked = 0
  for (const s of shots) {
    if (s.location_id) {
      used.add(s.location_id)
    } else if (s.scene_id && known.has(s.scene_id)) {
      used.add(s.scene_id)
      unlinked += 1
    }
  }
  const missing = [...used].filter((id) => !known.has(id))
  done.scenes = known.size > 0 && missing.length === 0 && unlinked === 0

  // 判据取的是原来「制作」那条，不是「分镜」那条：这一格现在代表
  // "这一集的镜头做完了"，光有分镜表不算——那时候一帧画面都还没有。
  const finalStates = new Set(['final_done', 'locked', 'fallback'])
  const producedShots = shots.filter((s) => finalStates.has(s.status)).length
  done.shots = shots.length > 0 && producedShots === shots.length
  done.film = false // 由调用方按 outputs 回填
  done.publish = Boolean(episode && publishedIds.has(episode.episode_id))

  return {
    done,
    counters: {
      shots: shots.length,
      produced: producedShots,
      characters: project?.characters?.length ?? 0,
      locations: known.size,
      episodeLocations: used.size,
      missingLocations: missing,
      unlinkedShots: unlinked,
      episodesWritten: written.length,
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

  const assessed = assess(project, shots, episode, publishedIds)
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
