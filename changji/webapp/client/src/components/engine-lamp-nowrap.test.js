/**
 * **这盏灯的字不能折行。**
 *
 * `.lamp` 的高是钉死的 26px。「引擎已连接」折成两行就是 38px，从药丸
 * 底下溢出去——而它是整屏唯一一处回答"引擎在不在"的地方，看上去就像
 * 界面裂了。
 *
 * 实测撞到的那一次：「这一集」那一页顶栏比别的页多一个集号选择器，
 * 视口 1142px（远不算窄屏）上这一格就放不下五个字，于是折了。
 * 640px 以下有另一条 media 把整段字藏掉，靠的不是折行让位。
 *
 * 判据只看 `.lamp__text` 上有没有 `white-space: nowrap`，不看 media
 * 那条——两件事，藏掉是窄屏的做法，不折是任何宽度都成立的底线。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fs.readFileSync(
  fileURLToPath(new URL('./EngineLamp.vue', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
const code = SRC.replace(/\/\*[\s\S]*?\*\//g, '').replace(/<!--[\s\S]*?-->/g, '')

/** 取顶层（不在 media 里）那条 `.lamp__text` 规则的声明块。 */
function topLevelLampText() {
  // media 块从 `@media` 起到它自己的收尾括号，先整段挖掉。
  let flat = code
  let at
  while ((at = flat.indexOf('@media')) >= 0) {
    const open = flat.indexOf('{', at)
    let depth = 0
    let i = open
    for (; i < flat.length; i += 1) {
      if (flat[i] === '{') depth += 1
      else if (flat[i] === '}') {
        depth -= 1
        if (depth === 0) break
      }
    }
    flat = flat.slice(0, at) + flat.slice(i + 1)
  }
  const k = flat.indexOf('.lamp__text')
  if (k < 0) return ''
  const open = flat.indexOf('{', k)
  return flat.slice(open, flat.indexOf('}', open))
}

describe('引擎灯', () => {
  it('那几个字钉死不折，任何宽度都不许溢出药丸', () => {
    expect(topLevelLampText()).toMatch(/white-space:\s*nowrap/)
  })

  it('药丸的高仍然是钉死的——所以上面那条才是必须的', () => {
    const at = code.indexOf('.lamp {')
    const block = code.slice(at, code.indexOf('}', at))
    expect(block).toMatch(/height:\s*26px/)
  })
})
