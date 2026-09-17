/**
 * 一键出图：**一次交一整批，队列在引擎那头**。
 *
 * 这一圈的历史值得留着，因为它是"同一件事该记在哪儿"的一个标本：
 *
 *   1. 最早是 `for` 里一张一张 `await`——池里两个位置只喂一个，一台双卡机
 *      从头到尾只有一张卡在动（用户 2026-09-17：「一键出图也没用多 gpu」）。
 *   2. 改成页面自己问位置数、自己开几条道。卡是用上了，可**排队这件事没
 *      人记**：页面手里只有"正在画的那几张"，说不出还排着几张；两条道恰好
 *      在同一个人身上时那一行显示成「唐海、唐海」（用户：「光作业中还显示
 *      同一个名字，排队被你吃了？」）。关掉页面队列还整个散掉。
 *   3. 现在队列在引擎（`cpp/src/http/ref_gen.cpp` 的 `RefQueue`）：页面
 *      一次 POST 交一整批，剩下的只管画它收到的那一份。
 *
 * 这份用例钉的就是第 3 条**别退回第 2 条**：页面不许再自己开道、自己数。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const raw = fs.readFileSync(
  fileURLToPath(new URL('../AssetsView.vue', import.meta.url)),
  'utf8',
)
const code = raw
  .replace(/<!--[\s\S]*?-->/g, '')
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/^\s*\/\/.*$/gm, '')

describe('一键出图把整批交给引擎', () => {
  it('一次 POST 交一整批', () => {
    expect(code, '没调那个整批接口').toContain('api.generateAllReferences(')
    const at = code.indexOf('async function genAll()')
    expect(at, '找不到 genAll').toBeGreaterThan(-1)
    const fn = code.slice(at, code.indexOf('\n}\n', at))
    expect(fn, '整批那一 POST 不在 genAll 里').toContain('generateAllReferences')
  })

  it('页面不再自己开道、自己数', () => {
    // 这三样是第 2 版的痕迹。**留一样都会退回去**：只要页面还自己开道，
    // 排队数就又变成它算的，而它算不出还没派出去的那几张。
    expect(code, '又自己问位置数了').not.toContain('frameLanes')
    expect(code, '又自己开并发了').not.toContain('Promise.all(')
    expect(code, '又自己一张一张发了').not.toContain('api.generateReference(')
  })

  it('正在画的和还排着的都照引擎报的画', () => {
    // **「派出去了」不等于「正在画」**：池开的路数比位置数多（跨机的每台
    // 再加一路），双卡机四路两卡，照实报四张"在画"就又回到"看着像四张在
    // 动而实际两张"。真在画的自己会报进度（live[target]），拿它分。
    expect(code, '没订引擎那份队列').toContain('queue: refQueue')
    expect(code, '没拿进度分开"在画"和"等位置"').toMatch(/drawing\s*=\s*all\.filter/)
    expect(code, '还排着几张不是引擎说了算').toMatch(/waiting:\s*Math\.max/)
  })

  it('停下、以及停下之后那一行改口', () => {
    expect(code).toContain('api.stopAllReferences(')
    // 队列是每结一件才播一份，而一件几十秒——按完「停下」那一行要当场
    // 改口，不然看着像没按上。
    expect(raw, '按完停没改口').toContain('bulk.stopping')
  })
})
