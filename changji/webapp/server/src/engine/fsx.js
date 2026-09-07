/**
 * 文件操作里几个不能直接用标准库的地方。
 *
 * ⚠ fs.cpSync 在这个项目里是禁用的。
 *
 * Node 22.20.0 / Windows 上，路径里带非 ASCII 字符时 fs.cpSync 会让进程
 * 直接段错误退出——不是抛异常，是整个进程没了，try/catch 拦不住。
 * 最小复现：
 *
 *     fs.mkdirSync(path.join(tmp, '剧'), { recursive: true })
 *     fs.cpSync(path.join(tmp, '剧'), path.join(tmp, '搬到这儿'), { recursive: true })
 *     // → Segmentation fault (139)
 *
 * 这对这套东西是致命的：项目目录名基本都是中文（雪夜、本轮验收……），
 * 复制一集、备份、导出只要走到 cpSync 就是硬崩。
 *
 * 实测同样的路径下 fs.promises.cp（异步版）和 fs.rmSync 都正常，
 * 所以下面的 copyDir 走异步版，copyDirSync 自己递归，都绕开 cpSync。
 */

import fs from 'node:fs'
import path from 'node:path'

/** 递归复制目录。异步版，内部用 fs.promises.cp。 */
export async function copyDir(src, dest) {
  await fs.promises.cp(src, dest, { recursive: true })
}

/**
 * 递归复制目录，同步版。
 *
 * 自己走一遍目录树而不是调 cpSync——理由见文件头。
 */
export function copyDirSync(src, dest) {
  fs.mkdirSync(dest, { recursive: true })
  for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
    const from = path.join(src, entry.name)
    const to = path.join(dest, entry.name)
    if (entry.isDirectory()) copyDirSync(from, to)
    else if (entry.isSymbolicLink()) fs.symlinkSync(fs.readlinkSync(from), to)
    else fs.copyFileSync(from, to)
  }
}
