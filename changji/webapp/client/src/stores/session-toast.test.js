/**
 * 读不到项目那条红字，**一场失败只该说一次**。
 *
 * `session.refresh()` 砸了会 `dispatchEvent('changji:error')`，那儿原来的
 * 注释写着「不怕刷屏：refresh 是边沿触发的，没有任何一处在轮询它」。
 * 边沿触发这件事是真的，但砸了之后还有一条会自己再来一趟——App.vue 那句
 *
 *     useRetryWhenBack(() => session.failed, () => session.refresh())
 *
 * 它盯的是系统表从 null 变成非空。而**页面第一次读就砸了的时候，这个跳变
 * 必然会发生一次**：第一份系统表到手就是一次 null → 非空（useSystemFeed
 * 里那个 composable 自己的注释也点了这件事）。
 *
 * 浏览器里实测过：把 /bff/flow 打成 500，进项目页，两条一模一样的红字，
 * 相隔约半秒——第一条是那一趟读，第二条是第一份系统表到手之后的重试。
 *
 * 重试本身是对的（引擎重启就是这么救回来的），不对的是**重试失败也再喊
 * 一遍**。`failed` 正好是"上一趟砸了而且还没成功过"，拿它当闸。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fs.readFileSync(
  fileURLToPath(new URL('./session.js', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const body = code(SRC)

describe('读不到项目那条红字', () => {
  it('挡在 failed 后面：同一场失败不喊第二遍', () => {
    const at = body.indexOf("'changji:error'")
    expect(at, "那条 dispatchEvent 挪走了？").toBeGreaterThan(0)
    // 往前找最近的那道闸
    const before = body.slice(Math.max(0, at - 300), at)
    expect(before, 'dispatch 没挡在 failed 后面').toMatch(/if\s*\(!failed\.value\)/)
  })

  it('成功那一支要把 failed 清掉，不然以后一句都不说了', () => {
    // 这是上面那道闸能成立的前提：闸是"这一场"，不是"永远"。
    expect(body).toMatch(/failed\.value = false/)
    const ok = body.indexOf('failed.value = false')
    const bad = body.indexOf('failed.value = true')
    expect(ok).toBeGreaterThan(0)
    expect(bad).toBeGreaterThan(ok)
  })

  it('闸只管喊话，不许把"过期那一趟"那道闸也一起改了', () => {
    // mine() 那道是防串台的（上一部剧的报错清掉这一部的数据），
    // 和刷屏是两件事，动它会把那个坑放回来。
    expect(body).toMatch(/if \(!mine\(\)\) return/)
  })
})
