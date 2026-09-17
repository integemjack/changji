/**
 * 设定的「章节」那一格里**不许再有集的概念**。
 *
 * 用户 2026-09-16：「章节只是梳理人物和场景的关系，这个页面不需要集的
 * 概念了。」后来又定了「落成剧集」自动做、不要按钮。
 *
 * 这一格原来摆着三样和集有关的东西，现在一样都不该在：
 *   · 「每集 30/60/90/120/180 秒」下拉——按一下就重新分集；
 *   · 「落成剧集」按钮——现在由 sync_episodes_to_chapters 自动对齐；
 *   · 「批量补分镜」按钮——生产上的事，搬回「这一章」那一页了。
 *
 * 这条用例是防回退的：这几样最容易在后面某次改动里被顺手加回来，
 * 而加回来之后页面又变成"在设定里定集数"。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const read = (name) =>
  fs.readFileSync(fileURLToPath(new URL(name, import.meta.url)), 'utf8')

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const chapters = code(read('./AssetEpisodes.vue'))

describe('章节那一格没有集的概念', () => {
  it('没有「落成剧集」那颗按钮，也没有它的处理函数', () => {
    expect(chapters).not.toContain('makeEpisodes')
    expect(chapters).not.toContain('落成剧集')
  })

  it('没有「批量补分镜」——那是生产上的事，在「这一章」那一页', () => {
    expect(chapters).not.toContain('planAll')
    expect(chapters).not.toContain('批量补分镜')
  })

  it('没有「每集几秒」那个下拉：切集是拍完之后按每集时长切的', () => {
    expect(chapters).not.toContain('pickDuration')
    expect(chapters).not.toContain('DURATIONS')
    expect(chapters).not.toContain('每集')
  })

  it('也不再预告集数（「N 章 → M 集」那行）', () => {
    expect(chapters).not.toContain('→ ')
    expect(chapters).not.toContain('集停在悬念上')
  })

  it('章节行上摆的是人和地方——这一格存在的理由', () => {
    expect(chapters).toContain('chapterFaces')
    expect(chapters).toContain('chapterScenes')
  })
})

describe('补全项目分镜这件事在「这一章」那一页', () => {
  const shots = code(read('../episode/EpShots.vue'))
  it('处理函数在那边', () => {
    expect(shots).toContain('planAll')
  })
  it('不是常驻的第三颗按钮，是主按钮做完这一章之后变身', () => {
    // 2026-09-17：原来「批量补分镜」和「AI 出分镜」并排常驻，而出片那边
    // 同一件事是主按钮自己变身。同一件事两套规矩，动作条上常年五颗按钮。
    // 现在两边同一条规矩，见 EpShots.vue 的 planGoAll。
    expect(shots).not.toContain('批量补分镜')
    expect(shots).toContain('planGoAll')
    expect(shots).toContain('全项目补分镜')
  })
})
