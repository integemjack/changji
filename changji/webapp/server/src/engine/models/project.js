/**
 * 项目模型与磁盘布局。
 *
 * 一个项目就是一个自包含的目录，换机器整个拷走即可。目录里只有相对路径，
 * 不含任何绝对路径，也不含模型文件（那些跟机器走，不跟项目走）。
 *
 *     我的短剧/
 *     ├── project.json          项目元数据与分镜表
 *     ├── assets.json           角色与场景资产库
 *     ├── changji.toml          项目级配置覆盖（可选）
 *     ├── refs/                 角色三视图、场景空景图
 *     ├── audio/                配音
 *     ├── frames/               逐镜首帧
 *     ├── shots/
 *     │   ├── draft/            草稿档视频
 *     │   └── final/            成片档视频
 *     ├── subtitles/
 *     └── output/               成片
 */

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { z } from 'zod'

import { AssetLibrarySchema, StyleLine } from './character.js'
import { ShotSchema, ShotStatus } from './shot.js'

export const PROJECT_FILE = 'project.json'
export const ASSETS_FILE = 'assets.json'
export const SCHEMA_VERSION = 1

const SUBDIRS = [
  'refs', 'audio', 'frames', 'shots/draft', 'shots/final',
  'subtitles', 'output', 'logs',
]

/** 一集。分镜表挂在这里。 */
export const EpisodeSchema = z
  .object({
    episode_id: z.string().regex(/^[a-z0-9_]+$/),
    title: z.string().default(''),
    synopsis: z.string().default(''),
    target_duration_s: z.number().gt(0).default(180), // 目标时长
    script: z.string().default(''), // 剧本原文
    shots: z.array(ShotSchema).default([]),
  })
  .strict()

export const sortedShots = (episode) =>
  [...episode.shots].sort((a, b) => a.order - b.order)

export const shotById = (episode, shotId) =>
  episode.shots.find((s) => s.shot_id === shotId) ?? null

export const plannedDurationS = (episode) =>
  episode.shots.reduce((a, s) => a + s.duration_s, 0)

export function countsByStatus(episode) {
  const out = {}
  for (const s of episode.shots) out[s.status] = (out[s.status] ?? 0) + 1
  return out
}

/** 取处于某个阶段的镜头。断点续跑靠它。 */
export const shotsNeeding = (episode, status) =>
  sortedShots(episode).filter((s) => s.status === status)

export const ProjectSchema = z
  .object({
    schema_version: z.number().int().default(SCHEMA_VERSION),
    project_id: z.string().regex(/^[a-z0-9_-]+$/),
    title: z.string().default(''),
    style_line: z.enum(Object.values(StyleLine)).default(StyleLine.REALISTIC),
    // 这部剧讲什么。写下一集时当提示词用。
    // 不存的话，隔天想接着写第六集，得凭记忆把当初那句话重打一遍。
    premise: z.string().max(2000).default(''),
    created_at: z.string().default(() => new Date().toISOString()),
    updated_at: z.string().default(() => new Date().toISOString()),
    episodes: z.array(EpisodeSchema).default([]),
  })
  .strict()

export const episodeById = (project, episodeId) =>
  project.episodes.find((e) => e.episode_id === episodeId) ?? null

/** 项目目录布局。所有路径都由项目根推导，绝不写死。 */
export class ProjectPaths {
  constructor(root) {
    this.root = path.resolve(expandHome(String(root)))
  }

  ensure() {
    for (const sub of SUBDIRS) {
      fs.mkdirSync(path.join(this.root, sub), { recursive: true })
    }
    return this
  }

  get projectFile() {
    return path.join(this.root, PROJECT_FILE)
  }

  get assetsFile() {
    return path.join(this.root, ASSETS_FILE)
  }

  get refs() {
    return path.join(this.root, 'refs')
  }

  get audio() {
    return path.join(this.root, 'audio')
  }

  get frames() {
    return path.join(this.root, 'frames')
  }

  get subtitles() {
    return path.join(this.root, 'subtitles')
  }

  get output() {
    return path.join(this.root, 'output')
  }

  get logs() {
    return path.join(this.root, 'logs')
  }

  shots(tier) {
    return path.join(this.root, 'shots', tier)
  }

  /**
   * 绝对路径转成相对项目根的路径。存进 JSON 的一律用这个。
   *
   * 用正斜杠，保证在 Windows 上存的项目拿到 Linux 上也能读。
   */
  rel(target) {
    const p = path.resolve(expandHome(String(target)))
    const relative = path.relative(this.root, p)
    if (relative.startsWith('..') || path.isAbsolute(relative)) {
      // 不在项目内的文件只能存绝对路径，但这会破坏可移植性
      throw new Error(
        `路径不在项目目录内，存进项目会破坏可移植性：${p}\n请先把文件复制进 ${this.root}`,
      )
    }
    return relative.split(path.sep).join('/')
  }

  /** 相对路径还原成绝对路径。 */
  abs(relPath) {
    return path.resolve(this.root, relPath)
  }
}

function expandHome(p) {
  return p.startsWith('~') ? path.join(os.homedir(), p.slice(1)) : p
}

/** 项目的读写。写入用原子替换，避免中途断电留下半个文件。 */
export class ProjectStore {
  constructor(root) {
    this.paths = new ProjectPaths(root)
  }

  get root() {
    return this.paths.root
  }

  exists() {
    return fs.existsSync(this.paths.projectFile) &&
      fs.statSync(this.paths.projectFile).isFile()
  }

  // ---- 创建 ----

  static create(root, projectId, title = '', styleLine = StyleLine.REALISTIC) {
    const store = new ProjectStore(root)
    if (store.exists()) {
      const err = new Error(`这个目录已经是一个项目了：${store.root}`)
      err.code = 'EEXIST'
      throw err
    }
    store.paths.ensure()
    store.saveProject(
      ProjectSchema.parse({
        project_id: projectId,
        title: title || projectId,
        style_line: styleLine,
      }),
    )
    store.saveAssets(
      AssetLibrarySchema.parse({ style: { style_line: styleLine } }),
    )
    return store
  }

  // ---- 读 ----

  loadProject() {
    if (!this.exists()) {
      throw new Error(
        `这里不是一个项目目录：${this.root}\n用 changji new 创建，或者切到正确的目录`,
      )
    }
    const raw = readJson(this.paths.projectFile)
    const version = raw.schema_version ?? 0
    if (version > SCHEMA_VERSION) {
      throw new Error(
        `项目是用更新版本的场记创建的（格式版本 ${version}，本机支持到 ${SCHEMA_VERSION}）。请升级后再打开`,
      )
    }
    return ProjectSchema.parse(raw)
  }

  loadAssets() {
    if (!fs.existsSync(this.paths.assetsFile)) {
      return AssetLibrarySchema.parse({})
    }
    return AssetLibrarySchema.parse(readJson(this.paths.assetsFile))
  }

  // ---- 写 ----

  saveProject(project) {
    project.updated_at = new Date().toISOString()
    writeJson(this.paths.projectFile, project)
  }

  saveAssets(assets) {
    writeJson(this.paths.assetsFile, assets)
  }

  /**
   * 把外部文件复制进项目，返回相对路径。
   *
   * 参考图这类素材必须复制进来而不是引用原位置，否则项目拷到别的机器就断链。
   */
  copyInto(src, subdir, name = null) {
    const source = path.resolve(expandHome(String(src)))
    if (!fs.existsSync(source) || !fs.statSync(source).isFile()) {
      throw new Error(`文件不存在：${source}`)
    }
    const targetDir = path.join(this.root, subdir)
    fs.mkdirSync(targetDir, { recursive: true })
    const target = path.join(targetDir, name || path.basename(source))
    if (source !== target) fs.copyFileSync(source, target)
    return this.paths.rel(target)
  }
}

function readJson(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'))
  } catch (err) {
    throw new Error(`文件损坏，不是合法 JSON：${file}\n${err.message}`)
  }
}

/** 原子写。先写临时文件再替换，中途断电不会留下半个文件。 */
function writeJson(file, data) {
  fs.mkdirSync(path.dirname(file), { recursive: true })
  const tmp = path.join(
    path.dirname(file),
    `.${path.basename(file)}.${process.pid}.${Date.now()}.tmp`,
  )
  try {
    fs.writeFileSync(tmp, JSON.stringify(data, null, 2), 'utf8')
    fs.renameSync(tmp, file)
  } catch (err) {
    try {
      fs.unlinkSync(tmp)
    } catch {
      // 临时文件本来就没建成，没什么可清的
    }
    throw err
  }
}

export { ShotStatus }
