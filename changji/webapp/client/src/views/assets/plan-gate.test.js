/**
 * 「落成剧集」：没有分集表就按不动。
 *
 * 它落的是**分集表**（`story.plan`），而分集表是写大纲那一步出的。空着的
 * 时候按下去引擎回
 *
 *     "还没有分集表。先写一份大纲，或者改一下每集时长重算一次"
 *
 * 而那一刻这一格左边那行字正写着「N 章 → **0 集**」——**页面自己已经说了
 * 没有，按钮却还亮着**。真引擎上量过：新写了两章、还没大纲，那颗按钮是
 * 亮的，按下去就是这句红字。
 *
 * 旁边「批量补分镜」判的就是同一个 `plan.length`（它用 v-if 整个藏起来），
 * 也就是说这条规矩这一格里本来就立着，只有主按钮漏了。
 *
 * 这颗改成**灰着**而不是藏起来：它是这一格的主按钮，藏了人会不知道有这
 * 一步。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const VIEW = fs.readFileSync(
  fileURLToPath(new URL('./AssetEpisodes.vue', import.meta.url)),
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

/** 取「落成剧集」那颗按钮。 */
const btn = (() => {
  const at = body.indexOf('@click="makeEpisodes"')
  expect(at, 'makeEpisodes 那颗按钮挪走了？').toBeGreaterThan(0)
  return body.slice(body.lastIndexOf('<button', at), at)
})()

describe('落成剧集', () => {
  it('没有分集表就禁用', () => {
    expect(btn).toMatch(/!plan\.length/)
  })

  it('禁用时那句悬停要说清缺的是什么、去哪儿补', () => {
    expect(btn, '没说缺分集表').toMatch(/还没有分集表/)
    expect(btn, '没说去哪儿补').toMatch(/大纲/)
  })

  it('是灰着不是藏起来——它是这一格的主按钮', () => {
    // 藏了的话，人不知道还有"落成剧集"这一步。
    expect(btn, '被 v-if 藏掉了').not.toMatch(/v-if="[^"]*plan\.length/)
    expect(btn).toMatch(/v-if="hasStory"/)
  })

  it('旁边「批量补分镜」判的是同一个东西', () => {
    // 这条规矩这一格里本来就立着，别只修一半。
    const at = body.indexOf('@click="planAll"')
    const sib = body.slice(body.lastIndexOf('<button', at), at)
    expect(sib).toMatch(/plan\.length/)
  })
})
