/**
 * 体检没过的时候页面要自己再查，不能等人按「重新体检」。
 *
 * 2026-09-15：远程 worker 重启那几十秒里进这一页，「只出首帧」「出片」
 * 灰着，worker 起来了也不会自己亮。灰着就每 15 秒问一次；亮了、停用、
 * 卸载都要停，不然一个看不见的定时器一直往引擎打体检（一趟最坏二十多秒）。
 *
 * 同这一目录别的用例：读源码验形状，不起组件。
 */
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'

const body = readFileSync(
  fileURLToPath(new URL('./EpShots.vue', import.meta.url)),
  'utf8',
)

describe('体检没过自动再查', () => {
  it('can_run 变化就同步定时器', () => {
    expect(body).toMatch(/watch\(\(\) => doctor\.value\?\.can_run, syncRecheck\)/)
    expect(body).toMatch(/setInterval\(loadDoctor, RECHECK_MS\)/)
  })

  it('亮了就停：need 为假时 stopRecheck', () => {
    const at = body.indexOf('function syncRecheck()')
    const fn = body.slice(at, body.indexOf('\n}\n', at))
    expect(fn).toMatch(/if \(!need\) stopRecheck\(\)/)
  })

  it('停用和卸载都停，回来先查一遍', () => {
    const un = body.slice(body.indexOf('onUnmounted(() => {'))
    expect(un.slice(0, un.indexOf('})'))).toMatch(/stopRecheck\(\)/)
    const de = body.slice(body.indexOf('onDeactivated(() => {'))
    expect(de.slice(0, de.indexOf('})'))).toMatch(/stopRecheck\(\)/)
    const ac = body.slice(body.indexOf('onActivated(() => {'))
    expect(ac.slice(0, ac.indexOf('})'))).toMatch(/loadDoctor\(\)/)
  })
})
