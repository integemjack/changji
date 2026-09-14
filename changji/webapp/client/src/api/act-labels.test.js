/**
 * 四段段名这一份清单，和引擎那份对不对得上。
 *
 * **为什么值得单写一条用例。** 这是一份**抄过来的**清单：真身在
 * `cpp/prompts.toml` 的 `[script].act_labels` 里，而它已经悄悄长过一次——
 * 从四个（一组）长到二十个槽（五组戏的走法），前端这边没跟。后果不报错，
 * 只是剧本阅读视图把段头当成普通描写行画进正文，「几句、约几秒」那套四段
 * 读数整个消失；而 `/api/script/write` 在界面不送 `variation` 时会
 * `random_shape()` 现摇一组，**五组里有四组**用的都不是老那四个名字，
 * 也就是十份剧本里有八份是坏的。没人会因为"少了一行统计"去翻代码。
 *
 * 所以直接读那个 toml 比对：那边再加一组，这条当场红。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

import { ACT_LABELS } from './labels.js'

const TOML = fileURLToPath(new URL('../../../../cpp/prompts.toml', import.meta.url))

/** 从 prompts.toml 里把 `act_labels = [...]` 那一段的字符串抠出来。 */
function engineActLabels() {
  const src = fs.readFileSync(TOML, 'utf8')
  const at = src.indexOf('\nact_labels = [')
  if (at < 0) throw new Error('prompts.toml 里找不到 act_labels')
  const end = src.indexOf(']', at)
  const body = src.slice(at, end)
  // 注释行（# 0 步步紧逼）里没有引号，天然不会被抠进来
  return [...body.matchAll(/"([^"]+)"/g)].map((m) => m[1])
}

describe('四段段名', () => {
  it('引擎那份里的每一个名字，界面这份都有', () => {
    const engine = engineActLabels()
    // 二十个槽（五组 × 四段），里面有重名（「集尾留扣」四组都用）
    expect(engine.length % 4).toBe(0)
    expect(engine.length).toBeGreaterThanOrEqual(8)
    const missing = [...new Set(engine)].filter((x) => !ACT_LABELS.includes(x))
    expect(missing).toEqual([])
  })

  it('界面这份里没有引擎不认的名字', () => {
    // 反向也要管：多写一个的话，一行普通描写会被当成段头切一刀
    const engine = new Set(engineActLabels())
    expect(ACT_LABELS.filter((x) => !engine.has(x))).toEqual([])
  })

  it('这份清单自己不重复', () => {
    expect(ACT_LABELS.length).toBe(new Set(ACT_LABELS).size)
  })
})
