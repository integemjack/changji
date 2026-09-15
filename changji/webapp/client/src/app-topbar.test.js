/**
 * 顶栏那一排是**唯一的一张地图**（`.nav` 上面那段注释写着这句），
 * 而窄屏上它横着能滚、还不画滚动条。两条规矩必须一直在，少哪一条都是
 * "手机上看不出自己在第几步"，而且不报错。
 *
 *   一、集号让位给导航，不是反过来。窄屏那段 media query 的注释一直这么
 *       写着，可 `.ep` 是 `flex: none`——一步都不让；真正被挤掉的是 `.nav`
 *       （`min-width: 0` 又能横滚，缩起来没有下限）。375px 上进「这一集」
 *       那一页时导航只剩 44px，四步里一格都摆不下。
 *   二、打开一页要停在当前那一步上。滚动位置一直是 0 的话，手机上打开
 *       「这一集」——四步里的最后一步——那一格在可视区外七十多像素。
 *
 * 这一条读源码，做法同 leave-guards / settings-persist。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const APP = fs.readFileSync(fileURLToPath(new URL('./App.vue', import.meta.url)), 'utf8')

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

/** 窄屏那一段 media query 的正文。 */
function narrowBlock() {
  const at = APP.indexOf('@media (max-width: 860px)')
  expect(at, '窄屏那段 media query 不见了').toBeGreaterThan(0)
  return APP.slice(at, APP.indexOf('</style>', at))
}

describe('窄屏顶栏', () => {
  it('集号可以缩，而且留了个底', () => {
    const css = narrowBlock()
    const ep = css.slice(css.indexOf('.ep {'), css.indexOf('}', css.indexOf('.ep {')))
    expect(ep, '.ep 又变回不让位了').not.toContain('flex: none')
    expect(ep).toMatch(/flex:\s*0\s+1/)
    // 缩到看不见也不行：那几个字是 `ep01 · …`，点开才是系统选单
    expect(ep).toMatch(/min-width:\s*\d/)
  })

  it('项目名在窄屏上让位（导航优先）', () => {
    expect(narrowBlock()).toMatch(/\.proj\s*\{\s*display:\s*none/)
  })

  it('换页之后要把当前那一步滚进视野', () => {
    expect(APP).toContain('function revealStep()')
    expect(APP).toMatch(/watch\(\(\) => route\.path,[\s\S]{0,80}revealStep/)
    // 窗口改大小也要跟一次：横竖屏一转，能摆下的格数就变了
    expect(APP).toMatch(/addEventListener\('resize',\s*revealStep\)/)
    expect(APP).toMatch(/removeEventListener\('resize',\s*revealStep\)/)
  })

  it('用的是 scrollLeft，不是 scrollIntoView', () => {
    // scrollIntoView 会把外层每一个可滚动祖先一起挪，页面主体跟着跳一下
    const at = APP.indexOf('function revealStep()')
    const body = APP.slice(at, at + 700)
    expect(body).toContain('scrollLeft')
    expect(body).not.toContain('scrollIntoView')
  })
})

describe('流程读不出来的时候，导航别装作"一步都没做"', () => {
  /**
   * `session.done` 是 `flow.done ?? {}`。读砸了之后它是个空对象——而空对象
   * 和"真的一步都没做"在导航上长得一模一样：一个对勾都没有，那个「下一步」
   * 的点落回第一步。一部写完了故事、出完了设定的剧，在引擎抽一下的时候
   * 看着像刚建。
   *
   * 「读不出来」和「还没有」是两回事，这条规矩仓库里写了好几处
   * （EpShots、StoryView），导航上漏了。
   */
  const body = code(APP)

  it('有一个"不知道"的判据，而且只认什么都没有那一种', () => {
    expect(body).toMatch(/const stepsUnknown = computed/)
    const at = body.indexOf('const stepsUnknown = computed')
    const line = body.slice(at, at + 160)
    expect(line).toContain('session.failed')
    // 5xx 之后上一份 flow 是留着的，那时候导航说的是实情，不该压暗
    expect(line, '没挡住"还留着上一份"那种').toContain('!session.flow')
  })

  it('不知道做到哪儿了就不指路', () => {
    const at = body.indexOf('const nextKey = computed')
    expect(at).toBeGreaterThan(0)
    const fn = body.slice(at, at + 300)
    expect(fn, 'nextKey 还是会指回第一步').toContain('stepsUnknown')
  })

  it('整条压暗，但链接照样能点', () => {
    expect(body).toContain("'nav--unknown': stepsUnknown")
    // 压暗，不是藏起来：人正要靠这几个链接去看个究竟
    const at = body.indexOf('.nav--unknown')
    expect(at, '.nav--unknown 没有样式').toBeGreaterThan(0)
    const rule = body.slice(at, at + 80)
    expect(rule).toMatch(/opacity/)
    expect(rule).not.toMatch(/display:\s*none|visibility:\s*hidden|pointer-events/)
  })
})
