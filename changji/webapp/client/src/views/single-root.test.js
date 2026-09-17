/**
 * **每一个挂在路由上的页面必须是单根。**
 *
 * ⚠️ 2026-09-17 的事故：`SettingsView` 的模板里，那个挑目录的遮罩弹窗和
 * 根 `<div>` **并排**摆在最外层（注释写着「挂在最外层：嵌在某一节里会被
 * 那一节的 overflow 裁掉」）。多根 = fragment。
 *
 * 而 `App.vue` 里路由那一层是 `<Transition name="fade" mode="out-in">`，
 * `<Transition>` 要的是**单个根元素**。多根的那一页离开时那次 leave 永远
 * 不结束，`out-in` 于是再也不放新的进来——**从设置页切到任何一页都是白的，
 * 而且之后每一次跳页都白，直到整页刷新**（用户原话：「在设置页面切到别的
 * 页面要刷新才能看到内容」）。
 *
 * **一个字的报错都没有**：控制台干净、网络全 200、路由也匹配上了，
 * `RouterView` 那一格就是空的。查了很久。
 *
 * 要在根之外再摆东西，用 `<Teleport>`：DOM 上它照样挂到 body 去、不被谁的
 * overflow 裁，而模板这头不算根节点。
 *
 * **这条只数根节点，不管别的**：判宽了会把正常的模板判红。注释不算根
 * （生产构建会剥掉），所以只数以 `  <` 开头、且不是注释的那几行。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const dir = fileURLToPath(new URL('.', import.meta.url))
const views = fs.readdirSync(dir).filter((f) => f.endsWith('.vue'))

/** 模板里的顶层节点。开标签数减闭标签数，按缩进两格那一层算。 */
function rootTags(src) {
  const m = src.match(/\n<template>\n([\s\S]*?)\n<\/template>/)
  if (!m) return []
  const lines = m[1].split('\n')
  const tops = []
  let depth = 0
  let inComment = false
  for (const raw of lines) {
    const line = raw.trimEnd()
    if (inComment) {
      if (line.includes('-->')) inComment = false
      continue
    }
    const t = line.trim()
    if (t.startsWith('<!--')) {
      if (!t.includes('-->')) inComment = true
      continue
    }
    if (!t) continue
    // 只看缩进两格那一层
    const indent = line.length - line.trimStart().length
    if (indent === 2 && t.startsWith('<') && !t.startsWith('</')) {
      if (depth === 0) tops.push(t.slice(0, 40))
    }
    // 粗略跟一下层级：自闭合的不算进出
    const opens = (line.match(/<[A-Za-z][^/>]*?>/g) || []).length
    const closes = (line.match(/<\/[A-Za-z][^>]*>/g) || []).length
    const selfClosing = (line.match(/\/>/g) || []).length
    depth += opens - closes - selfClosing
    if (depth < 0) depth = 0
  }
  return tops
}

describe('路由页面必须单根', () => {
  it.each(views)('%s', (file) => {
    const src = fs.readFileSync(path.join(dir, file), 'utf8')
    const tops = rootTags(src)
    expect(
      tops.length,
      `${file} 有 ${tops.length} 个根节点：${tops.join(' / ')}\n` +
        '多根组件在 <Transition mode="out-in"> 下会把整个路由卡死。' +
        '要在根之外摆东西用 <Teleport>。',
    ).toBe(1)
  })
})
