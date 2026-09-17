/**
 * 「还不能开工」的时候，那两颗灰按钮得说得出为什么。
 *
 * `blocked` 一真就把「只出首帧」「出片」关掉，而原来：
 *
 *   · 「只出首帧」灰着时悬停还在说「先把缺的首帧铺开，不出视频」——那是
 *     它**能干什么**，不是它**为什么不能干**；
 *   · 「出片」干脆一个 title 都没有。
 *
 * 人把鼠标停在一颗灰按钮上，问的就是"为什么"。原因确实写在底下那一块
 * （`failedChecks` / `bareShots`），但按钮自己是哑的——而它才是被点的那个。
 *
 * 真引擎上量过（CHANGJI_SD=OFF 又没装 ffmpeg）：两颗都灰着，悬停一句有用
 * 的都没有。补上之后是「还不能开工：FFmpeg · 未找到」。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fs.readFileSync(
  fileURLToPath(new URL('./EpShots.vue', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const body = code(SRC)

/** 取某颗按钮的那一段模板。 */
function btn(clickAttr) {
  const at = body.indexOf(clickAttr)
  expect(at, `${clickAttr} 那颗按钮挪走了？`).toBeGreaterThan(0)
  return body.slice(body.lastIndexOf('<button', at), at)
}

describe('按不动的时候说为什么', () => {
  it('有一句专门的话，而且只在真按不动时才出现', () => {
    expect(body).toMatch(/const blockedWhy = computed/)
    const at = body.indexOf('const blockedWhy = computed')
    const fn = body.slice(at, at + 520)
    expect(fn, '没挡住"能开工"那一支').toMatch(/if \(!blocked\.value\) return ''/)
  })

  it('两个来源都要能说出来：体检挂了、和拿不到参考图那几镜', () => {
    const at = body.indexOf('const blockedWhy = computed')
    const fn = body.slice(at, at + 520)
    expect(fn, '没认参考图那一路').toContain('bareShots')
    expect(fn, '没认体检那一路').toContain('failedChecks')
  })

  it('「只出首帧」：灰着时先说为什么，能点时还是原来那句', () => {
    const f = btn('@click="startFrames"')
    expect(f).toMatch(/blockedWhy \|\|/)
    expect(f, '能点时那句说明不能丢').toMatch(/先把缺的首帧铺开/)
  })

  it('「出片」：原来一个 title 都没有', () => {
    const f = btn('<AppIcon :name="isBusy(\'whole\') ? \'pause\' : \'film\'"')
    expect(f).toMatch(/:title=/)
    // **钉的是"那句话在 title 里"，不是它怎么拼的。** 2026-09-17 这颗按钮
    // 多了一支（只能写文的机器上先拆分镜），于是写法从 `blockedWhy ||`
    // 变成了三元包着它——意图一点没变，而原来的断言按字面比，挂了。
    expect(f, '灰着的时候说不出为什么').toContain('blockedWhy')
  })

  it('只能写文的机器上，这颗按钮不该整个灰掉', () => {
    // 缺一个配音模型把拆分镜也一起拦了，是 2026-09-17 在这台 Mac 上撞见的：
    // 三颗按钮全灰，而这台机器明明能写文、能拆镜头。
    const f = btn('<AppIcon :name="isBusy(\'whole\') ? \'pause\' : \'film\'"')
    expect(f, '还是按 blocked 一刀切').toContain('textOnlyTodo')
    expect(body).toMatch(/const textOnlyTodo = computed/)
    // 亮着的时候要说清两件事：这台机器出不了片、这一颗只干前半截。
    expect(f, '没说这台机器出不了片').toContain('还出不了片')
    expect(f, '没说这一颗只拆分镜').toContain('章分镜拆完')
  })
})
