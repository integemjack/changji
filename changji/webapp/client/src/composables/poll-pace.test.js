/**
 * 两处轮询都要跟着 WebSocket 调档：连上放慢，断了拉回来。
 *
 * 2026-09-17 从引擎日志量的（ep03 那一轮 53 分钟）：
 *
 *     /api/run     1757 次   每 1.8 秒一拍，每次带最多 200 条事件
 *     /api/shots    413 次   每 7 秒一拍，每次 14 KB
 *
 * 而那一整轮 WS 一直连着——事件是推过来的，定时器只是兜底。兜底该慢。
 *
 * **两个方向都要接**：只在连上时调慢的话，断线之后会一直停在慢档，
 * 而那正是它最需要顶班的时候（标签页在后台还会被 Chrome 再压一道）。
 *
 * 按源码钉：真定时器行为要跑满几十秒才看得出，不值得在用例里等。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const read = (rel) =>
  fs.readFileSync(fileURLToPath(new URL(rel, import.meta.url)), 'utf8')

describe('轮询跟着 WS 调档', () => {
  it('镜头墙：连上放慢，断了拉回来', () => {
    const src = read('./useShots.js')
    expect(src).toContain('runStore.live ? SLOW_MS : FAST_MS')
    // 断开那一头：光监听 running 是不够的
    expect(src, '没监听 live 变化').toMatch(/watch\(\(\) => runStore\.live/)
  })

  it('出片状态：连上放慢，断了拉回来', () => {
    const src = read('../stores/run.js')
    const head = src.slice(0, src.indexOf('let timer = null', src.indexOf('let timer = null') + 10))
    expect(head).toContain('live.value ? SLOW_POLL_MS : POLL_MS')
    expect(head).toMatch(/live\.value = false\s*\n\s*retime\(\)/)
  })
})
