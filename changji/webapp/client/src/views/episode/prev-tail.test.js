/**
 * 剧本页上的「上一章结尾」。
 *
 * 引擎在 `/api/script/context` 里一直给着 `previous_tail`（按章节计划取上一
 * 条的结尾，最多 300 字，`stages::script_tail`），而**前端一个地方都没读**
 * ——这一页的上下文脚注已经在显示同一个响应里的「照哪几章展开」「停在哪个
 * 钩子」，独独漏了这一条。
 *
 * 它不是可有可无的装饰：引擎写这一章剧本时就是把这一段拼进提示词的
 * （`render_script_context`）。人要判断"这一章接得上接不上"，看的必须是
 * **和模型看的同一段**——两边各取各的话，屏幕上说得通而模型手里是另一段。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fs.readFileSync(
  fileURLToPath(new URL('./EpScript.vue', import.meta.url)),
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

describe('上一章结尾', () => {
  it('读的是引擎给的那个字段，不自己另算一份', () => {
    // 自己去拉上一章的剧本再截 300 字的话，截法和引擎的不一定一样
    // （script_tail 会从第一个换行之后起，免得从半句中间开头）。
    expect(body).toMatch(/ctx\.value\?\.previous_tail/)
    expect(body, '别自己截').not.toMatch(/slice\(-300\)|substring\(.*300/)
  })

  it('摆在原文前面：时间上它在前', () => {
    const prev = body.indexOf('previousTail')
    const source = body.indexOf('v-if="sourceText"')
    expect(prev).toBeGreaterThan(0)
    expect(source).toBeGreaterThan(0)
    // 模板里「上一章结尾」那块要在「原文」那块之前
    const tplPrev = body.indexOf('v-if="previousTail"')
    expect(tplPrev, '模板里找不到那一块').toBeGreaterThan(0)
    expect(tplPrev).toBeLessThan(source)
  })

  it('没有就整块不显示，不留一个空折叠', () => {
    // 第一章、或者这一章不在章节计划上时它是空串。
    expect(body).toMatch(/v-if="previousTail"/)
  })

  it('默认收着：它是背景，不是这一页要读的正文', () => {
    const at = body.indexOf('v-if="previousTail"')
    const block = body.slice(at, at + 200)
    expect(block, '默认展开会把原文挤下去').not.toMatch(/\bopen\b/)
    // 而原文那块该开的时候还是要开（没剧本时人读的就是它）
    expect(body).toMatch(/v-if="sourceText"[^>]*:open="!script\.trim\(\)"/)
  })
})
