/**
 * 窄屏上摆不下的那几排，**不能把东西挤到屏幕外面去**。
 *
 * 375×812 上逐页量出来的（`.main` 右边缘 324px）：
 *
 *     故事页   「…」那颗（重写整章 / 从光标处朗读 / 删这一章的唯一入口）
 *              整个落在 343–371，在容器外；「让 AI 写这一章」也被切掉 11px
 *     设定页   「一键出图」落在 342–437，**整颗按钮在屏幕外**——而它正是
 *              这一页的主动作
 *     设置页   「写回配置文件」那个勾落在 332–346，屏幕外；而它管着下面
 *              每一颗保存
 *
 * 三处的毛病是同一种：一排 `nowrap` 的东西摆不下，溢出的那部分被外层那个
 * `overflow-x: auto` 吞掉——页面能横着推，但屏幕上什么都不说，而按钮在
 * 侧边那条栏底下。**这条不报错、不崩，只是那颗按钮按不到。**
 *
 * 三条规矩各在各的文件里，一条 CSS 就能删掉，所以钉一下。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const read = (rel) => fs.readFileSync(fileURLToPath(new URL(rel, import.meta.url)), 'utf8')

/** 取一条规则的正文。 */
function rule(css, selector) {
  const at = css.indexOf(selector + ' {')
  expect(at, `${selector} 不见了`).toBeGreaterThan(0)
  return css.slice(at, css.indexOf('}', at))
}

describe('窄屏上摆不下的那几排', () => {
  it('标签排摆不下就换行——右边挂着那一页的主按钮', () => {
    // 设定页是「照故事定妆」「一键出图」，这一集是「AI 重出分镜」那几个
    expect(rule(read('./styles/base.css'), '.tabs')).toMatch(/flex-wrap:\s*wrap/)
  })

  it('故事页那一排里，先让章号让位，不是让按钮溢出', () => {
    // <select> 的固有宽度按最长那个选项算，flex 子项又默认 min-width: auto
    const css = rule(read('./views/StoryView.vue'), '.doc__pick')
    expect(css).toMatch(/min-width:\s*0/)
    expect(css).toMatch(/flex:\s*0\s+1/)
  })

  it('设置页那一排换行，不藏在一条不画滚动条的横滚里', () => {
    const view = read('./views/SettingsView.vue')
    const at = view.indexOf('@media (max-width: 900px)')
    expect(at).toBeGreaterThan(0)
    const narrow = view.slice(at, view.indexOf('</style>', at))
    const secnav = rule(narrow, '  .secnav')
    expect(secnav).toMatch(/flex-wrap:\s*wrap/)
    // 横滚 + 不画滚动条 = 那个勾在屏幕外，而且没有任何东西说这排能滑
    expect(secnav).not.toMatch(/overflow-x:\s*auto/)
  })
})
