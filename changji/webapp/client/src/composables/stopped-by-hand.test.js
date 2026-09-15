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

  it('写正文：先留、后退——两条分支的顺序不能反', () => {
    // 反了的话按停就走进"写砸了"那一支，流出来的字被原样清掉，
    // 而那正是这条用例要挡的事。
    // 最后那一处才是收尾里的；前面还有一处是流式一边收一边画。
    const keep = STORY.lastIndexOf('buf[chapterId] = acc')
    const drop = STORY.indexOf('buf[chapterId] = had')
    expect(keep).toBeGreaterThan(0)
    expect(drop).toBeGreaterThan(0)
    expect(keep).toBeLessThan(drop)
  })

  it('写正文：留下来的那份要存，不然刷一下就没了', () => {
    const tail = STORY.slice(STORY.lastIndexOf('buf[chapterId] = acc'))
    expect(tail.slice(0, 1200)).toContain('scheduleSave(chapterId, 0)')
  })

  it('改一段：留下来的同时要摆底稿，「撤销」才有东西可退', () => {
    const at = STORY.indexOf("ui.info(`停下了，改出来的")
    expect(at).toBeGreaterThan(0)
    const near = STORY.slice(at - 600, at)
    expect(near).toContain('pending.value = {')
    expect(near).toContain('scheduleSave(id, 800)')
  })
})
