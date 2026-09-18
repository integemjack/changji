/**
 * 成片页：**不知道就说不知道**，不许报 0，也不许拿 files[0] 冒充那部电影。
 *
 * 接口那半边 2026-09-18 已经改成这个口径（`GET /api/film` 读不到清单就回
 * `total_s: null` / `shots: null` / `name: null`，并且多回一个 `legacy_cut`），
 * C++ 那头也有用例钉着。页面这半边当时没跟，于是同一份回包在屏幕上被读回了
 * 两句假话：
 *
 * 1. `Math.round(s ?? 0)` 和 `film.shots ?? 0`——`output/final` 里有 成片.mp4
 *    而没有 film.json（合成被 kill 在写清单之前、老项目的 cut.json 被删过、
 *    ffprobe 没给出时长）时，页面显示「成片 · **0 秒 · 0 镜** · 41.2 MB」。
 *    电影好好的，只是没人量过它——0 是替引擎说的一句它没说过的话。
 * 2. `files[0]`——2026-09-17 切过 6 段的老项目回的是
 *    `legacy_cut: true, name: null, total_s: 300（六段加起来）, files: 六条`，
 *    页面显示「第01集 · 5 分 00 秒 · 12 镜 · <第01集那个 MB>」（盘上就叫
 *    「第01集.mp4」这种名字）：秒数和镜数是六段的，名字和体积是第一段的，
 *    另外五段连入口都没有。更窄的一支是有人往 `output/final` 放了个
 *    raw.mp4——列表按名字排，「成片」的首字节比任何 ASCII 名字都大，
 *    `files[0]` 于是是那个杂文件。
 *
 * 所以这一份钉的是**页面怎么读那份回包**。页面没有挂载着测（这个仓库的前端
 * 用例都是读源码），但 `fmt` 是个纯函数——把它从源码里抠出来真跑一遍，
 * 「null 说成 0 秒」这件事就是当场量出来的，不是用正则猜的。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const HERE = fileURLToPath(new URL('.', import.meta.url))
const src = fs.readFileSync(path.join(HERE, 'FilmView.vue'), 'utf8')
/** 模板那一段。`files[0]` 这种写法要在这儿找——脚本里 `files.value[0]` 是另一回事。 */
const tpl = src.slice(src.indexOf('\n<template>'), src.indexOf('\n<style'))

describe('成片页：没量到的那几栏', () => {
  /** 把 fmt 从源码里抠出来跑。它不依赖任何 import，所以这么跑得起来。 */
  const fmt = new Function(`${src.match(/function fmt\(s\) \{[\s\S]*?\n\}/)[0]}\nreturn fmt`)()

  it('null / undefined 是「—」，不是「0 秒」', () => {
    expect(fmt(null)).toBe('—')
    expect(fmt(undefined)).toBe('—')
  })

  it('真量出来的秒数照常说人话', () => {
    expect(fmt(0)).toBe('0 秒')
    expect(fmt(41)).toBe('41 秒')
    expect(fmt(300)).toBe('5 分 00 秒')
  })

  it('镜数那一栏也是「—」，不是 0 镜', () => {
    expect(tpl).toContain("film.shots ?? '—'")
    expect(tpl).not.toContain('film.shots ?? 0')
  })

  it('每一段自己的时长也走 fmt——老清单里没写的那几段会是 undefined', () => {
    expect(tpl).toContain('fmt(f.duration_s)')
    expect(tpl).toContain("f.shots ?? '—'")
  })
})

describe('成片页：那部电影是 name 那一条', () => {
  it('标题和体积从 name 认出来的那一条来，不是 files[0]', () => {
    expect(src).toContain('const movie = computed(')
    expect(tpl).toContain('stem(movie.name)')
    expect(tpl).toContain('movie.size_mb')
    expect(tpl).not.toContain('files[0]')
  })

  it('播放器优先挂那部电影', () => {
    const at = src.indexOf('watch(film,')
    expect(at, '挂播放器那条 watch 不见了').toBeGreaterThan(0)
    expect(src.slice(at, at + 200)).toContain('movie.value ??')
  })
})

describe('成片页：上一版切出来的那几段', () => {
  it('legacy_cut 有人读，并且说清了这是上一版切的、该重新合成', () => {
    expect(src).toContain('film.legacy_cut')
    const at = tpl.indexOf('film.legacy_cut')
    const say = tpl.slice(at, at + 200)
    expect(say).toContain('上一版')
    expect(say).toContain('重新合成')
  })

  it('那几段每一段都点得开，不是只有第一段', () => {
    expect(tpl).toContain('v-for="f in files"')
    expect(tpl).toContain('@click="playing = f.rel"')
  })
})

/**
 * 跨语言的那一条（CLAUDE.md：收不动就让它会响）。
 *
 * 页面敢显示「—」，前提是引擎那头真的会回 null。哪天有人把那几栏改回
 * `0`，回包里就再没有"不知道"这个意思，而这一页会把 0 照实显示成
 * 「0 秒 · 0 镜」——页面这半边一条用例都不会红。所以直接读引擎的源码。
 */
describe('引擎那头还在回 null', () => {
  const cpp = fs.readFileSync(
    fileURLToPath(new URL('../../../../cpp/src/http/film.cpp', import.meta.url)),
    'utf8',
  )

  it('GET /api/film 那几栏的默认值是 nullptr，不是 0', () => {
    expect(cpp).toContain('{"total_s", nullptr}')
    expect(cpp).toContain('{"shots", nullptr}')
    expect(cpp).toContain('{"name", nullptr}')
  })

  it('legacy_cut 这一栏还在回', () => {
    expect(cpp).toContain('{"legacy_cut"')
  })
})
