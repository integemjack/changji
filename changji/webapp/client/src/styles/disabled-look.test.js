/**
 * **禁用要变灰，不能只是调淡。**
 *
 * 这几条原来都只有一句 `opacity`：
 *
 *     .btn:disabled { opacity: .45 }
 *     .input:disabled, .textarea:disabled, .select:disabled { opacity: .55 }
 *
 * 调淡把颜色留着了。于是一颗禁用的「出片」还是那身主色、「删掉」还是红的、
 * 「一键出图」还是那道橙紫渐变——看上去就是一颗**淡一点的、能点的**按钮。
 * 设定页读不出来的时候实测撞到过：那颗「一键出图」确实是禁用的（属性、
 * 鼠标形状都对），可它照样亮着一道渐变，人只会去点它。
 *
 * 所以判据是"有没有真的换成中性色"，不是"有没有 opacity"。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const CSS = fs.readFileSync(
  fileURLToPath(new URL('./base.css', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text.replace(/\/\*[\s\S]*?\*\//g, '')
}

const css = code(CSS)

/** 取某个选择器那一条规则的声明块。 */
function rule(selector) {
  const at = css.indexOf(selector)
  if (at < 0) return ''
  const open = css.indexOf('{', at)
  return css.slice(open, css.indexOf('}', open))
}

describe('禁用态', () => {
  it('按钮：底、字、边框都换成中性色', () => {
    const r = rule('.btn:disabled')
    expect(r, '.btn:disabled 不见了').not.toBe('')
    expect(r, '底色没换').toMatch(/background:\s*var\(--surface/)
    expect(r, '字色没换').toMatch(/color:\s*var\(--text-3\)/)
    expect(r, '边框没换').toMatch(/border-color:\s*var\(--line\)/)
  })

  it('按钮：**不许再靠 opacity 蒙混**', () => {
    const r = rule('.btn:disabled')
    // 留着 `opacity: 1` 是显式覆盖，别的值一律不行
    const m = r.match(/opacity:\s*([\d.]+)/)
    expect(m, 'opacity 那一句删了也行，但别写成小于 1').toBeTruthy()
    expect(Number(m[1]), '又调回半透明了').toBe(1)
  })

  it('按钮：把各变体自己的背景盖掉', () => {
    const r = rule('.btn:disabled')
    // --ai 那道渐变走的是 background-image，hover 那句亮度走的是 filter
    expect(r, '--ai 那道渐变会漏出来').toMatch(/background-image:\s*none/)
    expect(r, 'hover 的 brightness 会留着').toMatch(/filter:\s*none/)
  })

  it('ghost 那一档保持透明，别凭空多出个方块', () => {
    // 它平时就是透明的，而"多出一块底"恰好是它 hover 的样子——
    // 禁用长得像被悬停，比不改还糟。靠字色区分。
    const r = rule('.btn--ghost:disabled')
    expect(r).toMatch(/background:\s*transparent/)
  })

  it('输入框、下拉：同样换灰，不只是调淡', () => {
    const r = rule('.input:disabled')
    expect(r).toMatch(/background:\s*var\(--surface/)
    expect(r).toMatch(/color:\s*var\(--text-3\)/)
  })

  it('三处都要说明点不动', () => {
    expect(rule('.btn:disabled')).toMatch(/cursor:\s*not-allowed/)
    expect(rule('.input:disabled')).toMatch(/cursor:\s*not-allowed/)
    expect(rule('.iconbtn:disabled')).toMatch(/cursor:\s*not-allowed/)
  })
})
