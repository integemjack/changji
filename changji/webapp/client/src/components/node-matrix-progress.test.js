/**
 * 那张机器表里，「装成和本机同一套」按下去之后的下载进度。
 *
 * 一趟要下几十 GB，几分钟到几十分钟。这中间问一次进度失败太正常了——
 * 引擎重启一下、网络抖一下就是一次。而那一问原来是
 *
 *     } catch {
 *       clearInterval(pollTimer)
 *     }
 *
 * **一次失败就永久停掉，而且一个字不说。** 之后那一格永远停在最后一次
 * 拿到的进度上（「正在下 3 个文件 41%」），不动、不报错——看着像那头卡死
 * 了，而那头多半正在好好地下。
 *
 * 要的是两件事：连着几次问不到才算真断，断了得说一句、并且给条回去的路。
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

const body = code(SRC)
const poll = body.slice(
  body.indexOf('function pollProgress('),
  body.indexOf('function gb('),
)

describe('下载进度这一问', () => {
  it('找得到那个函数', () => {
    expect(poll.length, 'pollProgress 挪走了？').toBeGreaterThan(100)
  })

  it('**一次问不到不许停**：要连着几次才算断', () => {
    expect(poll).toMatch(/misses/)
    // 光 catch 就 clearInterval 那一版不许回来：catch 里在 clearInterval
    // 之前必须先过一道计数的闸。
    const at = poll.indexOf('} catch')
    expect(at, 'catch 没了？').toBeGreaterThan(0)
    const tail = poll.slice(at)
    const gate = tail.indexOf('misses')
    const stop = tail.indexOf('clearInterval')
    expect(gate).toBeGreaterThan(0)
    expect(gate, '还是一 catch 就 clearInterval').toBeLessThan(stop)
  })

  it('问通了要把计数清零，不然攒够三次照样停', () => {
    const ok = poll.indexOf('await api.nodeSetupProgress')
    const at = poll.indexOf('} catch')
    // 从"问通了"那一行往后找，别撞上开头 `let misses = 0` 那个声明
    const reset = poll.indexOf('misses = 0', ok)
    expect(reset, '问通了没把计数清零').toBeGreaterThan(ok)
    expect(reset, '清零跑到 catch 里去了').toBeLessThan(at)
  })

  it('真断了要说一句，不许默默停掉', () => {
    const at = poll.indexOf('} catch')
    expect(poll.slice(at)).toContain('error.value =')
  })

  it('计数是每一趟自己的，不跨机器攒', () => {
    // 模块级的一个 misses 会让 A 机器攒的次数算到 B 头上。
    expect(body).not.toMatch(/^let misses/m)
    expect(poll).toMatch(/let misses = 0/)
  })
})
