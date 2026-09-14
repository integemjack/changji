/**
 * 两份**抄过来的枚举清单**，和引擎那边对不对得上。
 *
 * `SHOT_STATUS`（镜头状态）和 `STAGE_LABELS`（出片走到哪一步）都是把 C++
 * 的枚举名抄了一份、配上中文。抄的一方落后时**不报错**：
 *
 *   · 状态那份落后 → 镜头墙上那颗牌子显示的是引擎发来的原文
 *     （`statusOf` 的兜底是 `{ label: v, tone: 'neutral' }`），于是中文界面
 *     上冒出一个 `final_rejected`，而且 tone 是中性的——一个"没过闸门"的
 *     镜头看着和"未开工"一样平静。
 *   · 阶段那份落后 → 顶栏「AI 作业中」和镜头格上的那一行显示英文阶段名
 *     （`STAGE_LABELS[x.stage] || x.stage`）。
 *
 * 做法和 `act-labels.test.js` / `speech-rate.test.js` 一样：**直接读 C++
 * 源码里那个 switch**，engine 那边加一个枚举值、这条当场红。
 *
 * 只比**键**，不比中文。两边的中文本来就不一样，而且是故意的——引擎那份
 * （`models::status_zh`）进的是命令行摘要，写得开：「配音完成，还没出片」；
 * 界面那份进的是镜头格上一颗牌子，一行放不下，所以是「配音完成」。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

import { SHOT_STATUS, STAGE_LABELS, statusOf } from './labels.js'

/** 从一个 C++ `to_string` switch 里把 return 的字符串全取出来。 */
function enumStrings(relPath, fnSignature) {
  const p = fileURLToPath(new URL('../../../../' + relPath, import.meta.url))
  const src = fs.readFileSync(p, 'utf8')
  const at = src.indexOf(fnSignature)
  if (at < 0) throw new Error(`${relPath} 里找不到 ${fnSignature}`)
  // 到函数结尾那个 `return "?";` 为止，别把下一个函数也吃进来
  const end = src.indexOf('return "?";', at)
  if (end < 0) throw new Error(`${fnSignature} 的形状变了（没有兜底的 return "?"）`)
  const body = src.slice(at, end)
  return [...body.matchAll(/case\s+\w+::\w+:\s*return\s+"([a-z_]+)";/g)].map((m) => m[1])
}

describe('镜头状态', () => {
  const engine = () => enumStrings('cpp/src/models/shot.cpp', 'const char* to_string(ShotStatus v)')

  it('引擎认的每一个状态，界面都有中文', () => {
    const missing = engine().filter((s) => !(s in SHOT_STATUS))
    expect(missing).toEqual([])
  })

  it('界面没有多出引擎不发的状态（多出来的是死条目）', () => {
    const extra = Object.keys(SHOT_STATUS).filter((s) => !engine().includes(s))
    expect(extra).toEqual([])
  })

  it('取到的确实是那九个，不是空转', () => {
    // 正则没匹配上的话上面两条会双双通过（空数组 vs 空数组）
    expect(engine().length).toBe(9)
    expect(engine()).toContain('planned')
    expect(engine()).toContain('final_rejected')
  })

  it('每一条都有 label 和 tone，tone 只用约定的那几个', () => {
    for (const [k, v] of Object.entries(SHOT_STATUS)) {
      expect(v.label, k).toBeTruthy()
      expect(['neutral', 'info', 'ok', 'warn', 'bad'], k).toContain(v.tone)
    }
  })

  it('认不出来的状态按原文显示，不抛也不空着', () => {
    // 老界面配新引擎时走的就是这一支
    expect(statusOf('something_new').label).toBe('something_new')
    expect(statusOf(undefined).label).toBe(undefined)
  })
})

describe('出片阶段', () => {
  const engine = () =>
    enumStrings('cpp/src/pipeline/episode.cpp', 'const char* to_string(Stage s)')

  it('引擎发的每一个阶段名，界面都有中文', () => {
    const missing = engine().filter((s) => !(s in STAGE_LABELS))
    expect(missing).toEqual([])
  })

  it('界面没有多出引擎不发的阶段', () => {
    const extra = Object.keys(STAGE_LABELS).filter((s) => !engine().includes(s))
    expect(extra).toEqual([])
  })

  it('取到的确实是那五个，不是空转', () => {
    expect(engine().length).toBe(5)
    expect(engine()).toContain('assemble')
  })
})
