/**
 * **读不到画幅的时候，别替它报一个竖屏。**
 *
 * 跳转条（EpFilm）和镜头墙（EpShots）的每一格都按项目的 [video] 排长宽比，
 * 而 `/bff/project/video` 是会读砸的（换项目那一下的竞态、老引擎没这条
 * 接口、网络抖一下）。两页原来的回落都写着 9:16。
 *
 * 2026-09-18 新建项目默认画幅从 portrait 改成 landscape，那条回落当场变成
 * 一个**说得比知道的多**的值：最常见的那种项目读砸一次，16:9 的首帧就被
 * 塞进 9:16 的槽——跳转条那边是 `object-fit: cover`，左右各切掉三分之一还
 * 多，条上剩一溜画面正中间的竖条。两页的注释自己写下过这个故障，只是当年
 * 的触发条件是"横屏项目"。
 *
 * 这条钉两件事：
 *
 * 1. 读不到那一支跟引擎给新项目的画幅走（16:9），不是这一页自己编的竖屏。
 * 2. **回落只有一份。** CSS 里 `var(--cell-ratio, 9 / 16)` 是同一个默认值
 *    的第二份，上一轮改 JS 那份时它没跟；现在 `aspect-ratio` 直接读
 *    `--cell-ratio`，默认值没有第二个地方可藏。
 *
 * （能这么写的前提是那个自定义属性总有值：computed 每一支都返回一串比例，
 * 而摆格子的那个容器上挂着 `:style` 把它绑上去。两条都在下面钉着。）
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const HERE = fileURLToPath(new URL('.', import.meta.url))
const read = (f) => fs.readFileSync(path.join(HERE, f), 'utf8')

const PAGES = [
  { file: 'EpFilm.vue', ratio: 'stripRatio', holder: 'class="strip"' },
  { file: 'EpShots.vue', ratio: 'cellRatio', holder: 'class="wall"' },
]

describe.each(PAGES)('$file：读不到画幅时那一格排成什么样', ({ file, ratio, holder }) => {
  const src = read(file)

  it('读不到就按引擎给新项目的画幅排（16:9）', () => {
    const at = src.indexOf(`const ${ratio} = computed(`)
    expect(at, `${ratio} 不见了`).toBeGreaterThan(0)
    const body = src.slice(at, at + 260)
    expect(body).toContain("return '16 / 9'")
  })

  it('每一支都给得出一串比例——CSS 那头就是靠这条才敢不带默认值', () => {
    const at = src.indexOf(`const ${ratio} = computed(`)
    const body = src.slice(at, at + 260)
    const returns = body.match(/return [^\n]+/g) ?? []
    expect(returns.length).toBeGreaterThanOrEqual(2)
    for (const r of returns) expect(r).toMatch(/\d \/ \d|\$\{v\.width\} \/ \$\{v\.height\}/)
  })

  it('摆格子那个容器上绑着 --cell-ratio', () => {
    expect(src).toContain(holder)
    expect(src).toMatch(new RegExp(`'--cell-ratio':\\s*${ratio}`))
  })

  it('aspect-ratio 直接读 --cell-ratio，不再自带第二份默认值', () => {
    expect(src).toContain('aspect-ratio: var(--cell-ratio);')
    expect(src).not.toContain('var(--cell-ratio,')
  })

  it('整个文件里没有写死竖屏的比例了（注释里讲那个故障不算）', () => {
    const code = src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^\s*\/\/[^\n]*$/gm, '')
    expect(code).not.toContain('9 / 16')
    expect(code).not.toContain('9/16')
  })
})

/**
 * 成片页 2026-09-18 整个重写过（切段那条链拔掉，改成一部电影一个文件），
 * 顺手钉一条：它那个播放器不许再长出一个写死的画幅。
 * 现在的做法是不摆比例——`.player video` 只给宽度和上限高度，
 * 剩下的交给成片自己的画幅，这是唯一一个不用猜的做法。
 */
describe('FilmView.vue：成片播放器不猜画幅', () => {
  const src = fs.readFileSync(path.join(HERE, '..', 'FilmView.vue'), 'utf8')

  it('没有写死的长宽比', () => {
    expect(src).not.toContain('aspect-ratio')
    expect(src).not.toContain('9 / 16')
  })
})
