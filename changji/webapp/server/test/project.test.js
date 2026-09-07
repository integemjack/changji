// 项目的读写与可移植性。从 Python 版 tests/test_project.py 搬过来。
//
// 可移植性是这套东西的一条硬承诺：整个目录拷到别的机器就能接着做。
// 下面几条测的都是能悄悄破坏这条承诺的写法——存了绝对路径、存了反斜杠、
// 引用了项目外的文件、写到一半断电留下半个 JSON。

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'

import {
  EpisodeSchema,
  ProjectStore,
  SCHEMA_VERSION,
  countsByStatus,
  episodeById,
  plannedDurationS,
  shotsNeeding,
  sortedShots,
} from '../src/engine/models/project.js'
import { ShotSchema, ShotStatus } from '../src/engine/models/shot.js'
import { copyDirSync } from '../src/engine/fsx.js'

let tmp

beforeEach(() => {
  tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'changji-test-'))
})
afterEach(() => {
  fs.rmSync(tmp, { recursive: true, force: true })
})

const newStore = (name = '剧') =>
  ProjectStore.create(path.join(tmp, name), 'drama', '测试剧')

describe('创建与读写', () => {
  it('创建目录结构', () => {
    const store = newStore()
    for (const sub of ['refs', 'audio', 'frames', 'shots/draft', 'shots/final',
      'subtitles', 'output', 'logs']) {
      expect(fs.existsSync(path.join(store.root, sub))).toBe(true)
    }
    expect(store.exists()).toBe(true)
  })

  it('重复创建会拒绝', () => {
    const store = newStore()
    expect(() => ProjectStore.create(store.root, 'drama')).toThrow(/已经是一个项目/)
  })

  it('存回读一致', () => {
    const store = newStore()
    const project = store.loadProject()
    project.premise = '深夜便利店'
    project.episodes.push(EpisodeSchema.parse({ episode_id: 'ep01', title: '第一集' }))
    store.saveProject(project)

    const again = new ProjectStore(store.root).loadProject()
    expect(again.premise).toBe('深夜便利店')
    expect(again.episodes[0].title).toBe('第一集')
  })

  it('打开非项目目录报错说清楚', () => {
    const empty = path.join(tmp, '空目录')
    fs.mkdirSync(empty)
    expect(() => new ProjectStore(empty).loadProject()).toThrow(/不是一个项目目录/)
  })

  it('未来版本的项目会被拒绝而不是读坏', () => {
    // 读坏比读不了糟得多：字段对不上时读出来的是一个残缺的项目，
    // 存回去就把新版本写的东西抹了
    const store = newStore()
    const raw = JSON.parse(fs.readFileSync(store.paths.projectFile, 'utf8'))
    raw.schema_version = SCHEMA_VERSION + 1
    fs.writeFileSync(store.paths.projectFile, JSON.stringify(raw), 'utf8')
    expect(() => store.loadProject()).toThrow(/更新版本/)
  })

  it('文件损坏时报出是哪个文件', () => {
    const store = newStore()
    fs.writeFileSync(store.paths.projectFile, '{ 这不是 json', 'utf8')
    expect(() => store.loadProject()).toThrow(/project\.json/)
  })

  it('原子写不留半个文件', () => {
    // 先写临时文件再改名。中途断电只会留下临时文件，
    // project.json 要么是旧的完整内容，要么是新的完整内容
    const store = newStore()
    const project = store.loadProject()
    store.saveProject(project)
    const leftovers = fs
      .readdirSync(store.root)
      .filter((f) => f.endsWith('.tmp'))
    expect(leftovers).toEqual([])
    expect(() => JSON.parse(fs.readFileSync(store.paths.projectFile, 'utf8'))).not.toThrow()
  })
})

describe('路径可移植性', () => {
  it('项目内路径存成相对且用正斜杠', () => {
    // 反斜杠存进去，这个项目拿到 Linux 上就读不出文件了
    const store = newStore()
    const target = path.join(store.root, 'refs', 'a.png')
    fs.writeFileSync(target, 'x')
    const rel = store.paths.rel(target)
    expect(rel).toBe('refs/a.png')
    expect(rel).not.toContain('\\')
    expect(path.isAbsolute(rel)).toBe(false)
  })

  it('项目外路径被拒绝', () => {
    const store = newStore()
    const outside = path.join(tmp, '别处.png')
    fs.writeFileSync(outside, 'x')
    expect(() => store.paths.rel(outside)).toThrow(/不在项目目录内/)
  })

  it('外部文件复制进项目后可用', () => {
    // 引用原位置的话，项目拷到别的机器就断链
    const store = newStore()
    const outside = path.join(tmp, '外面.png')
    fs.writeFileSync(outside, 'x')
    const rel = store.copyInto(outside, 'refs')
    expect(rel).toBe('refs/外面.png')
    expect(fs.existsSync(store.paths.abs(rel))).toBe(true)
  })

  it('相对路径能还原', () => {
    const store = newStore()
    expect(store.paths.abs('refs/a.png')).toBe(
      path.resolve(store.root, 'refs', 'a.png'),
    )
  })

  it('项目整体可搬迁', () => {
    // 换机器就是把目录拷走。拷完路径全变，但项目里存的都是相对路径，
    // 所以读出来的东西一字不差。
    const store = newStore()
    const project = store.loadProject()
    project.episodes.push(
      EpisodeSchema.parse({
        episode_id: 'ep01',
        shots: [
          ShotSchema.parse({
            shot_id: 'ep01_sh001',
            scene_id: 's1',
            order: 0,
            frame_path: 'frames/ep01_sh001.png',
          }),
        ],
      }),
    )
    store.saveProject(project)

    const moved = path.join(tmp, '搬到这儿')
    // 不能用 fs.cpSync：中文路径下它会让 Node 段错误退出，见 engine/fsx.js
    copyDirSync(store.root, moved)

    const there = new ProjectStore(moved).loadProject()
    const shot = there.episodes[0].shots[0]
    expect(shot.frame_path).toBe('frames/ep01_sh001.png')
    expect(new ProjectStore(moved).paths.abs(shot.frame_path)).toContain('搬到这儿')
  })
})

describe('剧集与镜头', () => {
  const episode = () =>
    EpisodeSchema.parse({
      episode_id: 'ep01',
      shots: [
        ShotSchema.parse({
          shot_id: 'ep01_sh002', scene_id: 's1', order: 1, duration_s: 3,
          status: ShotStatus.AUDIO_DONE,
        }),
        ShotSchema.parse({
          shot_id: 'ep01_sh001', scene_id: 's1', order: 0, duration_s: 5,
          status: ShotStatus.PLANNED,
        }),
      ],
    })

  it('按顺序排列', () => {
    // 存进 JSON 的顺序不保证，装配时得按 order 排
    expect(sortedShots(episode()).map((s) => s.shot_id)).toEqual([
      'ep01_sh001', 'ep01_sh002',
    ])
  })

  it('按状态取镜头支撑断点续跑', () => {
    const got = shotsNeeding(episode(), ShotStatus.PLANNED)
    expect(got.map((s) => s.shot_id)).toEqual(['ep01_sh001'])
  })

  it('统计各状态数量', () => {
    expect(countsByStatus(episode())).toEqual({ planned: 1, audio_done: 1 })
  })

  it('累计时长', () => {
    expect(plannedDurationS(episode())).toBe(8)
  })

  it('按 id 找剧集', () => {
    const project = { episodes: [episode()] }
    expect(episodeById(project, 'ep01').episode_id).toBe('ep01')
    expect(episodeById(project, 'ep99')).toBeNull()
  })
})
