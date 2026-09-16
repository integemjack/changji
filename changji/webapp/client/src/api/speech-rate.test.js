/**
 * 阅读视图从引擎**抄过来的那几个数**，和真身对不对得上。
 *
 * ScriptReader 一共抄了四个：中文语速、对白占比、以及「偏短 / 偏长」那两
 * 条阈值。四个都是"引擎先算一遍、界面再算一遍"的那种数，抄的一方一旦落后，
 * 症状都是**同一份稿子两个说法**，而两边都不报错。
 *
 * ---- 语速 ----
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

/**
 * 从 ScriptReader 源码里抠一个写死的小数。
 *
 * 抠的是"那一行里第一个小数"，所以传进来的 anchor 要能定位到那一行。
 */
function readerNumber(anchor) {
  const p = fileURLToPath(new URL('../components/ScriptReader.vue', import.meta.url))
  const line = fs
    .readFileSync(p, 'utf8')
    .split('\n')
    .find((l) => l.includes(anchor) && !l.trimStart().startsWith('*') && !l.trimStart().startsWith('//'))
  if (!line) throw new Error(`ScriptReader 里找不到含「${anchor}」的代码行`)
  const m = line.match(/([0-9]*\.[0-9]+)/)
  if (!m) throw new Error(`「${anchor}」那一行里没有小数`)
  return Number(m[1])
}

describe('对白占比', () => {
  it('阅读视图里那个 0.62 和 prompts.toml 的 dialogue_share 是同一个数', () => {
    // 用在「这一段对白不够」那条提示上（一段的时长里对白占几成）。
    // 落后了的表现是：引擎按新的比重算预算，而这一页按旧的比重提醒，
    // 于是一段明明够了却一直挂着"不够"，或者反过来。
    expect(readerNumber('* 0.62 *')).toBe(engineNumber('dialogue_share'))
  })
})

/**
 * 「偏短 / 合适 / 偏长」那两条阈值。
 *
 * 真身在 `cpp/src/http/scripting.cpp` 的 `post_script_write`：
 * `chars > budget * 1.35` 是偏长、`chars < budget * 0.6` 是偏短。
 *
 * **两条路都会走到**：写出来还没采用的那一份，`fit` 是引擎算好送过来的
 * （EpScript 里 `:fit="draft.fit"`）；而**已经存下来的剧本没有这条**——
 * `/api/script` 不回 `fit`，那一份的丸子是这一页自己按抄来的阈值算的。
 * 抄的那份落后，同一份稿子在草稿面板上写「合适」、存下来之后写「偏短」。
 *
 * 这条读的是 C++ 源码而不是某个配置文件——那两个数就写在那儿，没有第二
 * 个源头。锚在 `out["fit"] = ... chars > budget *` 上，只有正片那一处是
 * 这个形状（预告片那一处的上界是 `chars > budget`，没有乘号）。
 *
 * 2026-09-16 放宽了一点：章模式在 `out["fit"] =` 和 `chars > budget *`
 * 之间插了一个三目（章模式没有字数预算，长度由内容定，不判长短）。
 * 锚点跳过中间那一段，两个阈值本身一个没动。
 */
describe('够不够的两条阈值', () => {
  const CPP = fileURLToPath(new URL('../../../../cpp/src/http/scripting.cpp', import.meta.url))

  function engineFit() {
    const src = fs.readFileSync(CPP, 'utf8')
    const m = src.match(
      /out\["fit"\]\s*=[\s\S]{0,120}?chars\s*>\s*budget\s*\*\s*([0-9.]+)[\s\S]{0,120}?chars\s*<\s*budget\s*\*\s*([0-9.]+)/,
    )
    if (!m) throw new Error('scripting.cpp 里找不到 post_script_write 那两条阈值')
    return { long: Number(m[1]), short: Number(m[2]) }
  }

  /** 界面那两个数写在同一行三目里，一次抠两个。 */
  function readerFit() {
    const p = fileURLToPath(new URL('../components/ScriptReader.vue', import.meta.url))
    const m = fs
      .readFileSync(p, 'utf8')
      .match(/r\s*<\s*([0-9.]+)\s*\?\s*'偏短'\s*:\s*r\s*>\s*([0-9.]+)\s*\?\s*'偏长'/)
    if (!m) throw new Error('ScriptReader 里找不到「偏短 / 偏长」那个三目')
    return { short: Number(m[1]), long: Number(m[2]) }
  }

  it('界面那两个数和引擎一致', () => {
    expect(readerFit()).toEqual(engineFit())
  })

  it('短的那条在长的那条下面（写反了整页反过来）', () => {
    const engine = engineFit()
    expect(engine.short).toBeLessThan(1)
    expect(engine.long).toBeGreaterThan(1)
  })
})
