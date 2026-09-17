/**
 * 空章写完直接落盘，不用再点一下「采用」。
 *
 * 「采用」这一步存在的理由只有一个：它会**盖掉已经存下的那一版**，而那一版
 * 可能是人一句句改过的。空章上没有任何东西会被盖掉——那一下点击什么也没
 * 保住，纯粹是多一步，一部剧八章就是八下。
 *
 * 这条用例是防回退的：这一步最容易在后面某次改动里被"为了统一"加回来。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const code = fs
  .readFileSync(fileURLToPath(new URL('./EpScript.vue', import.meta.url)), 'utf8')
  .replace(/<!--[\s\S]*?-->/g, '')
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/^\s*\/\/.*$/gm, '')

describe('空章写完直接落盘', () => {
  it('write 末尾在没有存过剧本时调 adopt', () => {
    expect(code).toContain('if (!savedScript.value.trim()) await adopt()')
  })

  it('有剧本的时候不自动落盘——那一下会盖掉人改过的东西', () => {
    // adopt 里那句确认还在：它是"有存过、而且和草稿不一样"时才问的
    expect(code).toContain('const had = savedScript.value.trim()')
    expect(code).toContain('整份换掉')
  })

  it('人在这一趟里换了章就不落盘，只提示一句', () => {
    // 落到别的集上比多点一下严重得多
    expect(code).toContain('切回那一章就能看')
  })
})
