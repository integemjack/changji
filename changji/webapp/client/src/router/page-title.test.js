/**
 * 标签页上那个名字，**和屏幕上那一页得是同一页**。
 *
 * 站内换页现在拦得下来了（剧本页那篇没采用的稿子、设定页那条没存的预告片，
 * `onBeforeRouteLeave` 里问一句「确定？」）。人点取消之后 `afterEach` 照样
 * 会跑一趟，只是多带一个"这次没走成"——不认它的话，标签页上写着「故事」
 * 而屏幕上还是「这一集」。
 *
 * 这条不报错、不崩，只是名字一直错着，所以钉一下。
 */
import { describe, expect, it } from 'vitest'

import { applyTitle, titleOf } from './page-title'

const doc = () => ({ title: '这一集 · 场记' })

describe('applyTitle', () => {
  it('走成了就按新那一页写', () => {
    const d = doc()
    applyTitle({ meta: { title: '故事' } }, undefined, d)
    expect(d.title).toBe('故事 · 场记')
  })

  it('没有 title 的那几条路只留品牌名', () => {
    const d = doc()
    applyTitle({ meta: {} }, undefined, d)
    expect(d.title).toBe('场记')
    expect(titleOf(undefined)).toBe('场记')
  })

  it('**被拦下来的那一次一个字都不能动**', () => {
    const d = doc()
    // vue-router 的 NavigationFailureType.aborted：守卫返回了 false
    applyTitle({ meta: { title: '故事' } }, { type: 4 }, d)
    expect(d.title).toBe('这一集 · 场记')
  })
})

describe('步骤名直接来自路由表，中间没有翻译层', () => {
  // 2026-09-16 这儿一度有两条，钉的是「章模式下 chapterWord 把『这一集』
  // 换成『这一章』」。那一层当天删了：集模式整个没了，名字在路由表里写成
  // 什么就是什么（router/index.js 里 title: '这一章'）。
  //
  // **留这一条**是因为翻译层栽过两次，都栽在"两处不同源"上：先是导航栏过
  // 了翻译、标签页没过；补上之后翻译层要发请求问"章模式开没开"，引擎重启
  // 那几秒请求被拒就兜底成老叫法。钉住"照搬 meta.title"，两种飘法都回不来。
  it('照搬 meta.title，一个字不改', () => {
    expect(titleOf({ meta: { title: '这一章' } })).toBe('这一章 · 场记')
    expect(titleOf({ meta: { title: '章节' } })).toBe('章节 · 场记')
    expect(titleOf({ meta: { title: '故事' } })).toBe('故事 · 场记')
  })
})
