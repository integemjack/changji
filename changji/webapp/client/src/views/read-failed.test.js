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
      ['照故事定妆', "overwrite ? '重新定妆' : '照故事定妆'"],
      ['一键出图', "overwrite ? '全部重画' : '一键出图'"],
    ]) {
      const at = view.indexOf(mark)
      expect(at, `${label} 那颗按钮不见了`).toBeGreaterThan(0)
      const near = view.slice(Math.max(0, at - 1200), at)
      expect(near, `${label} 没有认 assetsError`).toContain('assetsError')
    }
  })

  it('读砸了的时候「定妆」不能还摆成新项目那颗主按钮', () => {
    expect(view).toContain("characters.length || assetsError ? 'btn--ghost' : 'btn--ai'")
  })
})

describe('镜头页和故事页：那一屏的先后不能反', () => {
  it('镜头页：先「读不到」，后「还没有」', () => {
    const view = read('views/episode/EpShots.vue')
    const bad = view.indexOf('title="读不到这一集的分镜"')
    const empty = view.indexOf('title="还没有分镜"')
    expect(bad).toBeGreaterThan(0)
    expect(empty).toBeGreaterThan(0)
    expect(bad).toBeLessThan(empty)
  })

  it('故事页：先「读不到」，后「开始写」那一屏', () => {
    const view = read('views/StoryView.vue')
    const bad = view.indexOf('title="读不到这部剧的故事"')
    const empty = view.indexOf('loadError')
    expect(bad).toBeGreaterThan(0)
    expect(empty).toBeGreaterThan(0)
    // 那一屏挂的就是 loadError，而且在「开始写」那一屏之前
    // （「从这儿开始」这几个字在注释里也出现，所以认模板里那个三元）
    const start = view.indexOf("hasStory ? '这本书' : '从这儿开始'")
    expect(start).toBeGreaterThan(0)
    expect(bad).toBeLessThan(start)
  })
})
