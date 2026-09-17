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
  it('不是单独一颗按钮，是「跑完整部剧」里的一步', () => {
    // 2026-09-17 两次改动叠起来：先是「批量补分镜」从常驻按钮并进主按钮
    // （同一件事不该有两套规矩），再是用户定「并成一颗，一路跑到底」——
    // 全项目那三件事（改编 → 补分镜 → 出片）现在是主按钮一次点完，
    // 见 EpShots.vue 的 runWholeShow。
    expect(shots).not.toContain('批量补分镜')
    expect(shots).not.toContain('全项目补分镜')
    expect(shots).toContain('runWholeShow')
    expect(shots).toContain('跑完整部剧')
  })
  it('三步一次点完，而且中途按停下就不再往下发', () => {
    const at = shots.indexOf('async function runWholeShow')
    expect(at).toBeGreaterThan(0)
    const fn = shots.slice(at, at + 1800)
    expect(fn, '少了改编那一步').toContain('scriptAll')
    expect(fn, '少了补分镜那一步').toContain('planAll')
    // 前两件事跑在同一个"写"槽上，必须等上一件闲下来再发下一件
    expect(fn, '没等写那个槽').toContain('seriesStatus')
    expect(fn, '没认取消').toContain('cancelled()')
  })
  it('跑起来之后主按钮自己变成「停下」——前两步在这一页本来按不停', () => {
    // 前两步跑在"写"那个槽上，而页面上原来那颗「停下」只在出片那个槽跑着
    // 时才出现。提示里却写着"按停下可以中断"——空头支票。
    expect(shots).toContain('async function stopWholeShow')
    expect(shots, '没停写那个槽').toContain('api.stopSeries()')
    expect(shots, '旗子没立').toContain('wholeAbort')
    // 按下去之后链条不再往下发：cancelled() 认这面旗子
    const at = shots.indexOf('const cancelled = () =>')
    expect(at).toBeGreaterThan(0)
    expect(shots.slice(at, at + 200)).toContain('wholeAbort.value')
  })
  it('按钮上不印为零的那一项', () => {
    // 全项目分镜都齐了的时候，原来按钮上摆着「还差 0 章分镜 · 68 镜出片」
    // ——让人先读一个 0 再自己忽略它。
    const at = shots.indexOf('const todoLabel = computed')
    expect(at).toBeGreaterThan(0)
    const fn = shots.slice(at, at + 320)
    expect(fn).toContain('projectNoShots.value > 0')
    expect(fn).toContain('projectPending.value > 0')
    expect(fn, '两项都有才用「·」连').toContain("join(' · ')")
    // 模板里不再直接拼那两个数
    expect(shots).not.toContain('章分镜 · ${projectPending}')
  })

  it('最后那一步起不来要说一声，不能吞掉', () => {
    // start() 只回 {ok:false, error}，自己不弹框。吞了的话第三步被 400
    // 挡回来时，人看到的是按钮转一圈又变回去，屏幕上一个字没有。
    const at = shots.indexOf('async function runWholeShow')
    const fn = shots.slice(at, at + 2600)
    expect(fn).toContain('出片这一步起不来')
    expect(fn, '409 不是错，别报成错').toContain('409')
  })
})
