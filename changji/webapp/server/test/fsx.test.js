// 目录复制。
//
// 这一组存在的唯一理由是钉死一个平台坑：Node 22 / Windows 上
// fs.cpSync 遇到非 ASCII 路径会段错误退出。项目目录名基本都是中文，
// 所以引擎里凡是复制目录都必须走这里的实现。
// 详见 src/engine/fsx.js 文件头。

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'

import { copyDir, copyDirSync } from '../src/engine/fsx.js'

let tmp

beforeEach(() => {
  tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'changji-fsx-'))
})
afterEach(() => {
  fs.rmSync(tmp, { recursive: true, force: true })
})

function makeTree(root) {
  fs.mkdirSync(path.join(root, 'refs'), { recursive: true })
  fs.mkdirSync(path.join(root, 'shots', 'draft'), { recursive: true })
  fs.writeFileSync(path.join(root, 'project.json'), '{"a":1}', 'utf8')
  fs.writeFileSync(path.join(root, 'refs', '角色_正面.png'), 'png', 'utf8')
  fs.writeFileSync(path.join(root, 'shots', 'draft', 'sh001.mp4'), 'mp4', 'utf8')
}

describe('copyDirSync', () => {
  it('中文目录名不会把进程搞崩', () => {
    // 这一条真跑起来才有意义：换成 fs.cpSync 的话整个 worker 会段错误退出，
    // 表现是测试「没有输出」而不是「失败」
    const src = path.join(tmp, '雪夜')
    makeTree(src)
    copyDirSync(src, path.join(tmp, '搬到这儿'))
    expect(
      fs.readFileSync(path.join(tmp, '搬到这儿', 'refs', '角色_正面.png'), 'utf8'),
    ).toBe('png')
  })

  it('目录结构和内容一字不差', () => {
    const src = path.join(tmp, 'a')
    makeTree(src)
    const dest = path.join(tmp, 'b')
    copyDirSync(src, dest)

    const walk = (root) =>
      fs
        .readdirSync(root, { withFileTypes: true, recursive: true })
        .map((e) => path.relative(root, path.join(e.parentPath ?? e.path, e.name)))
        .sort()
    expect(walk(dest)).toEqual(walk(src))
    expect(fs.readFileSync(path.join(dest, 'project.json'), 'utf8')).toBe('{"a":1}')
  })

  it('目标已存在时合并进去', () => {
    const src = path.join(tmp, 'a')
    makeTree(src)
    const dest = path.join(tmp, 'b')
    fs.mkdirSync(dest, { recursive: true })
    fs.writeFileSync(path.join(dest, '原有.txt'), 'keep', 'utf8')
    copyDirSync(src, dest)
    expect(fs.existsSync(path.join(dest, '原有.txt'))).toBe(true)
    expect(fs.existsSync(path.join(dest, 'project.json'))).toBe(true)
  })
})

describe('copyDir', () => {
  it('异步版同样能处理中文路径', async () => {
    const src = path.join(tmp, '本轮验收')
    makeTree(src)
    await copyDir(src, path.join(tmp, '副本'))
    expect(fs.existsSync(path.join(tmp, '副本', 'shots', 'draft', 'sh001.mp4'))).toBe(true)
  })
})
