/**
 * 投递路径的边界检查。
 *
 * 这一条决定"能不能读这个文件"。它原来是 deliverOne 里的两行，
 * 没导出、没有任何测试——而它挡的是"把项目目录外的文件投出去"。
 */

import { describe, expect, it } from 'vitest'
import path from 'node:path'
import { insideRoot } from './publish.js'

const ROOT = path.resolve('/a/proj')

describe('insideRoot', () => {
  it('目录里的文件放行', () => {
    expect(insideRoot(ROOT, path.join(ROOT, 'output', 'ep01.mp4'))).toBe(true)
  })

  it('目录本身不算可投递的文件', () => {
    // 放行的话后面 statSync 拿到的是目录，然后去"拷一个目录"。
    expect(insideRoot(ROOT, ROOT)).toBe(false)
  })

  it('往上跳要挡住', () => {
    expect(insideRoot(ROOT, path.join(ROOT, '..', 'secret.txt'))).toBe(false)
    expect(insideRoot(ROOT, path.join(ROOT, 'a', '..', '..', 'secret.txt')))
      .toBe(false)
  })

  it('同前缀的兄弟目录要挡住', () => {
    // root 是 /a/proj，别让 /a/proj2 混进来——所以拼的是 root + 分隔符。
    expect(insideRoot(ROOT, path.resolve('/a/proj2/x.mp4'))).toBe(false)
    expect(insideRoot(ROOT, path.resolve('/a/project/x.mp4'))).toBe(false)
  })

  it('绝对路径要挡住', () => {
    // **这一条最要紧。** path.resolve(root, rel) 碰上绝对的 rel 会直接
    // 返回它、把 root 整个忽略掉，全靠 insideRoot 挡。
    //
    // 路径**用 path.sep 拼**，不写反斜杠字面量：第一版写成
    // 'C:\Windows\win.ini'，经过 shell 之后掉成了单反斜杠，
    // JS 把 \W 读成 W，于是那串变成 'C:Windowswin.ini'——
    // 那是个**驱动器相对路径**，本来就该落在 ROOT 里面。
    // 用例因此报红，而红的是用例不是代码。
    const abs = process.platform === 'win32'
      ? ['C:', 'Windows', 'win.ini'].join(path.sep)
      : ['', 'etc', 'passwd'].join(path.sep)
    expect(path.isAbsolute(abs)).toBe(true)   // 先确认造出来的确实是绝对路径
    expect(insideRoot(ROOT, path.resolve(ROOT, abs))).toBe(false)
  })

  it('空值不放行', () => {
    expect(insideRoot('', '/a/proj/x')).toBe(false)
    expect(insideRoot(ROOT, '')).toBe(false)
  })
})
