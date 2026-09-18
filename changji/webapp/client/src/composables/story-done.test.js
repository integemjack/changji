/**
 * 新建一部电影、写完第一章，顶栏上「设定」那一格**得当场出现**，
 * 不该要人刷新一次页面才看得见（用户 2026-09-18）。
 *
 * 那一格是按 `session.done.story` 显示的（App.vue 的 visibleSteps），而
 * `session.refresh()` 只在挂载时、换项目 / 换章时、以及带 `refresh: true`
 * 的动作之后才跑。故事页上改故事的六条路里，删章 / 直接开写 / 反推三条
 * 带着它，敲字存稿 / AI 写这一章 / 批量展开这三条没有——漏掉的恰恰是
 * 最常走的。所以 StoryView 盯住 storyIsDone 的变化，不一致就补一趟。
 *
 * 这个文件测的是那两条判据本身。它们错了的表现都不报错：
 *   · 判宽了（比如按"有没有章节"算）——「直接开写」建的那一章是空的，
 *     一按下去「设定」就亮，而人正要开始写第一章；
 *   · 判窄了（比如 trim）——只敲了空格的章会让两边永远对不上，于是
 *     故事每变一次就白问一趟 /bff/flow，全站最慢的那条。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

import { needsFlowReread, storyIsDone } from './story-done'

describe('故事算不算做完了', () => {
  it('一章都没有 → 没做完', () => {
    expect(storyIsDone([])).toBe(false)
    expect(storyIsDone(null)).toBe(false)
    expect(storyIsDone(undefined)).toBe(false)
  })

  it('「直接开写」建的那一章是空的，不算做完', () => {
    // flow.cpp 那段注释写的就是这一条：按章节数判的话，那颗按钮一按下去
    // 这一步就打上勾，而人正要开始写第一章。
    expect(storyIsDone([{ title: '第一章', summary: '', text: '' }])).toBe(false)
  })

  it('只有大纲算，只有正文也算——两条路都要认', () => {
    // 采用大纲那条先有 summary、正文还没写；从别处粘正文进来那条先有 text。
    expect(storyIsDone([{ summary: '他回到了那座城', text: '' }])).toBe(true)
    expect(storyIsDone([{ summary: '', text: '夜.医院走廊' }])).toBe(true)
  })

  it('十章里只要有一章有字就算', () => {
    const chapters = Array.from({ length: 10 }, () => ({ summary: '', text: '' }))
    expect(storyIsDone(chapters)).toBe(false)
    chapters[7].text = '一'
    expect(storyIsDone(chapters)).toBe(true)
  })

  it('**不 trim**：只敲了空格也算有字', () => {
    // 引擎判的是原串非空。这边一 trim 就和它对不上，而对不上的代价是
    // 每次故事一变就白问一趟 /bff/flow。
    expect(storyIsDone([{ summary: ' ', text: '' }])).toBe(true)
    expect(storyIsDone([{ summary: '', text: '\n' }])).toBe(true)
  })

  it('字段缺了当空串，别抛', () => {
    // 老引擎回的章节可能没有 summary 这个键。抛出去的话整页白屏。
    expect(storyIsDone([{}])).toBe(false)
    expect(storyIsDone([null])).toBe(false)
    expect(storyIsDone([{ text: '有字' }])).toBe(true)
  })
})

describe('该不该补问一趟流程', () => {
  it('写出第一章：引擎还说没做完 → 问', () => {
    expect(needsFlowReread(true, false)).toBe(true)
  })

  it('把最后一章删光：引擎还说做完了 → 也要问', () => {
    // 反方向同样得灭，不然导航会给一部没有故事的电影一直打着勾。
    expect(needsFlowReread(false, true)).toBe(true)
  })

  it('两边一致 → 一个字都不问', () => {
    // 带 refresh: true 那三条路（删章 / 直接开写 / 反推）走到这儿时
    // refresh 已经落地了，不能让它们多问一趟。
    expect(needsFlowReread(true, true)).toBe(false)
    expect(needsFlowReread(false, false)).toBe(false)
  })

  it('done.story 还没读回来（undefined）当没做完', () => {
    // flow 读砸了 / 还没回来时 session.done 是个空对象。
    expect(needsFlowReread(false, undefined)).toBe(false)
    expect(needsFlowReread(true, undefined)).toBe(true)
  })
})

/**
 * **判据是抄引擎的，就得盯着别走散。**
 *
 * 引擎那份在 flow.cpp 里。哪天有人在那边加了条件（比如"还要有章节计划"），
 * 这边不跟就又回到"两边对不上、来回问"。这条读 C++ 源码，做法同
 * app-topbar / leave-guards 那几条读源码的测试。
 */
describe('和引擎那份对得上', () => {
  const FLOW = fileURLToPath(
    new URL('../../../../cpp/src/http/flow.cpp', import.meta.url),
  )

  it('flow.cpp 还在老地方', () => {
    expect(fs.existsSync(FLOW), `找不到 ${FLOW}——引擎那份判据挪了地方`).toBe(true)
  })

  it('引擎判的还是「summary 或 text 非空」', () => {
    const src = fs.readFileSync(FLOW, 'utf8')
    const at = src.indexOf('done["story"]')
    expect(at, 'flow.cpp 里不再算 done["story"] 了').toBeGreaterThan(0)
    const rule = src.slice(at, at + 400)
    expect(rule).toContain('any_of')
    expect(rule).toContain('"summary"')
    expect(rule).toContain('"text"')
    // 两个都要认，中间是"或"
    expect(rule).toMatch(/\|\|/)
    // 引擎那边没有 trim：它判的是 .empty()
    expect(rule).toContain('.empty()')
  })
})
