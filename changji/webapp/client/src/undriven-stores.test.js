/**
 * 「跑完了重拉一次」那几条下降沿，**不能盯没人驱动的旗子**。
 *
 * `useRun` 的轮询只有镜头格里的 `useShots` 会开，`useWriter` 只有故事页和
 * 设定页「分集」那一格会开。而要跟着"跑完了"刷新的地方在别的页上：
 *
 *     项目库那条栏     卡片上的「1/2 集已出片」
 *     这一集那排标签   「镜头 12 · 差 3 首帧」「成片 1」
 *     设定页那行标签   「分集 10 · 8 章没正文」
 *     成片那一页       片单本身
 *
 * 四处都盯着那两个旗子，于是"跑完了"这件事恰恰在**数字摆在眼前的那几页**
 * 上一次都不会发生。实测过：站在项目页让长跑任务结束，`/api/projects`
 * 一次都没重拉；在设定页上同样，`/api/assets` 一次都没重拉。
 *
 * 全改成读那份系统表（`useLongRunning`）——它在每一页上都两秒一拍地拉。
 * 这条用例挡的是"哪天有人又把它改回去"。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fileURLToPath(new URL('.', import.meta.url))
const read = (rel) => fs.readFileSync(path.join(SRC, rel), 'utf8')

/**
 * 把注释剥掉再比。
 *
 * 这一屋子的注释里到处引用着"当初错的那句代码"（这正是它们的价值），
 * 而 `not.toContain('runner.running')` 这种断言会一头撞上去——写这几条
 * 用例的时候连着被自己的注释绊了三次。要挡的是**代码**，不是那段记录。
 */
const code = (rel) =>
  read(rel)
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/(^|[^:])\/\/.*$/gm, '$1')

/** 那四处下降沿，和各自该认哪几种活。 */
const EDGES = [
  ['components/ProjectRail.vue', undefined],
  ['views/EpisodeView.vue', undefined],
  ['views/AssetsView.vue', undefined],
  // 片子只有出片那个槽会出，写整季跑完不该让这一页白拉一趟
  ['views/episode/EpFilm.vue', "['run']"],
  // 镜头页：在设定页点完「批量补分镜」，人多半直接过来这一页等
  ['composables/useShots.js', "['write']"],
]

/**
 * **`useShots` 里盯 `runStore.running` 那一条是对的，别顺手也改了。**
 * 它自己就是 `useRun` 的驱动者（`start()` / `stop()` 都在它的挂载卸载里），
 * 而且那条回调还要读 `runStore.state?.error` 和那个「自己按的停」的闩——
 * 那些只有 run store 有。
 */

describe('跑完了重拉一次', () => {
  it('四处都从那份系统表认下降沿', () => {
    for (const [rel, kinds] of EDGES) {
      const src = read(rel)
      expect(src, `${rel} 没用 useLongRunning`).toContain('useLongRunning')
      if (kinds) expect(src, `${rel} 该只认 ${kinds}`).toContain(`useLongRunning(${kinds})`)
    }
  })

  it('**不许再盯那两个旗子**', () => {
    for (const [rel] of EDGES) {
      const src = code(rel)
      // 认的是 `runner.` / `writer.` 那两个实例名。`runStore.running` 不算
      // ——useShots 自己就是 run store 的驱动者，见上面那段。
      expect(src, `${rel} 又在 watch 那两个旗子了`).not.toMatch(
        /watch\(\s*\(\) => \[?(runner|writer)\.running/,
      )
    }
  })

  it('「还不知道」不算刚跑完', () => {
    // 表没回来时 useLongRunning 回 null；判据要写死成 true→false
    for (const [rel] of EDGES) {
      const src = read(rel)
      expect(src, `${rel} 的判据太松`).toMatch(/before === true && now === false/)
    }
  })

  it('默认只认长跑那两种，别被出参考图那种短活带着重拉', () => {
    const feed = read('composables/useSystemFeed.js')
    const at = feed.indexOf('export function useLongRunning(')
    expect(at).toBeGreaterThan(0)
    expect(feed.slice(at, at + 260)).toContain("kinds = ['run', 'write']")
    expect(feed.slice(at, at + 400)).toContain('if (!jobs) return null')
  })
})
