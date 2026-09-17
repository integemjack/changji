/**
 * 设定页那颗「理解故事」：故事没正文时**按不动**，悬停说清去哪儿补。
 *
 * 2026-09-17 之前这儿钉的是「照故事定妆」的判据（有故事、或者有写好的剧本
 * ——引擎 post_bible 的 source=auto 那两条路）。定妆并进理解故事之后，剧本
 * 是理解写出来的，不再是来源；判据收成一条。
 *
 * 真引擎上量过（新建一个空项目）：那颗按钮原来是亮的、按下去 400。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const VIEW = fs.readFileSync(
  fileURLToPath(new URL('./AssetsView.vue', import.meta.url)),
  'utf8',
)
/** 引擎那一头。**跨文件比**：它换了判据这边会悄悄失准。 */

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const body = code(VIEW)

describe('理解故事：没正文就按不动', () => {
  // 2026-09-17 起「照故事定妆」并进了「理解故事」（提结构 → 定长相 → 逐章写
  // 剧本，一件活）。它读的是故事的正文，所以判据只有一条：有没有一章有字。
  // 原来那条"或者写好的剧本"不在了——剧本现在是理解写出来的，不是它的来源。
  it('有一个"有没有正文"的判据', () => {
    expect(body).toMatch(/const canUnderstand = computed/)
    const at = body.indexOf('const canUnderstand = computed')
    expect(body.slice(at, at + 200)).toContain("(c.text ?? '').trim()")
  })

  it('按钮挂上它，而且那句悬停要说清缺的是什么、去哪儿补', () => {
    const at = body.indexOf('@click="understand"')
    expect(at).toBeGreaterThan(0)
    const btn = body.slice(body.lastIndexOf('<button', at), at)
    expect(btn, '没挂上判据').toContain('!canUnderstand')
    expect(btn, '悬停没说缺什么').toMatch(/还没有正文/)
    expect(btn, '悬停没说去哪儿').toMatch(/故事页/)
  })

  it('「出图」要等理解完才出现', () => {
    const at = body.indexOf('@click="genAll"')
    expect(at).toBeGreaterThan(0)
    expect(body.slice(Math.max(0, at - 1200), at)).toContain(
      'v-if="session.counters.understood"',
    )
  })
})
