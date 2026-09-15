/**
 * 「接戏」那一句：模型专门写给看片的人的，而它一直到不了屏幕。
 *
 * 提示词第 16 条在教模型写它（「记录需要与前后镜保持一致的细节，比如道具
 * 在哪只手」），schema 里有、`validate()` 查它不超 200 字、存进 shots.json、
 * `/api/shots` 也原样发过来——然后在浏览器里被丢掉。整条链只差最后一行。
 *
 * **这个坑特别会骗人**：在引擎里 grep `continuity_notes` 会看到 readonly.cpp
 * 有一处"在读"，于是以为链是通的。shot.hpp 里那段注释当时就把这件事记下来
 * 了，这个用例是那段注释的执行版。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SHOTS = fs.readFileSync(
  fileURLToPath(new URL('./EpShots.vue', import.meta.url)),
  'utf8',
)
const READONLY = fs.readFileSync(
  fileURLToPath(new URL('../../../../../cpp/src/http/readonly.cpp', import.meta.url)),
  'utf8',
)
const PROMPTS = fs.readFileSync(
  fileURLToPath(new URL('../../../../../cpp/prompts.toml', import.meta.url)),
  'utf8',
)

function code(text) {
  return text.replace(/<!--[\s\S]*?-->/g, '')
}

describe('接戏那一句要摆到屏幕上', () => {
  it('模型确实被要求写它', () => {
    expect(PROMPTS, '提示词里不教写接戏了？').toMatch(/前后镜保持一致/)
  })

  it('引擎确实发过来了', () => {
    expect(READONLY).toMatch(/"continuity_notes"/)
  })

  it('镜头抽屉里摆出来了', () => {
    expect(code(SHOTS), '抽屉里没读 continuity_notes').toMatch(
      /openShot\.continuity_notes/,
    )
  })

  it('只读，不给编辑框', () => {
    // 它是模型对这一镜的观察，不是参数：改了没有下游会读，
    // 而一个能改却没人读的框比不摆更糟。
    expect(code(SHOTS), 'continuity_notes 不该进 draft').not.toMatch(
      /draft\.continuity_notes/,
    )
  })
})
