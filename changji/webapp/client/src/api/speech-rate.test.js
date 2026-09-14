/**
 * 中文语速这个数，界面这份和引擎那份对不对得上。
 *
 * **为什么值得单写一条用例。** 这是一份**抄过来的**数：真身在
 * `cpp/prompts.toml` 的 `chars_per_second`，而同一件物理事实（一个人一秒
 * 念几个字）今天有三份拷贝——
 *
 *   · prompts.toml 的 `chars_per_second` → 决定**叫模型写多少字**
 *     （`budget_chars`）；
 *   · `cpp/src/stages/audio_plan.hpp` 的 `kCharsPerSecond` → 决定
 *     `estimate_speech_duration()` 算出来**每句实际多长**，那个数会反推
 *     回去锁死镜头时长；
 *   · 这一份 → 界面上「约 x 秒」和「偏短 / 合适 / 偏长」那颗丸子。
 *
 * 谁单独动一个，症状是**剧本长度看着正好、成片却长出一截**，而三处都不
 * 报错：预算按 A 算、真实时长按 B 算、屏幕上那句话按 C 算。
 *
 * 前两份在引擎里用 `static_assert` 钉死了（stages/script.cpp，改一个编不
 * 过）。第三份在浏览器里，编译器够不着，所以直接读那个 toml 比对——
 * 那边改了这条当场红。做法和 `act-labels.test.js` 一样。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const TOML = fileURLToPath(new URL('../../../../cpp/prompts.toml', import.meta.url))

/** 从 prompts.toml 里取一个顶层数值。 */
function engineNumber(key) {
  const src = fs.readFileSync(TOML, 'utf8')
  // 行首那一条才算，避免撞上注释里提到的同名字眼
  const m = src.match(new RegExp(`^${key}\\s*=\\s*([0-9.]+)\\s*$`, 'm'))
  if (!m) throw new Error(`prompts.toml 里找不到 ${key}`)
  return Number(m[1])
}

/**
 * ScriptReader 那个默认值。
 *
 * **从源码里读，不 import 组件**：这个仓库没装 jsdom 和
 * @vue/test-utils，`import` 一个 .vue 在这儿跑不起来。要盯的也正是
 * "写在 defineProps 里的那个字面量"，读源码反而更直接。
 */
function readerDefault() {
  const p = fileURLToPath(new URL('../components/ScriptReader.vue', import.meta.url))
  const src = fs.readFileSync(p, 'utf8')
  const m = src.match(/charsPerSecond:\s*\{[^}]*default:\s*([0-9.]+)/)
  if (!m) throw new Error('ScriptReader 里找不到 charsPerSecond 的默认值')
  return Number(m[1])
}

describe('中文语速', () => {
  it('界面那份和 prompts.toml 里那份是同一个数', () => {
    expect(readerDefault()).toBe(engineNumber('chars_per_second'))
  })

  it('这个数本身是合理的（别被手误改成 0 或者天文数字）', () => {
    // 0 会让「约 x 秒」变成 Infinity，整行读数烂掉；
    // 上下界按中文播报的常识留宽一点，只挡明显的手误。
    const v = engineNumber('chars_per_second')
    expect(v).toBeGreaterThan(1)
    expect(v).toBeLessThan(20)
  })

  it('对白占比也在 0 和 1 之间', () => {
    // budget_chars = 时长 × 语速 × 这个数。超过 1 就是"整集全是对白还不够"
    const share = engineNumber('dialogue_share')
    expect(share).toBeGreaterThan(0)
    expect(share).toBeLessThanOrEqual(1)
  })
})
