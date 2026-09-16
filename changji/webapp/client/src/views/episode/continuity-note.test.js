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

describe('接戏那一句：模型不写了，界面还摆着', () => {
  /**
   * ⚠️ **这一条 2026-09-17 反过来了：模型现在不写它。**
   *
   * 原来钉的是「提示词里教模型写接戏」。而 0d2c984（用户的提交）把
   * continuity_notes 连同它的提示词规则一起从 schema 里删了，理由写在
   * prompts.toml 第 172 行：「没人读的字段…连同它们的规则一起删」。
   *
   * 于是这条用例从那天起一直是红的——**而红了不改比不写更糟**：一个永远
   * 红的用例会把人训练成"看见红的先跳过"。
   *
   * 钉住现在的真相，并且把那句判断错在哪儿写下来：**「没人读」对这一栏
   * 不成立**，它一路读到了界面上（下面三条还在绿着）。所以现在是半拆：
   * 引擎照发、界面照显示，而内容只可能来自存量数据（hulian-test 的
   * 159 镜里还剩 14 镜有，新出的镜头一律为空）。
   *
   * 两条路二选一，等人定：把这一栏和规则一起放回 schema，或者把下面那条
   * 链也拆掉。**保持现状的代价是界面上一格永远空着的「接戏」。**
   */
  it('模型已经不被要求写它了——这一格只会有存量数据', () => {
    expect(PROMPTS, 'continuity_notes 的规则又回到提示词里了？那把这条用例也翻回去')
      .not.toMatch(/前后镜保持一致/)
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
