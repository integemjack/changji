/**
 * 传参考图、传人声、撤掉那几颗，**正在跑的时候要看得出来**。
 *
 * `useAction` 里那段注释记着这个功能是怎么来的：2026-09-11 用户报"点了一个
 * 「画」再点另一个，什么都不会发生——没反应、不报错、按钮也不变灰，看着
 * 就像界面坏了"。修法是按 key 各跑各的，**同一个 key 再点还是拦**（防手抖
 * 连点）。
 *
 * 而这几颗恰恰是"被拦了但不说"的那一类：`run()` 那头老老实实传了 key
 * （`up:` / `clr:` / `voice:`），模板这头**一个都没读**。于是传一张图那几秒
 * 里按钮照旧亮着、点了没反应，人只会再选一次文件。旁边的「画」和「试听」
 * 两颗就在同一排，它们是对的——三缺二。
 *
 * 传文件那颗是 `<label class="btn">`：**label 接不了 disabled 属性**，
 * 加了浏览器也不认、照样点得开文件选择器。所以走 `.is-off`，它和真禁用
 * 共用 base.css 里同一个声明块，外加一句 `pointer-events: none`。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const CH = fs.readFileSync(
  fileURLToPath(new URL('./AssetCharacters.vue', import.meta.url)),
  'utf8',
)
const LOC = fs.readFileSync(
  fileURLToPath(new URL('./AssetLocations.vue', import.meta.url)),
  'utf8',
)
const CSS = fs.readFileSync(
  fileURLToPath(new URL('../../styles/base.css', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const ch = code(CH)
const loc = code(LOC)

describe('传文件那几颗按钮', () => {
  it('角色的参考图：传和撤都认自己那个 key', () => {
    expect(ch, '传那颗没认 up:').toMatch(/'is-off': isBusy\('up:' \+ openChar\.char_id \+ s\.key\)/)
    expect(ch, '撤那颗没认 clr:').toMatch(/:disabled="isBusy\('clr:' \+ openChar\.char_id \+ s\.key\)"/)
  })

  it('角色的人声：传和撤都认 voice:', () => {
    expect(ch).toMatch(/'is-off': isBusy\('voice:' \+ openChar\.char_id\)/)
    expect(ch).toMatch(/:disabled="isBusy\('voice:' \+ openChar\.char_id\)"/)
  })

  it('场景的空景图：传和撤一样', () => {
    expect(loc).toMatch(/'is-off': isBusy\('up:' \+ openLoc\.location_id\)/)
    expect(loc).toMatch(/:disabled="isBusy\('clr:' \+ openLoc\.location_id\)"/)
  })

  it('跑着的时候字要变，不能只有颜色', () => {
    // 颜色说的是"点不动"，字说的是"在干嘛"。少了后者，人不知道要等多久、
    // 甚至不知道是自己刚才点上了。
    expect(ch).toContain("'传着…'")
    expect(ch).toContain("'撤着…'")
    expect(loc).toContain("'传着…'")
    expect(loc).toContain("'撤着…'")
  })

  it('`.is-off` 和真禁用共用一个声明块，别让两边漂开', () => {
    const css = code(CSS)
    const at = css.indexOf('.btn.is-off')
    expect(at, '.is-off 没有样式').toBeGreaterThan(0)
    // 它得和 :disabled 写在同一组选择器里
    const head = css.slice(Math.max(0, at - 120), at)
    expect(head, '.is-off 自己单开了一条规则').toMatch(/\.btn:disabled/)
    // label 点不动要靠它
    expect(css).toMatch(/\.btn\.is-off\s*\{[^}]*pointer-events:\s*none/)
  })
})
