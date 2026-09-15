/**
 * 镜头页那个「停下」，**请求砸了的时候不许动任何本地状态**。
 *
 * 原来这个函数是"先收拾现场，再发请求"：
 *
 *     if (runStore.running) stoppedByHand = true
 *     const queued = waiting.value.size
 *     waiting.value = new Map()          // 排着的那几镜，清了
 *     try { await api.stopRun() } catch (err) { ui.error(err.message) }
 *
 * 请求砸了（引擎抽一下、500、网断）的时候，引擎**还在跑**，而这一页已经：
 *
 *   · 把排着的那几镜扔了。它们只活在这个 Map 里，一个字都没发给引擎——
 *     人排了 5 镜，按一下没停成的「停下」，那 5 镜凭空没了，屏幕上只有
 *     一句原始报错，不会提它们一个字。
 *   · 留着 `stoppedByHand`。那个旗是用来吃掉"按停之后不该弹的那句
 *     「这一轮跑完了」"的；停没成功的话，它会去吃下一轮**真正跑完**的
 *     那一句——这正是它自己上面那段注释在防的事。
 *
 * 顺序还有一处要紧：`wasRunning` 必须在 await 之前取。等回来再看的话引擎
 * 可能已经停了、`running` 是假，旗就立不上，那句提示照样弹。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fs.readFileSync(
  fileURLToPath(new URL('./useShots.js', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const body = code(SRC)
const fn = body.slice(
  body.indexOf('async function stop()'),
  body.indexOf('function flushWaiting()'),
)

describe('按「停下」没停成的时候', () => {
  it('找得到那个函数', () => {
    expect(fn.length, 'stop() 挪走了？').toBeGreaterThan(80)
  })

  it('排队那个 Map 在请求**成功之后**才清', () => {
    const send = fn.indexOf('await api.stopRun()')
    const clear = fn.indexOf('waiting.value = new Map()')
    expect(send).toBeGreaterThan(0)
    expect(clear, '还是先清队列再发请求').toBeGreaterThan(send)
  })

  it('那个旗也在请求成功之后才立', () => {
    const send = fn.indexOf('await api.stopRun()')
    const flag = fn.indexOf('stoppedByHand = true')
    expect(flag, '还是先立旗再发请求').toBeGreaterThan(send)
  })

  it('但"刚才在不在跑"要在发请求之前取', () => {
    const grab = fn.indexOf('const wasRunning')
    const send = fn.indexOf('await api.stopRun()')
    expect(grab).toBeGreaterThan(0)
    expect(grab, 'await 之后再看 running 就晚了').toBeLessThan(send)
  })

  it('砸了就地返回，不往下走', () => {
    const at = fn.indexOf('catch')
    expect(at).toBeGreaterThan(0)
    const tail = fn.slice(at, fn.indexOf('waiting.value = new Map()'))
    expect(tail, 'catch 里没有 return，后面那几句照样跑').toMatch(/\breturn\b/)
  })

  it('那句报错要说清是"没停下来"', () => {
    expect(fn).toMatch(/ui\.error\(`没停下来/)
  })
})
