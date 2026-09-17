/**
 * 一键出图要按池里的位置数并发，不是一张一张来。
 *
 * 出参考图走的是出首帧那个池（server.cpp 的 set_ref_renderer 拿的就是
 * `Backends::frame`）。这一圈原来是 `for` 里一张一张 `await`——池里两个
 * 位置也只喂一个，一台双卡机从头到尾只有一张卡在动，22 张图 11 分钟而
 * 不是 6 分钟。用户 2026-09-17：「一键出图也没用多 gpu」。
 *
 * **位置数必须问引擎**，不能拍脑袋定：单卡机上并发只会让每张都更慢、
 * 还容易爆显存（两个上下文同时占显存正是 6GB 卡上跑不动的原因）。
 * 问来的数在单卡机上就是 1，行为和以前一样。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const code = fs
  .readFileSync(fileURLToPath(new URL('../AssetsView.vue', import.meta.url)), 'utf8')
  .replace(/<!--[\s\S]*?-->/g, '')
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/^\s*\/\/.*$/gm, '')

describe('一键出图按位置数并发', () => {
  it('位置数问引擎，不是写死的', () => {
    expect(code).toContain('async function frameLanes()')
    expect(code, '没问机器表').toContain('api.nodes()')
    // 判据和引擎挑位置那套一致：在线 + 能出首帧 + 没被关掉
    expect(code).toContain("c.cap === 'frame'")
    expect(code).toContain('node.online')
    expect(code, '没按每台报的槽数累加').toContain('node.slots')
  })

  it('问不到就退回 1，不乱开并发', () => {
    const at = code.indexOf('async function frameLanes()')
    const fn = code.slice(at, at + 600)
    expect(fn).toContain('return 1')
  })

  it('几条并排领活，不是固定分片', () => {
    expect(code).toContain('Promise.all(')
    expect(code, '没有"谁空了谁领下一张"那个游标').toMatch(/const i = next\s*\n\s*next \+= 1/)
  })

  it('中间砸了不再开新的，但已经在画的让它画完', () => {
    const at = code.indexOf('const lane = async ()')
    const fn = code.slice(at, at + 260)
    expect(fn, '没认停止标记').toContain('stoppedAt >= 0')
  })
})
