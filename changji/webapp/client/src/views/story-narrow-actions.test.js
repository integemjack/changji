/**
 * 窄屏（≤1100px）下，整本书那两件事没有入口。
 *
 * 「展开剩下 N 章」和「提人物」长在左栏底下（`.list__foot`），而窄屏下
 * `.ed__list` **整个不渲染**——媒体查询只把右边的对话栏挪了位置。页面里
 * 早有一段注释记着这个结构（那次只把一颗点了没反应的「章节列表」按钮关
 * 掉了），这两件事却一直没给窝。
 *
 * **后果是整条流水线走不下去**：「提人物」够不着的话人物和地点永远是空的，
 * 设定页空着、拆分镜没人可引用。而「反推」「采用大纲」之后那两句提示还在
 * 说「接着点左边「提人物」」——左边什么都没有。
 *
 * 1100px 不是个偏门尺寸：笔记本半屏、并排开两个窗口就到了。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const VIEW = fs.readFileSync(
  fileURLToPath(new URL('./StoryView.vue', import.meta.url)),
  'utf8',
)

function code(text) {
  return text.replace(/<!--[\s\S]*?-->/g, '')
}
const src = code(VIEW)

describe('窄屏也要够得着整本书那两件事', () => {
  it('左栏在窄屏下确实不渲染——这是前提', () => {
    expect(src).toMatch(/listShown = computed\(\(\) => listOpen\.value && !narrow\.value/)
    expect(src).toMatch(/v-if="listShown && \(hasStory \|\| draft\)"/)
  })

  it('两颗按钮各有两处：左栏底下一处，正文抬头一处', () => {
    expect((src.match(/@click="writeAllChapters"/g) ?? []).length, '展开剩下 N 章只有一处').toBe(2)
    expect((src.match(/@click="analyzeStory"/g) ?? []).length, '提人物只有一处').toBe(2)
  })

  it('抬头那一处只在左栏不在的时候出现，否则同一件事摆两遍', () => {
    expect(src).toMatch(/v-if="!listShown && !draft && !writer\.running"/)
  })

  it('那一行要能折——挤不下就顶出屏幕，等于还是够不着', () => {
    const head = src.slice(src.indexOf('.doc__head {'))
    expect(head.slice(0, 300)).toMatch(/flex-wrap:\s*wrap/)
  })
})
