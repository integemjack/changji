/**
 * 场次头这条判据，前端和引擎**逐条对得上**。
 *
 * 为什么值得单写一条用例：这不只是显示。按场分镜就是按它切的
 * （`stages/script.cpp` 的 `is_scene_header` → `parse_scene_header`），
 * 而阅读视图按同一条规则画分场线。两边对不上时**不报错**，只是页面上
 * 那条线和引擎真正切的位置不是一回事——而人以为自己看见的就是要发生的。
 *
 * 这条规则已经分叉过四次（ScriptReader 里那段注释一条条记着）：分隔符
 * 可省、半角逗号、内层【】、以及首尾空白的定义。所以这儿不靠眼睛比，
 * 直接把两边的源码读出来对。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const VUE = fileURLToPath(new URL('../components/ScriptReader.vue', import.meta.url))
const CPP = fileURLToPath(new URL('../../../../cpp/src/stages/script.cpp', import.meta.url))

/** 把 ScriptReader 里那条正则和那个 asciiTrim 取出来，真的跑一遍。 */
function frontend() {
  const src = fs.readFileSync(VUE, 'utf8')
  const rm = src.match(/^const SCENE = (\/.*\/)$/m)
  if (!rm) throw new Error('ScriptReader 里找不到 SCENE 那条正则')
  const tm = src.match(/^const asciiTrim = \(s\) => s\.replace\((\/.*\/g), ''\)$/m)
  if (!tm) throw new Error('ScriptReader 里找不到 asciiTrim')
  // 从字面量文本造 RegExp，不用 eval：`/…/g` → source 和 flags 分开
  const lit = (text) => {
    const at = text.lastIndexOf('/')
    return new RegExp(text.slice(1, at), text.slice(at + 1))
  }
  const re = lit(rm[1])
  const trimRe = lit(tm[1])
  /** 回 null（不是场次头）或者 { index, body }，和引擎那个函数一个形状。 */
  return (line) => {
    const m = re.exec(line.replace(trimRe, ''))
    return m ? { index: Number(m[1]), body: (m[2] ?? '').trim() } : null
  }
}

/** 引擎 parse_scene_header 认的那几个分隔符。 */
function engineSeparators() {
  const src = fs.readFileSync(CPP, 'utf8')
  const at = src.indexOf('bool parse_scene_header(')
  if (at < 0) throw new Error('script.cpp 里找不到 parse_scene_header')
  const body = src.slice(at, src.indexOf('\n}', src.indexOf('return true;', at)))
  const m = body.match(/for \(const char\* sep : \{([^}]*)\}\)/)
  if (!m) throw new Error('parse_scene_header 里找不到分隔符那张表')
  return m[1].match(/"([^"]+)"/g).map((x) => x.slice(1, -1))
}

describe('场次头判据', () => {
  const hit = frontend()

  it('七个分隔符都吃掉，正文里不留它', () => {
    const seps = engineSeparators()
    // 取到的确实是七个，不是空转
    expect(seps.length).toBe(7)
    for (const sep of seps) {
      expect(hit(`【第1场${sep}天台】`), `分隔符 ${JSON.stringify(sep)}`)
        .toEqual({ index: 1, body: '天台' })
    }
  })

  it('**斜杠不是分隔符**，留在正文里——旧正则曾经把它吃掉', () => {
    // 引擎实测：body=[/天台]。注意它照样算场次头（分隔符本来就可省），
    // 差别只在正文，所以这条必须比 body，光比"是不是场次头"看不出来。
    expect(engineSeparators()).not.toContain('/')
    expect(hit('【第1场/天台】')).toEqual({ index: 1, body: '/天台' })
  })

  it('分隔符可以省：只空一格、甚至不空，都认', () => {
    expect(hit('【第2场 夜 内 天台】')).toEqual({ index: 2, body: '夜 内 天台' })
    expect(hit('【第3场天台】')).toEqual({ index: 3, body: '天台' })
  })

  it('里面再出现【】就不是场次头', () => {
    expect(hit('【第1场 · a】b【c】')).toBe(null)
  })

  it('**首尾空白只按 ASCII 算**——和引擎的 strip_ascii 一样', () => {
    // 修的就是这一条：JS 的 .trim() 连全角空格一起去，引擎不去。
    // 行首一个全角空格的场次头，引擎不认，这儿也必须不认。
    const wide = '　'
    const nbsp = ' '
    expect(hit('  【第5场 · 天台】  ')).toEqual({ index: 5, body: '天台' }) // ASCII 空白照去
    expect(hit('\t【第5场 · 天台】\n')).toEqual({ index: 5, body: '天台' })
    expect(hit(wide + '【第6场 · 天台】')).toBe(null)
    expect(hit('【第7场 · 天台】' + wide)).toBe(null)
    expect(hit(wide + wide + '【第8场 · 天台】')).toBe(null)
    expect(hit(nbsp + '【第9场 · 天台】')).toBe(null)
  })

  it('数字和「场」是必须的', () => {
    expect(hit('【第场 · 天台】')).toBe(null)
    expect(hit('【第1幕 · 天台】')).toBe(null)
    expect(hit('【天台】')).toBe(null)
  })
})
