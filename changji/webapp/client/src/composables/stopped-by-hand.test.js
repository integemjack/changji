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

  it('引擎那两句话，这边的正则都得认得', () => {
    // **这一条读的是真的 C++ 源码**，不是把字符串抄一份在用例里——
    // 抄一份的话引擎改了词这儿照样绿，等于没有告警。
    //
    // 2026-09-17 这两条原来分别在 llm/client.cpp 和 http/ref_gen.cpp 上按
    // 调用点的形状抠字面量（`LlmError("…")` / `ApiError(400, "…")`）。那天
    // 把两句话收进了 util/cancel_words.hpp（一处定义、五处引用），调用点
    // 上没有字面量了，这两条当场红——**它们红得对，是这个告警在干活**。
    // 现在读那个头文件，比按调用点形状抠更稳：改文案只会动那一处。
    const src = read('../../../../cpp/src/util/cancel_words.hpp')
    const said = [...src.matchAll(/inline constexpr const char\* k\w+ = "([^"]+)";/g)]
      .map((m) => m[1])
    // kStopToken1/2 是这边正则里那两个词，一并读出来，下面按"是不是完整
    // 句子"分开——两类都得认得。
    expect(said.length).toBeGreaterThanOrEqual(2)
    for (const s of said) expect(stoppedByHand(s)).toBe(true)
  })
})

describe('按停之后留下的那几个字', () => {
  const STORY = read('../views/StoryView.vue')

  // 2026-09-17 之前这儿还钉着「写正文」那条流的三支收尾。「所有让 AI 做的
  // 都只是这一个章的内容」之后 writeChapter 没了，只剩改一段（空章从头写
  // 也走它），下面这一条就是全部。

  it('改一段：留下来的同时要摆底稿，「撤销」才有东西可退', () => {
    const at = STORY.indexOf("ui.info(`停下了，改出来的")
    expect(at).toBeGreaterThan(0)
    const near = STORY.slice(at - 600, at)
    expect(near).toContain('pending.value = {')
    expect(near).toContain('scheduleSave(id, 800)')
  })
})
