/**
 * 参考图那条固定频道（`refs`），**没有 WebSocket 时的退路**。
 *
 * 用户 2026-09-12 报的是「我刷新了这个页面，正在生成的图就不会实时更新」，
 * 修法是引擎往一条名字固定的频道上也播一份。而那条频道是**纯 socket** 的：
 * 代理把 Upgrade 掐了、页面挂在反向代理后面，设定页上那几格就从头到尾一动
 * 不动——没有进度、没有半成品小图，画完了也不会把新图换上来。
 *
 * 顶栏那份系统表有 REST 的那一份（`/api/system`，和推过去的同一个函数拼的），
 * 引擎那边每一行现在带着 `target`，就够认出"这一格在画"了。
 *
 * 两条最容易写错、而且错了只是"一动不动"的：
 *
 *   一、**判据不能是 `sock`**。`openJobSocket` 当场就回一个对象，连不上是
 *       几毫秒之后 onerror 才知道；断线之后还会排五秒重连，那五秒里它也非空。
 *       拿它当"有 socket"的话退路几乎永远不跑——第一版就是这么写的，
 *       浏览器里那一格照旧是死的。
 *   二、**引擎两头都要有 target**。少一头（比如只有推送那份有）的表现是
 *       "有 WebSocket 时好好的，没有时一动不动"，最难联想到。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fileURLToPath(new URL('..', import.meta.url))
const read = (rel) => fs.readFileSync(path.join(SRC, rel), 'utf8')
const cpp = (rel) =>
  fs.readFileSync(fileURLToPath(new URL('../../../../cpp/' + rel, import.meta.url)), 'utf8')

describe('没有 WebSocket 时那几格还得动', () => {
  const feed = read('composables/useRefStream.js')

  it('退路挂在系统表上', () => {
    expect(feed).toContain('useSystemFeed')
    expect(feed).toContain('function fromJobs(')
    // 认的是那一行的 target
    expect(feed).toMatch(/row\??\.target/)
  })

  it('**判据是"真连上了"，不是"有没有那个对象"**', () => {
    expect(feed).toContain('wsOk')
    // 那条 watch 里不许再拿 sock 判
    const at = feed.indexOf('watch(')
    expect(at).toBeGreaterThan(0)
    const body = feed.slice(at, at + 300)
    expect(body).toContain('wsOk')
    expect(body).not.toMatch(/\(sock \?/)
  })

  it('画完了靠"这一行没了"认，而且要换代号', () => {
    const at = feed.indexOf('function fromJobs(')
    const body = feed.slice(at, at + 900)
    expect(body).toContain('forget(t)')
    expect(body).toContain('drawn[t]')
    expect(body).toContain('finished.value += 1')
  })

  it('断线时要把上一轮认过的清掉', () => {
    // 不清的话，退路接管的第一拍会把它们全当成"画完了"，三个格子各白拉一遍
    expect(feed).toMatch(/seen = new Set\(\)[\s\S]{0,200}retry = setTimeout/)
  })
})

describe('引擎那两头都要带 target', () => {
  it('短活那一行（出参考图走这条）', () => {
    // 这一族的账 2026-09-17 整个搬到了 `task_board.cpp`（三个状态收成一
    // 本，`Activity` 只剩一层壳）。这一栏跟着搬，**这条用例也跟着搬**，
    // 别留在只剩壳的那个文件上——那样它会一直绿着，而数据早不在那儿了。
    expect(cpp('src/pipeline/task_board.cpp')).toContain('{"target", row->target}')
    expect(cpp('src/pipeline/activity.cpp')).toContain('void Activity::set_target')
  })

  it('长跑任务那一行也要有这一栏，哪怕是空串', () => {
    expect(cpp('src/pipeline/jobs.cpp')).toContain('{"target", ""}')
  })

  it('出参考图时真的填了', () => {
    expect(cpp('src/http/ref_gen.cpp')).toContain('act.set_target(stem)')
  })
})
