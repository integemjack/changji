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
