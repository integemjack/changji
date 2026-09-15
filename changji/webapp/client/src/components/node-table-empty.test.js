/**
 * **一个 200 + 空响应体不该把整张设置页干掉。**
 *
 * api 那层拿到空响应体时回的是 `{}`（api/index.js 的 `return data ?? {}`，
 * 那是给不看返回值的调用方兜底的，不是这儿的错）。而这张表原来判的是
 * `v-if="data"` —— `{}` 是真值，于是放行，紧接着 `data.nodes[0]` 抛
 *
 *     Cannot read properties of undefined (reading '0')
 *
 * 整张设置页被 ErrorBoundary 换成一张崩溃卡。
 *
 * **不是假想的**：这一块每 15 秒问一次，引擎重启那一下正好落在窗口里，
 * 2026-09-15 实测撞到。
 *
 * 两条判据，缺一不可：
 *   一、模板判的是"有没有这份列表"，不是"有没有拿到响应对象"；
 *   二、形状不对的那一份**不许装进去**——装了的话表会凭空消失一轮，
 *       不崩，但屏幕上那张表没了，而它上一秒还好好的。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fs.readFileSync(
  fileURLToPath(new URL('./NodeMatrix.vue', import.meta.url)),
  'utf8',
)
/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
const code = SRC.replace(/\/\*[\s\S]*?\*\//g, '').replace(/<!--[\s\S]*?-->/g, '')

describe('机器表遇到空响应', () => {
  it('判的是有没有列表，不是有没有响应对象', () => {
    // data.nodes[0] 那一行只要还被裸的 v-if="data" 罩着就会重演。
    expect(code, '表还挂在 v-if="data" 上').not.toMatch(/v-if="data"/)
    expect(code, '没有按列表判').toMatch(/v-if="data\?\.nodes"/)
  })

  it('形状不对的那一份不装进去', () => {
    expect(code, '没有 setTable 这道闸').toMatch(/function setTable\(/)
    // 闸门本身要真的查 nodes，不能只是改个名字。
    const fn = code.slice(code.indexOf('function setTable('))
    expect(fn.slice(0, 160), 'setTable 没查 nodes').toMatch(/next\?\.nodes/)
    // 五条改表的路都要走它，不许有人绕过去直接赋值。
    expect(code, '还有地方直接给 data.value 赋值').not.toMatch(
      /data\.value = await api\./,
    )
  })
})

/**
 * **速度掉到近零时，那个「还要多久」不能照印。**
 *
 * 引擎算的是 剩余字节 / 当前速度，而速度是瞬时值——重启引擎、网络抖一下、
 * 一个文件刚下完还没接上下一个，它都会短暂掉到接近 0，商就炸了。
 * 2026-09-15 实测截到过「还要 533374 小时 59 分」，同一行的速度是 0.0 MB/s。
 */
describe('下载进度那行读数', () => {
  const fn = code.slice(code.indexOf('function dlSummary('))
  const body = fn.slice(0, fn.indexOf('\n}'))

  it('速度太低就不报剩余时间', () => {
    expect(body, '没有低速闸').toMatch(/speedBps\s*>\s*kSlow/)
  })

  it('算出来离谱的也不报', () => {
    expect(body, '没有上限闸').toMatch(/etaSeconds\s*<\s*kTooLong/)
  })
})

/**
 * **下砸了必须在那一行看得见。**
 *
 * 「正在下 N%」那颗 2026-09-15 加了，失败这一档当时漏了——提示只写在抽屉
 * 里，而抽屉默认收着。实测：基础版模型下到 102 GB 报 failed，那一行什么
 * 都不显示、`下 99%` 也一起消失，屏幕上看起来就是下完了。
 */
describe('下载失败', () => {
  it('行上要有失败标，不能只写在抽屉里', () => {
    expect(code, '行上没有失败标').toMatch(/state === 'failed'[\s\S]{0,200}下载失败/)
  })

  it('失败标要说清怎么办', () => {
    const at = code.indexOf("state === 'failed'")
    expect(code.slice(at, at + 400), '没告诉人下一步做什么').toMatch(/重来一次|点「模型」/)
  })
})
