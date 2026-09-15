/**
 * 写正文 / 改一段写到一半**连接断了**，流出来的字不能跟着一起清掉。
 *
 * 这是「按停」那条（b260cf1，用户 2026-09-15：「ai 写文章点击停下来之前
 * 写的内容应该保留」）的同一件事，换了个触发方式——而那一版只认了按停：
 *
 *     byHand = !fin.ok && stoppedByHand(fin.message)
 *     if (byHand && acc.trim()) { 留着 }
 *     buf[chapterId] = had          // ← 断线落在这儿，一个字不剩
 *
 * 浏览器里验过（假引擎补了一个 WebSocket，写到一半把那条连接掐掉）：
 * 38 个字流进编辑器，掐断，红字「和引擎的连接断了，这一章写没写完不好说」，
 * 编辑器回到 0 字。而那句话自己都说"不好说"——**在"不知道"上做了破坏性的
 * 那个选择**。
 *
 * ---- 留，但是不存 ----
 *
 * 按停那条敢 `scheduleSave`，是因为停是确定的：引擎收到停就不会再写了。
 * 断线不是——那头可能已经写完并落库了，这会儿把半截存回去就是拿它盖掉
 * 完整那一份（`applyRevision` 是按整章长度替换的）。所以留在编辑器里、
 * 摆上底稿（Ctrl+Z / 撤销）、**不存**，并且把"刷新看引擎那份"说出来。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fs.readFileSync(
  fileURLToPath(new URL('./StoryView.vue', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const body = code(SRC)

describe('写到一半断线', () => {
  it('两条路都认得出"断了"这一种，而且和"按停"分开', () => {
    // onLost 只在"我们跟丢了"时叫（socket 掉了 / 信箱取不到 / 收尾那条没
    // 看见），和引擎明说的 job_error 是两回事。
    expect((body.match(/lostLink = true/g) || []).length, '两处都要记').toBe(2)
    expect((body.match(/let lostLink = false/g) || []).length).toBe(2)
    expect((body.match(/if \(lostLink && acc\.trim\(\)\)/g) || []).length).toBe(2)
  })

  it('⚠️ 那面旗要**声明在 openJobFeed 之前**', () => {
    // onLost 可能在 `await openJobFeed(...)` 里面就叫（socket 当场被拒）。
    // 声明留在后面踩的是 TDZ——ReferenceError，而且只在"刚好那一刻断了"
    // 才现，平时一次都撞不到。
    //
    // 不变式：每一处都是 声明 → openJobFeed( → lostLink = true。
    let at = 0
    for (let i = 0; i < 2; i += 1) {
      const decl = body.indexOf('let lostLink = false', at)
      expect(decl, `第 ${i + 1} 处声明没了`).toBeGreaterThan(0)
      const call = body.indexOf('openJobFeed(', decl)
      const set = body.indexOf('lostLink = true', decl)
      expect(call, '声明后面找不到 openJobFeed').toBeGreaterThan(decl)
      expect(set, '赋值不在那次 openJobFeed 里').toBeGreaterThan(call)
      at = decl + 1
    }
  })

  /** 两处「断了但字还在」的收尾，各取一段。 */
  function keepBlocks() {
    const out = []
    let at = 0
    for (;;) {
      const a = body.indexOf('if (lostLink && acc.trim()) {', at)
      if (a < 0) break
      out.push(body.slice(a, body.indexOf('if (lostLink) {', a)))
      at = a + 1
    }
    return out
  }

  it('两处都是"留字、摆底稿，但**不存**"', () => {
    const blocks = keepBlocks()
    expect(blocks.length, '不是两处').toBe(2)
    for (const f of blocks) {
      expect(f.length).toBeGreaterThan(50)
      expect(f, '没摆底稿，Ctrl+Z / 撤销就退不回原来那份').toContain('pending.value =')
      // **这一条是要害**：那头可能已经写完并落库了，存半截等于盖掉完整那份
      expect(f, '**不许存**').not.toMatch(/scheduleSave/)
      expect(f, '要说清没存').toMatch(/没存下去/)
      expect(f, '要说怎么拿引擎那份').toMatch(/刷新/)
    }
  })

  it('写正文那一处留的是流出来那一份', () => {
    expect(keepBlocks().some((f) => f.includes('buf[chapterId] = acc'))).toBe(true)
  })

  it('断线不再弹那条红的：话由下面那两支说', () => {
    // 两条一起弹的话，人先看到的是红的那句，而它只说"不好说"，
    // 不说字还在不在、也不说该怎么办。
    expect(body).toMatch(/if \(!fin\.ok && !byHand && !lostLink\) ui\.error/)
    expect((body.match(/!byHand && !lostLink/g) || []).length).toBe(2)
  })

  it('一个字都没流出来也要说清是"断了"不是"写砸了"', () => {
    // 两者该做的事不一样：前者刷新看看，后者重来一次。
    expect((body.match(/if \(lostLink\) \{/g) || []).length).toBe(2)
    expect(body).toMatch(/连接断了，这一章写没写完不好说/)
    expect(body).toMatch(/连接断了，这一段改没改完不好说/)
  })
})
