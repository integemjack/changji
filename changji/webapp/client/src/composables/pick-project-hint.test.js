/**
 * 「还没选项目」底下那句话，**得是句办得到的话**。
 *
 * 四页各写各的：项目页写的是「…或者点栏头的加号建一个」，而故事 / 设定 /
 * 这一集只写了「在项目库里点一个」。一台刚装好的机器上项目库是空的，
 * 这三页却在叫人去那儿点一个——而初始化那一页 2026-09-14 删了，"第一次
 * 打开该干什么"现在全靠这句话。
 *
 * 底下还顺带钉住同一族的另一句：「还没选到某一集」下面那半句。一集都没有
 * 的时候「顶上挑一集」同样办不到——那时候顶栏那个下拉是灰的，里面只有一行
 * 「还没有剧集」。新项目最常撞见的就是这个状态（故事写完、还没落成剧集）。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

import { pickProjectHint } from './pick-project-hint'

describe('pickProjectHint', () => {
  it('有项目：叫人去点一个', () => {
    expect(pickProjectHint({ loaded: true, count: 3 })).toContain('点一个')
    expect(pickProjectHint({ loaded: true, count: 3 })).not.toContain('建一个')
  })

  it('一个都没有：叫人建一个', () => {
    const s = pickProjectHint({ loaded: true, count: 0 })
    expect(s).toContain('建一个')
    expect(s).toContain('加号')
  })

  it('还没读回来：别抢着说"一个都没有"', () => {
    // 读的那一下 count 天然是 0，照着它说话会先闪一句错的
    expect(pickProjectHint({ loaded: false, count: 0 })).not.toContain('一个都没有')
    expect(pickProjectHint(null)).toContain('点一个')
    expect(pickProjectHint(undefined)).toContain('点一个')
  })
})

describe('四页都从这一处取', () => {
  const SRC = fileURLToPath(new URL('..', import.meta.url))
  const views = ['ProjectView.vue', 'StoryView.vue', 'AssetsView.vue', 'EpisodeView.vue']

  it('没有哪一页再写死那半句', () => {
    for (const v of views) {
      const src = fs.readFileSync(path.join(SRC, 'views', v), 'utf8')
      const at = src.indexOf('title="还没选项目"')
      expect(at, `${v} 里那一屏不见了`).toBeGreaterThan(0)
      const near = src.slice(at, at + 200)
      expect(near, `${v} 还写死着提示语`).toContain('pickProjectHint')
    }
  })
})

describe('「还没选到某一集」底下那句', () => {
  const SRC = fileURLToPath(new URL('..', import.meta.url))

  it('一集都没有时不能说"顶上挑一集"', () => {
    const src = fs.readFileSync(path.join(SRC, 'views', 'EpisodeView.vue'), 'utf8')
    const at = src.indexOf('title="还没选到某一集"')
    expect(at, '那一屏不见了').toBeGreaterThan(0)
    const near = src.slice(at, at + 260)
    // 判据得是"这部剧有没有集"，不是写死一句
    expect(near).toContain('session.episodes.length')
    expect(near).toContain('顶上挑一集')
    expect(near).toMatch(/一集都没有/)
  })
})
