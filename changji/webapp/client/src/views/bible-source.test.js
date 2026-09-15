/**
 * 新项目上，「照故事定妆」那颗按钮**按不动**。
 *
 * 定妆要么读故事、要么读某一集的剧本——引擎 `post_bible` 的 `source=auto`
 * 就是这个顺序，两样都没有时它抛
 *
 *     400 "还没有剧本，先去写一集"
 *
 * 而新项目缺的是**故事**。那句话把人支去一个更靠后的步骤（剧本是分完集
 * 才有的），而这一页自己的空状态说的是「先写故事，再点右上角『照故事
 * 定妆』」。**两句话对不上，按钮又按得动**，人就在两头之间来回跑。
 *
 * 真引擎上量过（新建一个空项目）：那颗按钮是亮的、按下去 400。
 *
 * 判据必须和引擎那条一模一样——只看"有没有故事"的话，老项目（只有剧本
 * 没有故事）会被误挡住，而那种项目定妆是跑得通的。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const VIEW = fs.readFileSync(
  fileURLToPath(new URL('./AssetsView.vue', import.meta.url)),
  'utf8',
)
/** 引擎那一头。**跨文件比**：它换了判据这边会悄悄失准。 */
const ENGINE = fs.readFileSync(
  fileURLToPath(new URL('../../../../cpp/src/http/planning.cpp', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const body = code(VIEW)

describe('照故事定妆：没料就按不动', () => {
  it('有一个"有没有料"的判据', () => {
    expect(body).toMatch(/const hasSource = computed/)
  })

  it('两样都认：故事，或者写好的剧本', () => {
    const at = body.indexOf('const hasSource = computed')
    const fn = body.slice(at, at + 220)
    expect(fn, '没认故事').toContain('session.done.story')
    expect(fn, '只认故事的话，老项目（只有剧本）会被误挡').toContain(
      'writtenEpisodes',
    )
  })

  it('按钮挂上它，而且那句悬停要说清缺的是什么', () => {
    const at = body.indexOf('@click="bible"')
    expect(at).toBeGreaterThan(0)
    const btn = body.slice(body.lastIndexOf('<button', at), at)
    expect(btn, '没挂上判据').toContain('!hasSource')
    expect(btn, '悬停没说缺什么').toMatch(/还没有故事/)
  })

  it('引擎那头还是这两条路，判据没有失准', () => {
    // auto：有故事走故事，没有就退回"第一集有内容的剧本"，都没有才 400。
    expect(ENGINE, 'post_bible 挪走了？').toContain('ApiResult post_bible')
    const fn = ENGINE.slice(
      ENGINE.indexOf('ApiResult post_bible'),
      ENGINE.indexOf('ApiResult post_bible') + 3000,
    )
    expect(fn, '不再退回剧本那条了？那这边的判据要跟着收紧').toMatch(
      /还没有剧本，先去写一集/,
    )
    expect(fn, 'source=auto 那条分支变了？').toMatch(/source\s*!=\s*"script"/)
  })
})
