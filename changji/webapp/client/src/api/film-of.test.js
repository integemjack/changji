/**
 * 「这个成片文件是不是这一章的」这个判据。
 *
 * **为什么值得单写一条用例。** 它原来是 `name.includes(episodeId)`，
 * 而章号到 99 以内是 `ep%02d`、**第 100 章起变成 `ep100`**——这一支是
 * story_plan.cpp 的 `ep_id` 里刻意写的，不是意外。于是站在 `ep10` 上时
 * ep100～ep109 十个成片文件全算成它的，而一部长篇改编下来七八十上百章是
 * 常事。
 *
 * 坏的方式最难发现：审片页默认选 `forThisEpisode[0]`（按 mtime 倒序），
 * 可能选中 ep107 那条；而「播的是不是这一章」那个守卫用的是同一个判据，
 * 于是它也说"是你的"，一个字都不提示。
 *
 * 引擎那边 `flow.cpp` 的 `film_of` 是同一条规则的 C++ 版——两边分头写的
 * 东西迟早分叉，所以这儿把边界情形一条条钉死。
 */
import { describe, expect, it } from 'vitest'

import { isFilmOf } from './labels.js'

describe('成片文件属于哪一章', () => {
  it('引擎自己出的那份：<章号>.mp4', () => {
    // pipeline/episode.cpp: assemble(timeline, ep.episode_id + ".mp4")
    expect(isFilmOf('ep01.mp4', 'ep01')).toBe(true)
    expect(isFilmOf('ep10.mp4', 'ep10')).toBe(true)
    expect(isFilmOf('trailer.mp4', 'trailer')).toBe(true)
  })

  it('**三位章号不许串到两位上**——修的就是这一条', () => {
    // 第 100 章起 ep_id 回落到 "ep" + n，于是 ep100…ep109 长得像 ep10 开头
    for (const n of [100, 101, 105, 109]) {
      expect(isFilmOf(`ep${n}.mp4`, 'ep10')).toBe(false)
    }
    // 反过来也不许：站在 ep100 上时 ep10 那条不算它的
    expect(isFilmOf('ep10.mp4', 'ep100')).toBe(false)
    // 自己还是要认得出来
    expect(isFilmOf('ep107.mp4', 'ep107')).toBe(true)
  })

  it('人手加的后缀还算这一章', () => {
    // 体检里那条 2K 出路教人 `changji --upscale 成片.mp4 出来的.mp4`，
    // 出来的那份多半就放在同一个目录里
    expect(isFilmOf('ep01_2k.mp4', 'ep01')).toBe(true)
    expect(isFilmOf('ep01-final.mp4', 'ep01')).toBe(true)
    expect(isFilmOf('ep01 (修).mp4', 'ep01')).toBe(true)
    // 前缀同理，中文也算边界
    expect(isFilmOf('导演版_ep01.mp4', 'ep01')).toBe(true)
  })

  it('别的章一律不算', () => {
    expect(isFilmOf('ep02.mp4', 'ep01')).toBe(false)
    expect(isFilmOf('trailer.mp4', 'ep01')).toBe(false)
    expect(isFilmOf('ep011.mp4', 'ep01')).toBe(false)
  })

  it('没选章、名字是空的，一律不算', () => {
    // 空章号返回真的话，片单上每一条都会挂上「这一章」的牌子
    expect(isFilmOf('ep01.mp4', '')).toBe(false)
    expect(isFilmOf('ep01.mp4', null)).toBe(false)
    expect(isFilmOf('ep01.mp4', undefined)).toBe(false)
    expect(isFilmOf(undefined, 'ep01')).toBe(false)
    expect(isFilmOf(null, 'ep01')).toBe(false)
  })

  it('同一个章号在名字里出现两次，有一处对得上就算', () => {
    // 不扫完的话 `ep100_ep10.mp4` 这种会被第一处的失败带跑
    expect(isFilmOf('ep100_ep10.mp4', 'ep10')).toBe(true)
  })
})
