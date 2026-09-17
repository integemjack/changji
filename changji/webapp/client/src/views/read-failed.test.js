/**
 * 「读不出来」和「还没有」是两回事，而屏幕上长得一模一样。
 *
 * 这条规矩镜头页和故事页都写下来过：
 *
 *   「**读砸了不能摆"还没有"那一屏。** 那一屏说的是"这一集还没分镜"，而读
 *     砸的时候到底有没有根本不知道——按下「AI 出分镜」就是拿一份新的盖掉
 *     可能还在的那份。」
 *
 * 设定页漏了这一条：`loadAssets` 的 catch 是吞掉的，理由写的是「刚建的项目
 * 还没有资产库」——而那句话早就不成立了，`ProjectStore::load_assets` 见文件
 * 不在**回一份空的**，200。走到 catch 的只剩真读砸了（文件在但坏了，引擎回
 * 400 带着行号）。吞掉之后 `characters` 空着，「照故事定妆」就摆成一颗带
 * 星星的主按钮，和刚建好的新项目一模一样——而定妆是往库里写的。
 *
 * 三页一起钉：错的那一屏要排在"还没有"前面，设定页那两颗按钮要认那句报错。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fileURLToPath(new URL('..', import.meta.url))
const read = (rel) => fs.readFileSync(path.join(SRC, rel), 'utf8')

describe('设定页：读不出来就别往里写', () => {
  const view = read('views/AssetsView.vue')

  it('那一趟读砸了要记下来，不是吞掉', () => {
    const at = view.indexOf('async function loadAssets()')
    expect(at).toBeGreaterThan(0)
    const body = view.slice(at, at + 1600)
    expect(body).toContain('assetsError.value =')
    // 裸 catch 就是当年那个吞法
    expect(body).not.toMatch(/\}\s*catch\s*\{\s*\n\s*\/\/[^\n]*\n\s*assets\.value = null\s*\n\s*\}/)
    // 过期那一趟的报错不算数
    expect(body).toContain('if (want !== session.projectPath) return')
  })

  it('两颗主按钮都要认它', () => {
    // 认模板里那两行**按钮文字**（同样的字在注释里出现过好几次，所以要
    // 连着那个三元一起找），再往前看它自己那一段属性。
    for (const [label, mark] of [
      ['理解故事', "overwrite ? '重新理解' : '理解故事'"],
      ['出图', "overwrite ? '全部重画' : '出图'"],
    ]) {
      const at = view.indexOf(mark)
      expect(at, `${label} 那颗按钮不见了`).toBeGreaterThan(0)
      const near = view.slice(Math.max(0, at - 1200), at)
      expect(near, `${label} 没有认 assetsError`).toContain('assetsError')
    }
  })

  it('读砸了的时候「理解故事」不能还摆成新项目那颗主按钮', () => {
    expect(view).toContain("characters.length || assetsError ? 'btn--ghost' : 'btn--ai'")
  })
})

describe('镜头页和故事页：那一屏的先后不能反', () => {
  it('镜头页：先「读不到」，后「还没有」', () => {
    const view = read('views/episode/EpShots.vue')
    const bad = view.indexOf('title="读不到这一章的分镜"')
    const empty = view.indexOf('title="还没有分镜"')
    expect(bad).toBeGreaterThan(0)
    expect(empty).toBeGreaterThan(0)
    expect(bad).toBeLessThan(empty)
  })

  it('故事页：先「读不到」，后稿纸', () => {
    const view = read('views/StoryView.vue')
    const bad = view.indexOf('title="读不到这部剧的故事"')
    const empty = view.indexOf('loadError')
    expect(bad).toBeGreaterThan(0)
    expect(empty).toBeGreaterThan(0)
    // 那一屏挂的就是 loadError，而且在稿纸（一章）之前——2026-09-17 起进来
    // 就是稿纸，一章都没有时 load() 会先建一章空的；读砸了不能去建。
    const paper = view.indexOf('class="doc doc--chapter"')
    expect(paper).toBeGreaterThan(0)
    expect(bad).toBeLessThan(paper)
  })
})

/**
 * 开工前那趟预检（`/api/run/preview`）读砸了。
 *
 * 它原来是吞掉的，注释写着「这一行是锦上添花」——那时候它确实只带一个
 * 时间估算。现在它还带着 `shots_without_refs`，也就是"这一集能不能开工"的
 * 一半判据：读不到的时候 `blocked` 静悄悄变成假，两颗开跑的按钮照常亮着，
 * 屏幕上一个字不说。
 *
 * **但不该跟着关按钮**（体检那条是关的，理由不一样）：预检没查成不等于
 * 不能跑，`post_run` 按下去会拿同一条规则再查一遍、该 400 照样 400。
 * 代价是一个来回，不是一个钟头。所以这两条都要钉：要说出来，也不要乱关。
 */
describe('镜头页：开工前那趟预检读砸了', () => {
  const view = read('views/episode/EpShots.vue')

  it('要记下来，不是吞掉', () => {
    const at = view.indexOf('async function loadPreview()')
    expect(at).toBeGreaterThan(0)
    const body = view.slice(at, at + 2200)
    expect(body).toContain('previewError.value =')
    // 成功和早退那两支也要清，不然会挂着上一次的报错
    expect(body.match(/previewError\.value = ''/g)?.length ?? 0).toBeGreaterThanOrEqual(2)
  })

  it('那一块要跟着出现', () => {
    expect(view).toContain('v-if="(blocked || previewError) && shots.length"')
    // 标题别写死成「还不能开工」：按钮还亮着，人会去找那颗灰的在哪儿
    expect(view).toMatch(/blocked \? '还不能开工' :/)
  })

  it('**不许**因为它去关开跑那两颗按钮', () => {
    // blocked 才关按钮；预检没查成只是说一句
    const at = view.indexOf('const blocked = computed(')
    expect(at).toBeGreaterThan(0)
    expect(view.slice(at, at + 300)).not.toContain('previewError')
  })

  it('「重新体检」两趟一起重来', () => {
    expect(view).toContain('async function recheck()')
    expect(view).toMatch(/recheck[\s\S]{0,120}loadPreview\(\)/)
    expect(view).toContain('@click="recheck"')
  })
})

/**
 * 项目那一份没读回来的时候，别替它说"还没有"。
 *
 * `session.characters` 是 `project?.characters ?? []`——`/bff/flow` 砸了
 * （引擎在重启、项目 JSON 坏了）它就是空的，而 `hasProject` 看的是路径、
 * 照样为真。镜头页那一行于是写出「还没有角色和场景 · 先去出角色」，
 * 而真正的原因是读不到这个项目。session store 自己那段注释点的就是这件事：
 * 「一个指着用户去做一件做不到的事的提示，真正的原因一个字没有」。
 */
describe('镜头页：项目没读回来时那行「还没有角色和场景」', () => {
  const view = read('views/episode/EpShots.vue')

  it('先看项目那一份在不在', () => {
    const at = view.indexOf('const missingAssets = computed(')
    expect(at).toBeGreaterThan(0)
    const body = view.slice(at, at + 260)
    expect(body).toContain('if (!session.project) return []')
    // 真的空着还是要说
    expect(body).toContain('session.characters.length')
    expect(body).toContain('session.locations.length')
  })
})

/**
 * 成片页读砸了，屏幕上是**两条一模一样的红字**。
 *
 * 那一格被 KeepAlive 冻着，进来时靠 `onActivated(load)` 重拉；而底下那条
 * 换剧换集的 watch 原来还带着 `immediate: true`——Vue 对 KeepAlive 里的
 * 组件初次挂载也会触发 activated，于是两边都跑，进这一格发两遍
 * `/api/outputs`、`ui.error` 也弹两次。那一页自己已经摆着「读不到这部剧的
 * 成片」那一屏了。
 */
describe('成片页：进这一格只该读一遍', () => {
  const view = read('views/episode/EpFilm.vue')

  it('挂载那一趟交给 onActivated，watch 不要再 immediate 一次', () => {
    expect(view).toContain('onActivated(load)')
    const at = view.indexOf('watch(() => [session.projectPath, session.episodeId], load')
    expect(at, '换剧换集那条 watch 不见了').toBeGreaterThan(0)
    expect(view.slice(at, at + 120)).not.toContain('immediate')
  })
})
