/**
 * 「这句话是不是人自己按的停」——判据只有那句话，而那句话在引擎里。
 *
 * **人按的停不是失败。** 引擎那头分得很清（`ref_gen.cpp` 里就写着
 * 「报成「出图失败：已取消」的话，人会去找哪儿出错了」），取消留下的是
 * 一句「已取消」/「已停下这一张」。界面这边拿它分两件事：
 *
 *   · 别再弹一条红的报错（自己按下去的停，屏幕上不该是出错）；
 *   · **按停之前写出来的字要留着**，而写砸了那条要清掉（用户 2026-09-15：
 *     「ai 写文章点击停下来之前写的内容应该保留」）。
 *
 * 也就是说：引擎那边换个说法，这边会**悄悄**退回"红字 + 一个字不剩"——
 * 不报错、不崩，只是那两件事同时错。所以直接读 C++ 源码对一遍。
 * 做法同 speech-rate / upload-formats 那两条。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

import { stoppedByHand } from './stopped-by-hand'

const read = (rel) => fs.readFileSync(fileURLToPath(new URL(rel, import.meta.url)), 'utf8')

describe('stoppedByHand', () => {
  it('认得出引擎留的那两句', () => {
    expect(stoppedByHand('已取消')).toBe(true)
    expect(stoppedByHand('已停下这一张')).toBe(true)
  })

  it('别的都算真砸了', () => {
    expect(stoppedByHand('')).toBe(false)
    expect(stoppedByHand(undefined)).toBe(false)
    expect(stoppedByHand('显存不够加载 LLM')).toBe(false)
    expect(stoppedByHand('大模型没写出能用的正文：少了 body 这一栏')).toBe(false)
  })

  it('大模型那条路取消时抛的就是这句', () => {
    // llm/client.cpp: `if (tok.cancelled()) throw LlmError("已取消");`
    // 它穿过 story_api 的 catch 变成 ApiError(502)，再由 start_async 播成
    // job_error——写大纲 / 写正文 / 改一段 / 写剧本按「停下」走的都是这条。
    const src = read('../../../../cpp/src/llm/client.cpp')
    const said = [...src.matchAll(/LlmError\("([^"]+)"\)/g)].map((m) => m[1])
    const cancels = said.filter((s) => /取消|停下/.test(s))
    expect(cancels.length).toBeGreaterThan(0)
    for (const s of cancels) expect(stoppedByHand(s)).toBe(true)
  })

  it('出参考图那条路取消时回的也认得', () => {
    // ref_gen.cpp: `if (tok.cancelled()) throw ApiError(400, "已停下这一张");`
    const src = read('../../../../cpp/src/http/ref_gen.cpp')
    const m = src.match(/tok\.cancelled\(\)\)\s*throw ApiError\(400,\s*"([^"]+)"\)/)
    expect(m).toBeTruthy()
    expect(stoppedByHand(m[1])).toBe(true)
  })
})

describe('按停之后留下的那几个字', () => {
  const STORY = read('../views/StoryView.vue')

  /**
   * 按停那一支的开头。
   *
   * **按分支名找，别按 `buf[chapterId] = acc` 找。** 那一句在这个文件里
   * 出现三次：流式一边收一边画、按停留下、断线留下（story-lost-link）。
   * 原来这两条用的是 `lastIndexOf`，断线那一支加进来之后就指错了地方
   * ——用例当场红给我看了，好过悄悄测着另一段。
   */
  const BY_HAND = STORY.indexOf(
    'if (byHand && acc.trim()) {',
    STORY.indexOf('async function writeChapter('),
  )

  it('写正文：先留、后退——两条分支的顺序不能反', () => {
    // 反了的话按停就走进后面那几支，而那几支**都不存盘**（理由见它们
    // 各自的注释：引擎那头没落库、字也没过守卫）。按停是确定的，它那一支
    // 存；所以它必须排在最前面。
    //
    // 2026-09-16 这条用例原来钉的是 `// 写砸了：把流出来那半截清掉` 那句
    // 注释，而"写砸了就清掉"这件事本身已经不做了（写砸的字现在也留着，
    // 只是不存）。钉注释文字太脆，改成钉**分支自己的判据**。
    const keep = STORY.indexOf('buf[chapterId] = acc', BY_HAND)
    // 一个字都没流出来那一支：这时候才轮到把原来那份放回去
    const drop = STORY.indexOf(
      "// 一个字都没流出来：放回原来那份",
      BY_HAND,
    )
    expect(BY_HAND).toBeGreaterThan(0)
    expect(keep).toBeGreaterThan(0)
    expect(drop).toBeGreaterThan(0)
    expect(keep).toBeLessThan(drop)
  })

  it('写正文：写砸了也把流出来的字留着，但不存', () => {
    // 2026-09-16 连砸三次都是收尾那一下出的事（一句话复读三遍、一个字段
    // 短几个字、JSON 结尾没闭合），而正文本身一千多字整整齐齐。每砸一次
    // 十分钟，人看着它一个字一个字写完，然后眼睁睁全没了。
    // 用户 2026-09-15 定的规矩：「点停下来之前写的内容应该保留」——
    // 按停和写砸是同一件事的两种触发，字都是同样的字。
    const fail = STORY.indexOf("if (acc.trim()) {", BY_HAND)
    expect(fail).toBeGreaterThan(BY_HAND)
    const tail = STORY.slice(fail, fail + 1400)
    // 留：把流出来那份装回编辑器
    expect(tail).toContain('buf[chapterId] = acc')
    // 但**不存**——引擎那头这一趟明确没落库，而这份没过守卫
    expect(tail).not.toContain('scheduleSave')
    // 原来那份要能退回去
    expect(tail).toContain('pending.value')
  })

  it('写正文：留下来的那份要存，不然刷一下就没了', () => {
    // ⚠️ 只有**按停**这一支该存。断线那一支特意不存（那头可能已经写完
    // 落库了，存半截等于盖掉完整那份），见 story-lost-link.test.js。
    const tail = STORY.slice(BY_HAND, STORY.indexOf('if (lostLink', BY_HAND))
    expect(tail).toContain('scheduleSave(chapterId, 0)')
  })

  it('改一段：留下来的同时要摆底稿，「撤销」才有东西可退', () => {
    const at = STORY.indexOf("ui.info(`停下了，改出来的")
    expect(at).toBeGreaterThan(0)
    const near = STORY.slice(at - 600, at)
    expect(near).toContain('pending.value = {')
    expect(near).toContain('scheduleSave(id, 800)')
  })
})
