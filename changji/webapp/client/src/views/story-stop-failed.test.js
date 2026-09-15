/**
 * 故事页那颗「停」，**请求砸了的时候要把那面旗收回来**。
 *
 * 引擎把「已手动停止」写进 job 级的 error，而下一拍的 `announceFatal`
 * 见 error 就弹红字——自己按的停不该再红一次。所以这一路是"先打招呼
 * （`writer.markStopped()`）再发请求"，那个顺序本身是对的：招呼晚了，
 * 下一拍就已经把红字弹出去了。
 *
 * 漏的是另一头：**停没发出去的时候，那面旗还立着**。而它是一次性的
 * （`announceFatal` 里 `if (stoppedByHand) { stoppedByHand = false; return }`）
 * ——于是接下来那趟**真的**炸了的时候（盘满了、引擎半路重启），那一条
 * 会被当成"自己按的停"吃掉，屏幕上一个字都没有。而按下去那一刻活儿压根
 * 没停，还在写。
 *
 * 还有一处要紧：判据不能只看 `run()` 的回值。它那道去重闸（同 key 正在跑
 * 就不发了）挡住之后回的也是 undefined，和"请求砸了"分不出来——连点两下
 * 「停」的话，第二下会把第一下立的旗擦掉。所以先自己用 `isBusy` 挡一道。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const VIEW = fs.readFileSync(
  fileURLToPath(new URL('./StoryView.vue', import.meta.url)),
  'utf8',
)
const STORE = fs.readFileSync(
  fileURLToPath(new URL('../stores/run.js', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const view = code(VIEW)
const fn = view.slice(
  view.indexOf('async function stopWriting()'),
  view.indexOf('</script>'),
)

describe('故事页按「停」没停成', () => {
  it('找得到那个函数', () => {
    expect(fn.length, 'stopWriting 挪走了？').toBeGreaterThan(60)
  })

  it('招呼还是先打的：markStopped 在发请求之前', () => {
    // 晚了的话下一拍已经把红字弹出去了。这一条是原来就对的，别改回去。
    const mark = fn.indexOf('writer.markStopped()')
    const send = fn.indexOf('api.stopSeries()')
    expect(mark).toBeGreaterThan(-1)
    expect(mark, 'markStopped 跑到发请求后面去了').toBeLessThan(send)
  })

  it('砸了要把旗收回来', () => {
    expect(fn).toMatch(/markStopped\(false\)/)
    const send = fn.indexOf('api.stopSeries()')
    expect(fn.indexOf('markStopped(false)')).toBeGreaterThan(send)
  })

  it('连点两下自己先挡一道，别靠 run() 那道去重闸', () => {
    // 那道闸回的也是 undefined，和"请求砸了"分不出来：第二下会把第一下
    // 立的旗擦掉，于是引擎那条「已手动停止」照样红一次。
    const guard = fn.indexOf("isBusy('stopWrite')")
    expect(guard, '没有自己那道闸').toBeGreaterThan(-1)
    expect(guard, '挡的那一道得在打招呼之前').toBeLessThan(fn.indexOf('writer.markStopped()'))
  })

  it('store 那头收得下这个参数', () => {
    const store = code(STORE)
    expect(store).toMatch(/function markStopped\(on = true\)/)
    expect(store).toMatch(/stoppedByHand = on/)
  })
})
