/**
 * 设置页不重复画「能产什么」——同一页下面那张机器表画的就是它。
 *
 * 体检里那条是一段文字（「写文、装配 / 配音：[models].tts 没配…」），
 * 而机器表本机那一行是同样五格，干不了的画一道短横、悬停给的是**同一句**
 * why（引擎一处算的，node_json.cpp 原样带过来），还多了别的机器、还能点。
 *
 * 同一件事在一屏里用两种说法讲两遍，人得先分辨这两处是不是一回事。
 * 这条用例防的是"后面某次改动顺手把它加回来"。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const code = fs
  .readFileSync(fileURLToPath(new URL('./SettingsView.vue', import.meta.url)), 'utf8')
  .replace(/<!--[\s\S]*?-->/g, '')
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/^\s*\/\/.*$/gm, '')

describe('设置页不重复画「能产什么」', () => {
  it('两个 checks 列表都把它滤掉', () => {
    expect(code).toContain("kShownInNodeTable = '能产什么'")
    const failed = code.slice(code.indexOf('const failedChecks'), code.indexOf('const okChecks'))
    const ok = code.slice(code.indexOf('const okChecks'), code.indexOf('const okChecks') + 260)
    expect(failed, '失败那列没滤').toContain('kShownInNodeTable')
    expect(ok, '通过那列没滤').toContain('kShownInNodeTable')
  })

  it('机器表还在这一页上——滤掉的那条得有地方看', () => {
    expect(code).toContain('NodeMatrix')
  })
})
