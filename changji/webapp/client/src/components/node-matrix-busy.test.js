/**
 * 那张「机器 × 能力」的表上，**本机那一行的「忙」一次都没亮过**。
 *
 * 三颗徽章原来串在一条链上：
 *
 *     <span v-if="n.local">本机</span>
 *     <span v-else-if="!n.online">连不上</span>
 *     <span v-else-if="n.busy">忙</span>
 *
 * 本机那一行 `n.local` 恒真，链子到第一个分支就断了。而本机恰恰是最常在
 * 跑的那一台——引擎每次都老老实实算了 `local_exec().busy()` 发过来
 * （node_registry.cpp 的 `local_node`），界面接着就扔了。
 *
 * 表现是：八张卡都在出片，那张表上一台「忙」都没有，看着像全闲着。
 *
 * 「本机」是**身份**，「连不上／忙」是**状态**，两件事不能共用一条链。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fs.readFileSync(
  fileURLToPath(new URL('./NodeMatrix.vue', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

describe('那张表上的徽章', () => {
  const body = code(SRC)

  it('「本机」自己一条 v-if，不把后面两颗吃掉', () => {
    const at = body.indexOf('本机</span>')
    expect(at, '找不到「本机」那颗').toBeGreaterThan(0)
    const line = body.slice(body.lastIndexOf('<span', at), at)
    expect(line).toContain('v-if="n.local"')

    // 紧跟着那颗必须是 v-if，不是 v-else-if——是 v-else-if 的话本机这一行
    // 就再也走不到后面了。
    const after = body.slice(at, at + 200)
    expect(after, '「连不上」又串回本机那条链上了').toContain(
      '<span v-if="!n.online"',
    )
    expect(after).not.toContain('v-else-if="!n.online"')
  })

  it('「忙」接在「连不上」后面：连不上的时候不说忙', () => {
    const at = body.indexOf('n.busy')
    expect(at).toBeGreaterThan(0)
    const line = body.slice(body.lastIndexOf('<span', at), at + 40)
    expect(line).toContain('v-else-if')
  })

  it('第一次还没问回来的时候，说一声「问着」', () => {
    // 挨个去连，每台最多等 3 秒。这段时间里整块只剩标题和底下那句说明，
    // 看着像"就本机一台"——而真是那样的话，表里至少还有本机那一行。
    expect(body).toMatch(/v-if="loading && !data"/)
  })

  it('配置关的格子和你关的格子，长得要不一样', () => {
    // 引擎特意分了这两种（NodeState::off_locked 的注释：「两种显示成一样
    // 的话，用户会在一个点不动的格子上反复点」）。原来这儿只拿 locked 去
    // disable 按钮——屏幕上两种一模一样，差别只有鼠标形状和悬停提示。
    const at = body.indexOf('function cellClass(')
    expect(at, 'cellClass 挪走了？').toBeGreaterThan(0)
    const fn = body.slice(at, body.indexOf('function cellTitle('))
    expect(fn, 'cellClass 没看 locked').toContain('cap.locked')
    expect(fn).toContain('cell--locked')
    // 样式真的存在，不是个没人定义的类名
    expect(body).toMatch(/\.cell--locked\s*\{/)
  })

  it('忙不忙是引擎算的，界面不自己推', () => {
    // 界面这头没有任何"根据别的字段推出忙"的算法：只认 n.busy。
    expect(body).not.toMatch(/busy\s*=\s*computed/)
    expect(body).not.toMatch(/function\s+\w*[Bb]usy\s*\(/)
  })
})
