/**
 * 引擎回来了，那一页还停在「读不到…」上。
 *
 * **引擎重启是这套东西里最常见的一种断。** 仓库里三处注释都写着「引擎重启
 * 时会连着失败几次」——而那三处说的都是**轮询**那一层，它们自己会慢下来
 * 再接着试。真正卡住的是**页面那一趟读**：进页面时引擎正好没起来，那一页
 * 就停在「读不到这一章的分镜 · 网络请求发不出去」上；两秒之后引擎回来，
 * 顶栏那盏灯自己变回「引擎已连接」、系统表和「AI 作业中」也自己回来了，
 * **唯独那一页的正文一直是那句报错**。人只能刷新，或者切一下章号 / 标签
 * 把它骗回来。
 *
 * 实测过：成片页在引擎回来之后又停了十几秒一动不动，而灯是绿的。
 *
 * 判据用那份系统表在不在（两秒一拍，拉不到时 `stat` 是 null）。
 * **只在那一页真挂着错的时候才重来**——不然每开一页都要白读一趟，因为
 * 第一次拿到表也是一次 null → 非空的跳变。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fileURLToPath(new URL('..', import.meta.url))
const read = (rel) => fs.readFileSync(path.join(SRC, rel), 'utf8')

/** 摆着「读不到…」那一屏的几页。 */
const PAGES = [
  'views/StoryView.vue',
  'views/episode/EpScript.vue',
  'views/episode/EpFilm.vue',
  'composables/useShots.js', // 镜头页那一屏
]

describe('引擎回来之后自己重来一趟', () => {
  it('四页都接上了', () => {
    for (const rel of PAGES) {
      const src = read(rel)
      expect(src, `${rel} 没接 useRetryWhenBack`).toContain('useRetryWhenBack(')
      // 判据是那一页自己的 loadError，不是别的
      expect(src, `${rel} 的判据不对`).toMatch(/useRetryWhenBack\(\(\) => loadError\.value,\s*load\)/)
    }
  })

  it('没挂着错就别白读一趟', () => {
    const feed = read('composables/useSystemFeed.js')
    const at = feed.indexOf('export function useRetryWhenBack(')
    expect(at).toBeGreaterThan(0)
    const body = feed.slice(at, at + 400)
    expect(body).toContain('if (up && hasError()) retry()')
    // 判的是"表在不在"，不是别的
    expect(body).toContain('stat.value != null')
  })
})
